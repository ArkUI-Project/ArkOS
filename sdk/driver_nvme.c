/* NVMe kernel driver: a PCIe NVM controller (QEMU's nvme, class 01/08) as a
 * .arco loadable module. Original code implementing the NVMe 1.x register and
 * queue protocol directly — no libc, no host kernel storage stack.
 *
 * The controller is MSI-X only on QEMU, so INIT asks the host for an MSI-X
 * vector with host->msi_attach. The kernel owns the capability and programs
 * the table entry; the driver only points its I/O completion queue at vector
 * index 0. If MSI-X cannot be delivered the driver still works but stays
 * poll-only and says so — it never claims interrupt success.
 *
 * All DMA buffers (admin/IO queues, identify page, PRP list, bounce buffer)
 * live in this module's .bss. The image and stack occupy one contiguous
 * kernel-pool physical run, so host->phys_of turns each buffer into its real
 * bus address; the block layer's transfer buffer is a kernel VA with no bus
 * address, so reads and writes bounce through the 64 KiB buffer exactly like
 * the built-in AHCI transport.
 *
 * Build:  python3 scripts/arco.py sdk/driver_nvme.c -o build/nvme.arco \
 *            --name nvme --version 1.0.0
 * The kernel embeds this image as an inbox driver; a manifest copy under
 * @drv.nvme shadows it for development updates. */
#include "driver.h"

#define DRIVER_NAME "nvme"

/* Queue geometry and buffer sizes. The module .bss budget is 256 KiB. */
#define ADMIN_QID 0u
#define IO_QID 1u
#define ADMIN_DEPTH 16u
#define IO_DEPTH 64u
#define SQ_ENTRY 64u
#define CQ_ENTRY 16u
#define IDENTIFY_BYTES 4096u
#define BOUNCE_BYTES 65536u /* 128 sectors per command */
#define PRP_ENTRIES 512u    /* 4096 / 8 */
#define ADMIN_SPINS 200000000u
#define IO_SPINS 200000000u

enum {
    NVME_CAP = 0x00,
    NVME_VS = 0x08,
    NVME_INTMS = 0x0c,
    NVME_INTMC = 0x10,
    NVME_CC = 0x14,
    NVME_CSTS = 0x1c,
    NVME_AQA = 0x24,
    NVME_ASQ = 0x28,
    NVME_ACQ = 0x30,
    NVME_DBS = 0x1000
};

/* Command opcodes. */
enum {
    NVME_OP_FLUSH = 0x00,
    NVME_OP_WRITE = 0x01,
    NVME_OP_READ = 0x02,
    NVME_OP_CREATE_SQ = 0x01,
    NVME_OP_CREATE_CQ = 0x05,
    NVME_OP_IDENTIFY = 0x06
};

static uint8_t admin_sq[ADMIN_DEPTH * SQ_ENTRY] __attribute__((aligned(4096)));
static uint8_t admin_cq[ADMIN_DEPTH * CQ_ENTRY] __attribute__((aligned(4096)));
static uint8_t io_sq[IO_DEPTH * SQ_ENTRY] __attribute__((aligned(4096)));
static uint8_t io_cq[IO_DEPTH * CQ_ENTRY] __attribute__((aligned(4096)));
static uint8_t identify[IDENTIFY_BYTES] __attribute__((aligned(4096)));
static uint8_t bounce[BOUNCE_BYTES] __attribute__((aligned(4096)));
static uint64_t prp_list[PRP_ENTRIES] __attribute__((aligned(4096)));

static const ArkDriverHost *host;
static volatile uint32_t *regs;
static uint32_t ctrl_bdf, ctrl_vendor, ctrl_device;
static uint64_t ns_sectors;
static uint32_t page_bytes = 4096, max_io_sectors = BOUNCE_BYTES / 512u;
static uint32_t admin_tail, admin_head, admin_phase = 1;
static uint32_t io_tail, io_head, io_phase = 1;
static uint16_t admin_cid, io_cid;
static bool ready;
static int node = -1;
static int block_slot = -1;
static volatile uint32_t irq_seen;

