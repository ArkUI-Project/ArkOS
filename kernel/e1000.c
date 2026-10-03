/* Native polling driver for Intel 82540EM (PCI 8086:100e, QEMU e1000).
 * Original code using the Intel 8254x register/legacy descriptor protocol.
 * Rings are identity-addressed DMA RAM; firmware PCI BARs are not relocated.
 */
#include "e1000.h"
#include "mmio.h"

#define RX_COUNT 64u
#define TX_COUNT 32u
#define BUFFER_SIZE 2048u
enum {
    CTRL = 0x0000,
    STATUS = 0x0008,
    EERD = 0x0014,
    ICR = 0x00c0,
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
static volatile uint32_t *registers;
static unsigned rx_next, tx_next;
static bool ready, discard_fragment;
static inline void fence(void) {
    __asm__ volatile("mfence" ::: "memory");
}
static inline void out32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0,%1" ::"a"(value), "Nd"(port));
}
static inline uint32_t in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}
static uint32_t pci_read(uint32_t bdf, unsigned offset) {
    out32(0xcf8, 0x80000000u | bdf | (offset & 0xfcu));
    return in32(0xcfc);
}
static void pci_command(uint32_t bdf, uint16_t command) {
    out32(0xcf8, 0x80000004u | bdf);
    __asm__ volatile("outw %0,%1" ::"a"(command), "Nd"((uint16_t)0xcfc));
}
static uint32_t read_reg(unsigned offset) {
    return registers[offset / 4];
}
static void write_reg(unsigned offset, uint32_t value) {
    registers[offset / 4] = value;
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
bool e1000_init(uint8_t mac[6]) {
    ready = false;
    registers = 0;
    uint32_t found = 0;
    bool present = false;
    for (unsigned bus = 0; bus < 256 && !present; bus++)
        for (unsigned dev = 0; dev < 32 && !present; dev++) {
            uint32_t bdf = (bus << 16) | (dev << 11), id = pci_read(bdf, 0);
            if ((id & 0xffff) == 0xffff)
                continue;
            unsigned count = (pci_read(bdf, 0x0c) & 0x800000) ? 8 : 1;
            for (unsigned function = 0; function < count; function++)
                if (pci_read(bdf | (function << 8), 0) == 0x100e8086) {
                    found = bdf | (function << 8);
                    present = true;
                    break;
                }
        }
    if (!present)
        return false;
    uint32_t bar = pci_read(found, 0x10);
    if ((bar & 1) || (bar & 6) == 2 || (bar & 6) == 6)
        return false;
    uint64_t base = bar & ~15u;
    if ((bar & 6) == 4)
        base |= (uint64_t)pci_read(found, 0x14) << 32;
    if (!base || !platform_map_mmio(base, 0x20000))
        return false;
    pci_command(found, (uint16_t)pci_read(found, 4) | 6u);
    registers = (volatile uint32_t *)(uintptr_t)base;
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
    if (!reset_done) {
        registers = 0;
        return false;
    }
    write_reg(IMC, 0xffffffffu);
    (void)read_reg(ICR);
    uint32_t low = read_reg(RAL), high = read_reg(RAH);
    for (unsigned i = 0; i < 4; i++)
        mac[i] = (uint8_t)(low >> (i * 8));
    mac[4] = (uint8_t)high;
    mac[5] = (uint8_t)(high >> 8);
    if (!(high & 0x80000000u) || !(low | (high & 0xffff))) {
        for (unsigned i = 0; i < 3; i++) {
            uint16_t word;
            if (!eeprom_word(i, &word))
                return false;
            mac[i * 2] = (uint8_t)word;
            mac[i * 2 + 1] = (uint8_t)(word >> 8);
        }
        low = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) |
              ((uint32_t)mac[3] << 24);
        high = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);
    }
    if ((mac[0] & 1) || !(low | (high & 0xffff)))
        return false;
    write_reg(RAL, low);
    write_reg(RAH, (high & 0xffff) | 0x80000000u);
    for (unsigned i = 0; i < 128; i++)
        write_reg(MTA + i * 4, 0);
    memset((void *)rx, 0, sizeof rx);
    memset((void *)tx, 0, sizeof tx);
    for (unsigned i = 0; i < RX_COUNT; i++)
        rx[i].address = (uint64_t)(uintptr_t)rx_data[i];
    for (unsigned i = 0; i < TX_COUNT; i++) {
        tx[i].address = (uint64_t)(uintptr_t)tx_data[i];
        tx[i].status = 1;
    }
    uint64_t address = (uint64_t)(uintptr_t)rx;
    write_reg(RDBAL, (uint32_t)address);
    write_reg(RDBAH, (uint32_t)(address >> 32));
    write_reg(RDLEN, sizeof rx);
    write_reg(RDH, 0);
    write_reg(RDT, RX_COUNT - 1);
    address = (uint64_t)(uintptr_t)tx;
    write_reg(TDBAL, (uint32_t)address);
    write_reg(TDBAH, (uint32_t)(address >> 32));
    write_reg(TDLEN, sizeof tx);
    write_reg(TDH, 0);
    write_reg(TDT, 0);
    /* Full-duplex standard 802.3 inter-packet gap and collision distance. */
    write_reg(TIPG, 10u | (8u << 10) | (6u << 20));
    control = read_reg(CTRL);
    control &= ~((1u << 3) | (1u << 7) | (1u << 31));
    write_reg(CTRL, control | (1u << 6)); /* Set Link Up, auto-negotiated speed. */
    fence();
    write_reg(TCTL, (1u << 1) | (1u << 3) | (15u << 4) | (64u << 12));
    write_reg(RCTL, (1u << 1) | (1u << 15) | (1u << 26)); /* 2048-byte buffers, strip FCS. */
    rx_next = tx_next = 0;
    discard_fragment = false;
    ready = true;
    serial_write("[net] Native Intel 82540EM e1000 DMA rings ready\n");
    return true;
}
bool e1000_link(void) {
    return ready && (read_reg(STATUS) & 2) != 0;
}
bool e1000_send(const uint8_t *frame, size_t length) {
    if (!ready || !frame || length < 14 || length > 1514)
        return false;
    volatile TxDescriptor *descriptor = &tx[tx_next];
    if (!(descriptor->status & 1) || (tx_next + 1) % TX_COUNT == read_reg(TDH))
        return false;
    fence();
    memcpy(tx_data[tx_next], frame, length);
    if (length < 60) {
        memset(tx_data[tx_next] + length, 0, 60 - length);
        length = 60;
    }
    descriptor->length = (uint16_t)length;
    descriptor->checksum_offset = 0;
    descriptor->checksum_start = 0;
    descriptor->special = 0;
    descriptor->status = 0;
    descriptor->command = 0x0b; /* EOP | IFCS | RS, no hardware checksum offload. */
    fence();
    tx_next = (tx_next + 1) % TX_COUNT;
    write_reg(TDT, tx_next);
    return true;
}
size_t e1000_receive(uint8_t *frame, size_t capacity) {
    if (!ready || !frame)
        return 0;
    for (unsigned count = 0; count < RX_COUNT; count++) {
        volatile RxDescriptor *descriptor = &rx[rx_next];
        if (!(descriptor->status & 1))
            return 0;
        fence();
        unsigned length = descriptor->length;
        bool end = (descriptor->status & 2) != 0;
        bool valid = !discard_fragment && end && !descriptor->errors && length >= 14 &&
                     length <= 1514 && length <= capacity;
        if (valid)
            memcpy(frame, rx_data[rx_next], length);
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
