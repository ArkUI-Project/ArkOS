/* e1000 kernel driver: Intel 82540EM (PCI 8086:100e, QEMU e1000) as a .arco
 * loadable module. Original code using the Intel 8254x register/legacy
 * descriptor protocol — the same hardware contract the built-in net code
 * always used, now reached only through the ArkDriverHost table.
 *
 * The module owns the machine NIC: INIT probes the PCI bus, programs the DMA
 * rings and binds its ArkNetOps with host->net_attach, so the network stack
 * comes up as soon as the module loads (deferred bring-up inside net.c). All
 * DMA buffers live in .bss inside the module's own physical run; their bus
 * addresses come from host->phys_of, never from window VAs.
 *
 * Build:  python3 scripts/arco.py sdk/driver_e1000.c -o build/e1000.arco \
 *            --name e1000 --version 1.0.0
 * The kernel build embeds this image as the inbox driver; a manifest copy
 * under @drv.e1000 shadows it for development updates.
 */
#include "driver.h"

#define DRIVER_NAME "e1000"
#define RX_COUNT 64u
#define TX_COUNT 32u
#define BUFFER_SIZE 2048u
enum {
    CTRL = 0x0000,
    STATUS = 0x0008,
    EERD = 0x0014,
    ICR = 0x00c0,
    IMS = 0x00d0,
    IMC = 0x00d8,
    RCTL = 0x0100,
    TCTL = 0x0400,
    TIPG = 0x0410,
    RDBAL = 0x2800,
    RDBAH = 0x2804,
    RDLEN = 0x2808,
    RDH = 0x2810,
    RDT = 0x2818,
    TDBAL = 0x3800,
    TDBAH = 0x3804,
    TDLEN = 0x3808,
    TDH = 0x3810,
    TDT = 0x3818,
    MTA = 0x5200,
    RAL = 0x5400,
    RAH = 0x5404
};
typedef struct __attribute__((packed)) {
    uint64_t address;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
} RxDescriptor;
typedef struct __attribute__((packed)) {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset, command, status, checksum_start;
    uint16_t special;
} TxDescriptor;
_Static_assert(sizeof(RxDescriptor) == 16, "e1000 legacy RX descriptor size");
_Static_assert(sizeof(TxDescriptor) == 16, "e1000 legacy TX descriptor size");
static volatile RxDescriptor rx[RX_COUNT] __attribute__((aligned(128)));
static volatile TxDescriptor tx[TX_COUNT] __attribute__((aligned(128)));
static uint8_t rx_data[RX_COUNT][BUFFER_SIZE] __attribute__((aligned(4096)));
static uint8_t tx_data[TX_COUNT][BUFFER_SIZE] __attribute__((aligned(4096)));

static const ArkDriverHost *host;
static volatile uint32_t *registers;
static uint8_t nic_mac[6];
static unsigned rx_next, tx_next;
static bool ready, discard_fragment;
static int node = -1;
/* Byte deltas the poll callback has not reported to the device model yet. */
static uint64_t rx_bytes, tx_bytes, rx_drops;
/* Interrupt accounting: the ISR only acknowledges the device and counts
 * causes; the poll callback forwards the totals as device counters. */
static volatile uint32_t irq_seen, irq_overruns;
static int irq_line = -1;
/* Interrupt cause bits (ICR read clears them). */
#define CAUSE_LSC 0x04u    /* link status change */
#define CAUSE_RXDMT0 0x10u /* descriptor ring running low */
#define CAUSE_RXO 0x40u    /* receive overrun: ring could not keep up */
#define CAUSE_RXT0 0x80u   /* receive timer expired / packet waiting */

/* Freestanding runtimes: the image links no libc, and the compiler may emit
 * calls for structure copies even with -fno-builtin. */
void *memset(void *p, int value, size_t bytes) {
    uint8_t *out = (uint8_t *)p;
    while (bytes--)
        *out++ = (uint8_t)value;
    return p;
}
void *memcpy(void *dst, const void *src, size_t bytes) {
    uint8_t *out = (uint8_t *)dst;
    const uint8_t *in = (const uint8_t *)src;
    while (bytes--)
        *out++ = *in++;
    return dst;
}