/* Freestanding runtimes: the image links no libc. */
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
static uint32_t rd(unsigned offset) {
    return regs[offset / 4];
}
static void wr(unsigned offset, uint32_t value) {
    regs[offset / 4] = value;
}
static uint64_t rd64(unsigned offset) {
    return (uint64_t)rd(offset) | ((uint64_t)rd(offset + 4) << 32);
}
static void wr64(unsigned offset, uint64_t value) {
    wr(offset, (uint32_t)value);
    wr(offset + 4, (uint32_t)(value >> 32));
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static bool phys_of(const void *va, uint64_t *out) {
    uint64_t pa = host->phys_of(va);
    if (!pa)
        return false;
    *out = pa;
    return true;
}
/* ---- interrupt ----------------------------------------------------------- */

/* Runs in interrupt context on the module stack: no host calls, no allocation.
 * The completion queue entry is the interrupt source; nvme_io consumes it and
 * rings the CQ head, so the ISR only records that the vector really fired. */
static void nvme_isr(void) {
    ++irq_seen;
}

/* ---- admin queue --------------------------------------------------------- */

static int admin_submit(const uint32_t *cmd) {
    uint32_t slot = admin_tail;
    uint32_t *sq = (uint32_t *)(admin_sq + (unsigned long)slot * SQ_ENTRY);
    for (unsigned i = 0; i < 16; i++)
        sq[i] = cmd[i];
    fence();
    admin_tail = (admin_tail + 1) % ADMIN_DEPTH;
    wr(NVME_DBS + (2 * ADMIN_QID) * 4, admin_tail);
    volatile uint32_t *cq = (volatile uint32_t *)(admin_cq + (unsigned long)admin_head * CQ_ENTRY);
    for (unsigned spin = 0; spin < ADMIN_SPINS; spin++) {
        /* DW3: CID bits 15:0, status field bits 31:16 (phase bit 0 of the
         * field, status code bits 8:1). */
        uint16_t field = (uint16_t)(cq[3] >> 16);
        if ((field & 1u) == admin_phase) {
            uint16_t sc = (uint16_t)((field >> 1) & 0xffu);
            /* The controller flips the phase tag only when the CQ wraps. */
            if (++admin_head == ADMIN_DEPTH) {
                admin_head = 0;
                admin_phase ^= 1;
            }
            wr(NVME_DBS + (2 * ADMIN_QID + 1) * 4, admin_head);
            return sc ? -5 : 0;
        }
        __asm__ volatile("pause");
    }
    return -5;
}

static int nvme_disable(void) {
    uint32_t cc = rd(NVME_CC);
    if (!(cc & 1u))
        return 0;
    wr(NVME_CC, cc & ~1u);
    for (unsigned spin = 0; spin < ADMIN_SPINS; spin++)
        if (!(rd(NVME_CSTS) & 1u))
            return 0;
    return -5;
}
static int nvme_enable(void) {
    /* AQA: ASQS bits 11:0, ACQS bits 27:16 (the field is not contiguous). */
    wr(NVME_AQA, (ADMIN_DEPTH - 1u) | ((ADMIN_DEPTH - 1u) << 16));
    uint64_t asq, acq;
    if (!phys_of(admin_sq, &asq) || !phys_of(admin_cq, &acq))
        return -12;
    wr64(NVME_ASQ, asq);
    wr64(NVME_ACQ, acq);
    /* EN=1, CSS=NVM(0), MPS=4 KiB(0), AMS=0, IOSQES=6, IOCQES=4. */
    wr(NVME_CC, (1u << 0) | (6u << 16) | (4u << 20));
    for (unsigned spin = 0; spin < ADMIN_SPINS; spin++) {
        uint32_t csts = rd(NVME_CSTS);
        if (csts & 2u)
            return -5; /* controller fatal status */
        if (csts & 1u)
            return 0;
    }
    return -5;
}
static int nvme_identify(uint32_t cns, uint32_t nsid) {
    uint64_t pa;
    if (!phys_of(identify, &pa))
        return -12;
    uint32_t cmd[16];
    for (unsigned i = 0; i < 16; i++)
        cmd[i] = 0;
    cmd[0] = NVME_OP_IDENTIFY | ((uint32_t)(++admin_cid) << 16);
    cmd[1] = nsid;
    cmd[6] = (uint32_t)pa;
    cmd[7] = (uint32_t)(pa >> 32);
    cmd[10] = cns;
    return admin_submit(cmd);
}
static int nvme_create_queues(bool interrupts) {
    uint64_t cq, sq;
    if (!phys_of(io_cq, &cq) || !phys_of(io_sq, &sq))
        return -12;
    uint32_t cmd[16];
    for (unsigned i = 0; i < 16; i++)
        cmd[i] = 0;
    cmd[0] = NVME_OP_CREATE_CQ | ((uint32_t)(++admin_cid) << 16);
    cmd[6] = (uint32_t)cq;
    cmd[7] = (uint32_t)(cq >> 32);
    cmd[10] = IO_QID | ((IO_DEPTH - 1u) << 16);
    cmd[11] = 1u | (interrupts ? 2u : 0u); /* PC | IEN; IV = vector index 0 */
    int rc = admin_submit(cmd);
    if (rc)
        return rc;
    for (unsigned i = 0; i < 16; i++)
        cmd[i] = 0;
    cmd[0] = NVME_OP_CREATE_SQ | ((uint32_t)(++admin_cid) << 16);
    cmd[6] = (uint32_t)sq;
    cmd[7] = (uint32_t)(sq >> 32);
    cmd[10] = IO_QID | ((IO_DEPTH - 1u) << 16);
    cmd[11] = 1u | (IO_QID << 16); /* PC | QPRIO=0 | CQID */
    return admin_submit(cmd);
}

/* ---- I/O queue ----------------------------------------------------------- */

static int io_complete(void) {
    volatile uint32_t *cq = (volatile uint32_t *)(io_cq + (unsigned long)io_head * CQ_ENTRY);
    for (unsigned spin = 0; spin < IO_SPINS; spin++) {
        /* Same DW3 layout as the admin queue: status field is bits 31:16. */
        uint16_t field = (uint16_t)(cq[3] >> 16);
        if ((field & 1u) == io_phase) {
            uint16_t sc = (uint16_t)((field >> 1) & 0xffu);
            /* The controller flips the phase tag only when the CQ wraps. */
            if (++io_head == IO_DEPTH) {
                io_head = 0;
                io_phase ^= 1;
            }
            wr(NVME_DBS + (2 * IO_QID + 1) * 4, io_head);
            return sc ? -5 : 0;
        }
        __asm__ volatile("pause");
    }
    return -5;
}
static void io_ring(void) {
    io_tail = (io_tail + 1) % IO_DEPTH;
    wr(NVME_DBS + (2 * IO_QID) * 4, io_tail);
}
static int nvme_io(uint32_t opcode, uint64_t lba, uint32_t sectors) {
    uint64_t prp1;
    if (!phys_of(bounce, &prp1))
        return -12;
    uint32_t pages = (sectors * 512u + page_bytes - 1) / page_bytes;
    uint64_t prp2 = 0;
    if (pages > 1) {
        uint64_t list;
        if (!phys_of(prp_list, &list))
            return -12;
        for (uint32_t i = 1; i < pages; i++)
            prp_list[i - 1] = prp1 + (uint64_t)i * page_bytes;
        prp2 = list;
    }
    uint32_t *sq = (uint32_t *)(io_sq + (unsigned long)io_tail * SQ_ENTRY);
    for (unsigned i = 0; i < 16; i++)
        sq[i] = 0;
    sq[0] = opcode | ((uint32_t)(++io_cid) << 16);
    sq[1] = 1; /* NSID */
    sq[6] = (uint32_t)prp1;
    sq[7] = (uint32_t)(prp1 >> 32);
    sq[8] = (uint32_t)prp2;
    sq[9] = (uint32_t)(prp2 >> 32);
    sq[10] = (uint32_t)lba;
    sq[11] = (uint32_t)(lba >> 32);
    sq[12] = sectors - 1u;
    fence();
    io_ring();
    return io_complete();
}
static int nvme_flush(void) {
    if (!ready)
        return -22;
    uint32_t *sq = (uint32_t *)(io_sq + (unsigned long)io_tail * SQ_ENTRY);
    for (unsigned i = 0; i < 16; i++)
        sq[i] = 0;
    sq[0] = NVME_OP_FLUSH | ((uint32_t)(++io_cid) << 16);
    sq[1] = 1;
    fence();
    io_ring();
    return io_complete();
}

/* ---- ArkBlockOps --------------------------------------------------------- */

static uint64_t nvme_sectors(void) {
    return ready ? ns_sectors : 0;
}
static int nvme_transfer(uint64_t lba, uint32_t count, void *buffer, int write) {
    if (!ready || !buffer || !count || lba >= ns_sectors || count > ns_sectors - lba)
        return -22;
    uint8_t *p = (uint8_t *)buffer;
    while (count) {
        uint32_t n = count > max_io_sectors ? max_io_sectors : count;
        if (write)
            memcpy(bounce, p, (unsigned long)n * 512u);
        int rc = nvme_io(write ? NVME_OP_WRITE : NVME_OP_READ, lba, n);
        if (rc)
            return rc;
        if (!write)
            memcpy(p, bounce, (unsigned long)n * 512u);
        p += (unsigned long)n * 512u;
        lba += n;
        count -= n;
    }
    return 0;
}
static ArkBlockOps nvme_block_ops;

/* ---- device counters ----------------------------------------------------- */

static void nvme_poll(void) {
    if (node < 0)
        return;
    static uint64_t seen;
    uint64_t now = irq_seen;
    if (now != seen) {
        host->device_add_counters((uint32_t)node, 0, 0, now - seen, 0);
        host->device_notify((uint32_t)node);
        seen = now;
    }
}

/* ---- init ---------------------------------------------------------------- */

static int nvme_find(uint32_t *bdf, uint32_t *vendor, uint32_t *device) {
    unsigned count = host->pci_count();
    for (unsigned i = 0; i < count; i++) {
        uint32_t rec[10];
        if (!host->pci_device(i, rec))
            continue;
        /* Base class 01 (mass storage), subclass 08 (NVM). */
        if (rec[3] != 0x01u || rec[4] != 0x08u)
            continue;
        *bdf = rec[0];
        *vendor = rec[1];
        *device = rec[2];
        return 0;
    }
    return -19;
}

static int nvme_fail(const char *why, int rc) {
    host->log(why);
    return rc;
}

static int nvme_hw_init(void) {
    if (nvme_find(&ctrl_bdf, &ctrl_vendor, &ctrl_device))
        return nvme_fail("nvme: no PCIe NVM controller (class 01/08)", -19);
    uint64_t base = 0;
    if (host->pci_bar_base(ctrl_bdf, 0, &base) || !base)
        return nvme_fail("nvme: BAR0 is not a mapped memory BAR", -5);
    regs = (volatile uint32_t *)host->map_mmio(base, 0x4000);
    if (!regs)
        return nvme_fail("nvme: BAR0 MMIO map failed", -5);
    host->pci_command(ctrl_bdf, 6); /* memory space + bus master */
    /* CAP.MPSMIN (bits 51:48) is the smallest supported host page size as a
     * power of two above 4 KiB; we only program CC.MPS = 0 (4 KiB). */
    uint64_t cap = rd64(NVME_CAP);
    if (((cap >> 48) & 0xfu) > 0u)
        return nvme_fail("nvme: controller needs a page size above 4 KiB", -19);
    wr(NVME_INTMS, 0xffffffffu); /* mask until the queues are live */
    int rc = nvme_disable();
    if (rc)
        return nvme_fail("nvme: controller reset (disable) failed", rc);
    rc = nvme_enable();
    if (rc)
        return nvme_fail("nvme: controller enable failed", rc);

    bool msi_ok = false;
    uint32_t vector = 0;
    if (host->msi_attach && host->msi_attach(ctrl_bdf, nvme_isr, &vector) == 0)
        msi_ok = true;
    rc = nvme_create_queues(msi_ok);
    if (rc)
        return nvme_fail("nvme: I/O queue creation failed", rc);

    if (nvme_identify(1, 0)) /* Identify Controller */
        return nvme_fail("nvme: identify controller failed", -5);
    char model[41];
    for (unsigned i = 0; i < 40; i++)
        model[i] = (char)identify[24 + i];
    model[40] = 0;
    unsigned end = 40;
    while (end && model[end - 1] == ' ')
        model[--end] = 0;
    uint8_t mdts = identify[77];

    if (nvme_identify(0, 1)) /* Identify Namespace 1 */
        return nvme_fail("nvme: identify namespace 1 failed", -5);
    uint64_t nsze = le64(identify + 0);
    uint8_t flbas = identify[26] & 0x0fu;
    uint32_t lbaf = le32(identify + 128 + flbas * 4u);
    unsigned lbads = (lbaf >> 16) & 0xffu;
    if (!nsze || lbads != 9u)
        return nvme_fail("nvme: namespace is empty or not 512-byte LBA", -19);
    ns_sectors = nsze;
    max_io_sectors = BOUNCE_BYTES / 512u;
    if (mdts) {
        uint64_t mdts_sectors = ((uint64_t)1 << mdts) * (page_bytes / 512u);
        if (mdts_sectors < max_io_sectors)
            max_io_sectors = (uint32_t)mdts_sectors;
    }
    ready = true;
    if (msi_ok)
        wr(NVME_INTMC, 1u); /* unmask vector 0 */

    nvme_block_ops.sectors = nvme_sectors;
    nvme_block_ops.transfer = nvme_transfer;
    nvme_block_ops.flush = nvme_flush;
    if (!host->block_attach)
        return nvme_fail("nvme: host has no block_attach", -19);
    int unit = host->block_attach(&nvme_block_ops);
    if (unit < 0)
        return nvme_fail("nvme: block_attach refused (no free disk slot)", unit);
    block_slot = unit;

    char detail[96];
    unsigned at = 0;
    for (unsigned i = 0; model[i] && at + 1 < sizeof detail; i++)
        detail[at++] = model[i];
    if (at && at + 1 < sizeof detail)
        detail[at++] = ' ';
    const char *tail = "512 B LBA";
    for (unsigned i = 0; tail[i] && at + 1 < sizeof detail; i++)
        detail[at++] = tail[i];
    detail[at] = 0;

    ArkDeviceInfo info;
    ark_drv_node(&info, ARK_DEV_CLASS_BLOCK, (uint32_t)unit, "NVMe namespace",
                 DRIVER_NAME, detail);
    info.bus = ARK_BUS_PCI;
    info.bdf = ctrl_bdf;
    info.vendor = (uint16_t)ctrl_vendor;
    info.device = (uint16_t)ctrl_device;
    info.device_class = 0x0108;
    info.blocks = ns_sectors;
    info.flags |= ARK_DEV_READABLE | ARK_DEV_WRITABLE;
    node = host->device_register(&info);
    if (node < 0)
        return nvme_fail("nvme: device node registration failed", -12);
    if (host->device_register_poll(nvme_poll) < 0)
        return nvme_fail("nvme: poll callback registration failed", -16);
    host->log(msi_ok ? "nvme module online; MSI-X vector bound"
                     : "nvme module online; MSI-X unavailable, poll-only (gate fails)");
    return 0;
}

/* ---- module face --------------------------------------------------------- */

int64_t arco_entry(const ArkDriverHost *table, uint32_t op) {
    host = table;
    if (op == ARCO_OP_DEINIT) {
        ready = false;
        if (regs)
            wr(NVME_INTMS, 0xffffffffu);
        host->block_detach();
        if (host->msi_detach)
            host->msi_detach();
        if (node >= 0)
            host->device_set_state((uint32_t)node, 0, ARK_DEV_STATE_ABSENT);
        node = -1;
        block_slot = -1;
        regs = 0;
        host->log("nvme module removed; namespace detached");
        return 0;
    }
    if (op != ARCO_OP_INIT)
        return -22;
    return nvme_hw_init();
}