static void fence(void) {
    __asm__ volatile("mfence" ::: "memory");
}
static uint32_t read_reg(unsigned offset) {
    return registers[offset / 4];
}
static void write_reg(unsigned offset, uint32_t value) {
    registers[offset / 4] = value;
}
/* Runs in interrupt context on the module stack: bounded, no host calls.
 * Reading ICR acknowledges every asserted cause in one shot. */
static void e1000_isr(void) {
    uint32_t cause = read_reg(ICR);
    if (!cause)
        return; /* shared line: another device asserted it */
    ++irq_seen;
    if (cause & CAUSE_RXO)
        ++irq_overruns;
}
static bool eeprom_word(unsigned word, uint16_t *value) {
    write_reg(EERD, 1u | (word << 8));
    for (unsigned tries = 0; tries < 100000; tries++) {
        uint32_t result = read_reg(EERD);
        if (result & 0x10) {
            *value = (uint16_t)(result >> 16);
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

/* ---- ArkNetOps callbacks -------------------------------------------------- */

static int nic_send(const uint8_t *frame, uint32_t length) {
    if (!ready || !frame || length < 14 || length > 1514)
        return 0;
    volatile TxDescriptor *descriptor = &tx[tx_next];
    if (!(descriptor->status & 1) || (tx_next + 1) % TX_COUNT == read_reg(TDH))
        return 0;
    fence();
    memcpy(tx_data[tx_next], frame, length);
    if (length < 60) {
        memset(tx_data[tx_next] + length, 0, 60 - length);
        length = 60;
    }
    tx_bytes += length;
    descriptor->length = (uint16_t)length;
    descriptor->checksum_offset = 0;
    descriptor->checksum_start = 0;
    descriptor->special = 0;
    descriptor->status = 0;
    descriptor->command = 0x0b; /* EOP | IFCS | RS, no checksum offload. */
    fence();
    tx_next = (tx_next + 1) % TX_COUNT;
    write_reg(TDT, tx_next);
    return 1;
}
static int nic_link(void) {
    return ready && (read_reg(STATUS) & 2) != 0;
}
static uint32_t nic_receive(uint8_t *frame, uint32_t capacity) {
    if (!ready || !frame)
        return 0;
    for (unsigned count = 0; count < RX_COUNT; count++) {
        volatile RxDescriptor *descriptor = &rx[rx_next];
        if (!(descriptor->status & 1))
            return 0;
        fence();
        unsigned length = descriptor->length;
        bool end = (descriptor->status & 2) != 0;
        bool valid = !discard_fragment && end && !descriptor->errors &&
                     length >= 14 && length <= 1514 && length <= capacity;
        if (valid) {
            memcpy(frame, rx_data[rx_next], length);
            rx_bytes += length;
        } else if (end && !discard_fragment) {
            ++rx_drops;
        }
        discard_fragment = !end;
        descriptor->status = 0;
        descriptor->errors = 0;
        descriptor->length = 0;
        fence();
        write_reg(RDT, rx_next);
        rx_next = (rx_next + 1) % RX_COUNT;
        if (valid)
            return length;
    }
    return 0;
}
static void nic_read_mac(uint8_t out[6]) {
    memcpy(out, nic_mac, 6);
}
/* Filled at INIT: a relocation-free image cannot carry static function
 * pointers, so the ops table is assembled in .bss at runtime. */
static ArkNetOps nic_ops;

/* ---- init ---------------------------------------------------------------- */

/* Translate one module-owned buffer into its DMA address. The module image
 * and stack occupy one contiguous kernel-pool physical run, so a nonzero
 * answer is a real bus address, not a window VA. */
static bool phys_of_checked(const void *va, uint64_t *out) {
    uint64_t pa = host->phys_of(va);
    if (!pa)
        return false;
    *out = pa;
    return true;
}

static uint32_t nic_bdf;
static int nic_hw_init(void) {
    if (host->pci_find(0x8086, 0x100e, &nic_bdf))
        return -19; /* no supported adapter on this machine */
    uint64_t base = 0;
    if (host->pci_bar_base(nic_bdf, 0, &base) || !base)
        return -5;
    registers = (volatile uint32_t *)host->map_mmio(base, 0x20000);
    if (!registers)
        return -5;
    /* Memory-space decode + bus mastering so BAR0 and DMA rings work. */
    host->pci_command(nic_bdf, 6);
    write_reg(IMC, 0xffffffffu);
    write_reg(RCTL, 0);
    write_reg(TCTL, 0);
    uint32_t control = read_reg(CTRL);
    write_reg(CTRL, control | (1u << 26));
    bool reset_done = false;
    for (unsigned count = 0; count < 1000000; count++)
        if (!(read_reg(CTRL) & (1u << 26))) {
            reset_done = true;
            break;
        }
    if (!reset_done)
        return -5;
    write_reg(IMC, 0xffffffffu);
    (void)read_reg(ICR);
    uint32_t low = read_reg(RAL), high = read_reg(RAH);
    for (unsigned i = 0; i < 4; i++)
        nic_mac[i] = (uint8_t)(low >> (i * 8));
    nic_mac[4] = (uint8_t)high;
    nic_mac[5] = (uint8_t)(high >> 8);
    if (!(high & 0x80000000u) || !(low | (high & 0xffff))) {
        for (unsigned i = 0; i < 3; i++) {
            uint16_t word;
            if (!eeprom_word(i, &word))
                return -5;
            nic_mac[i * 2] = (uint8_t)word;
            nic_mac[i * 2 + 1] = (uint8_t)(word >> 8);
        }
        low = (uint32_t)nic_mac[0] | ((uint32_t)nic_mac[1] << 8) |
              ((uint32_t)nic_mac[2] << 16) | ((uint32_t)nic_mac[3] << 24);
        high = (uint32_t)nic_mac[4] | ((uint32_t)nic_mac[5] << 8);
    }
    if ((nic_mac[0] & 1) || !(low | (high & 0xffff)))
        return -5;
    write_reg(RAL, low);
    write_reg(RAH, (high & 0xffff) | 0x80000000u);
    for (unsigned i = 0; i < 128; i++)
        write_reg(MTA + i * 4, 0);
    memset((void *)rx, 0, sizeof rx);
    memset((void *)tx, 0, sizeof tx);
    uint64_t pa;
    for (unsigned i = 0; i < RX_COUNT; i++) {
        if (!phys_of_checked(rx_data[i], &pa))
            return -12;
        rx[i].address = pa;
    }
    for (unsigned i = 0; i < TX_COUNT; i++) {
        if (!phys_of_checked(tx_data[i], &pa))
            return -12;
        tx[i].address = pa;
        tx[i].status = 1;
    }
    uint64_t address;
    if (!phys_of_checked((const void *)rx, &address))
        return -12;
    write_reg(RDBAL, (uint32_t)address);
    write_reg(RDBAH, (uint32_t)(address >> 32));
    write_reg(RDLEN, sizeof rx);
    write_reg(RDH, 0);
    write_reg(RDT, RX_COUNT - 1);
    if (!phys_of_checked((const void *)tx, &address))
        return -12;
    write_reg(TDBAL, (uint32_t)address);
    write_reg(TDBAH, (uint32_t)(address >> 32));
    write_reg(TDLEN, sizeof tx);
    write_reg(TDH, 0);
    write_reg(TDT, 0);
    /* Full-duplex standard 802.3 inter-packet gap and collision distance. */
    write_reg(TIPG, 10u | (8u << 10) | (6u << 20));
    control = read_reg(CTRL);
    control &= ~((1u << 3) | (1u << 7) | (1u << 31));
    write_reg(CTRL, control | (1u << 6)); /* Set Link Up, auto-negotiated. */
    fence();
    write_reg(TCTL, (1u << 1) | (1u << 3) | (15u << 4) | (64u << 12));
    write_reg(RCTL, (1u << 1) | (1u << 15) | (1u << 26)); /* 2048 B, strip FCS. */
    rx_next = tx_next = 0;
    discard_fragment = false;
    ready = true;
    return 0;
}

/* ---- module face ----------------------------------------------------------- */

static void format_mac(uint8_t out[18]) {
    static const char digits[] = "0123456789abcdef";
    unsigned at = 0;
    for (unsigned i = 0; i < 6; i++) {
        if (i)
            out[at++] = ':';
        out[at++] = (uint8_t)digits[nic_mac[i] >> 4];
        out[at++] = (uint8_t)digits[nic_mac[i] & 15];
    }
    out[at] = 0;
}

static void e1000_poll(void) {
    if (node < 0)
        return;
    /* Report byte/drop deltas and the interrupt count gathered since the
     * last run; overruns fold into the error counter. */
    static uint64_t seen_rx, seen_tx, seen_drops, seen_irq;
    uint64_t drx = rx_bytes - seen_rx, dtx = tx_bytes - seen_tx;
    uint64_t ddrop = rx_drops + irq_overruns - seen_drops;
    uint64_t dirq = irq_seen - seen_irq;
    if (drx || dtx || ddrop || dirq) {
        seen_rx = rx_bytes;
        seen_tx = tx_bytes;
        seen_drops = rx_drops + irq_overruns;
        seen_irq = irq_seen;
        if (!host->device_add_counters((uint32_t)node, drx, dtx, dirq, ddrop))
            host->device_notify((uint32_t)node);
    }
}

int64_t arco_entry(const ArkDriverHost *table, uint32_t op) {
    host = table;
    if (op == ARCO_OP_DEINIT) {
        /* Stop the network stack before the device node goes absent: the
         * ops table must never outlive the module's ownership claim. */
        host->net_detach();
        if (node >= 0)
            host->device_set_state((uint32_t)node, 0, ARK_DEV_STATE_ABSENT);
        node = -1;
        ready = false;
        if (irq_line >= 0 && registers) {
            /* The kernel masks the PIC line on removal; mask at the device
             * too so a pending cause cannot linger inside the chip. */
            write_reg(IMC, 0xffffffffu);
            (void)read_reg(ICR);
        }
        irq_line = -1;
        registers = 0;
        host->log("e1000 module removed; NIC detached");
        return 0;
    }
    if (op != ARCO_OP_INIT)
        return -22;

    int rc = nic_hw_init();
    if (rc)
        return rc;
    /* Interrupt line from PCI config (offset 0x3c): attach the ISR when the
     * host exports irq_attach; without it the driver stays purely polled. */
    uint32_t line = host->pci_read8(nic_bdf, 0x3c);
    if (host->irq_attach && line < 16 &&
        host->irq_attach(line, e1000_isr) == 0) {
        irq_line = (int)line;
        write_reg(IMS, CAUSE_LSC | CAUSE_RXDMT0 | CAUSE_RXO | CAUSE_RXT0);
    }
    nic_ops.send = nic_send;
    nic_ops.link = nic_link;
    nic_ops.receive = nic_receive;
    nic_ops.read_mac = nic_read_mac;
    rc = host->net_attach(&nic_ops);
    if (rc) {
        ready = false;
        return rc; /* another driver already owns the NIC (-16) */
    }
    uint8_t mac_text[18];
    format_mac(mac_text);
    ArkDeviceInfo info;
    ark_drv_node(&info, ARK_DEV_CLASS_NETWORK, 0, "Ethernet controller",
                 DRIVER_NAME, (const char *)mac_text);
    info.bus = ARK_BUS_PCI;
    info.bdf = nic_bdf;
    info.vendor = 0x8086;
    info.device = 0x100e;
    info.device_class = 0x0200; /* base class network, subclass ethernet */
    node = host->device_register(&info);
    if (node < 0) {
        host->net_detach();
        ready = false;
        return -12;
    }
    if (host->device_register_poll(e1000_poll) < 0) {
        host->net_detach();
        ready = false;
        return -16;
    }
    host->log("e1000 module online; NIC bound to module image");
    if (irq_line >= 0) {
        /* host->log is a plain-string service; the line is 0..15. */
        char text[48];
        static const char lead[] = "e1000 irq ";
        unsigned at = 0;
        while (lead[at])
            text[at] = (char)lead[at], at++;
        unsigned line = (unsigned)irq_line;
        if (line >= 10)
            text[at++] = (char)('0' + line / 10);
        text[at++] = (char)('0' + line % 10);
        static const char tail[] = " bound; interrupt-driven receive active";
        for (unsigned i = 0; tail[i]; i++)
            text[at++] = (char)tail[i];
        text[at] = 0;
        host->log(text);
    }
    return 0;
}
