/* Native PCI AHCI 1.3.1 SATA DMA driver. Polling, one command per port.
 * Firmware owns PCI resource assignment. Only 512-byte LBA48 disks accepted.
 * DMA uses private permanent low physical buffers; no user pointer reaches HBA. */
#include "ahci.h"
#include "mmio.h"
#define AHCI_MAX 3u
#define CD_SLOT 2u
static bool cd_ready;
#define DMA_BYTES (64u * 1024u)
static struct {
    volatile uint32_t *p;
    uint64_t sectors;
    bool ready;
    uint8_t list[1024] __attribute__((aligned(1024)));
    uint8_t fis[256] __attribute__((aligned(256)));
    uint8_t table[256] __attribute__((aligned(128)));
    uint8_t bounce[DMA_BYTES] __attribute__((aligned(4096)));
} disks[AHCI_MAX];
static unsigned count;
static bool searched;
/* DX operands are required: PCI configuration ports exceed 8-bit immediates. */
static void out32(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(port));
}
static uint32_t in32(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static uint32_t cfg(uint32_t bdf, unsigned reg) {
    out32(0xcf8, 0x80000000u | bdf | (reg & ~3u));
    return in32(0xcfc);
}
static bool wait_clear(volatile uint32_t *p, uint32_t mask) {
    uint64_t start = platform_ticks();
    for (unsigned i = 0; i < 5000000; i++) {
        if (!(*p & mask))
            return true;
        if (platform_ticks() - start > 200)
            break;
        __asm__ volatile("pause");
    }
    return false;
}
static bool stop(volatile uint32_t *p) {
    p[0x18 / 4] &= ~1u;
    if (!wait_clear(p + 0x18 / 4, 1u << 15))
        return false;
    p[0x18 / 4] &= ~16u;
    return wait_clear(p + 0x18 / 4, 1u << 14);
}
static bool command(unsigned disk, uint8_t op, uint64_t lba, unsigned sectors, bool write,
                    unsigned bytes) {
    if (disk >= AHCI_MAX || !disks[disk].p || bytes > DMA_BYTES)
        return false;
    volatile uint32_t *p = disks[disk].p;
    if (!wait_clear(p + 0x20 / 4, 0x88) || p[0x38 / 4])
        return false;
    uint8_t *ct = disks[disk].table;
    uint32_t *header = (uint32_t *)(void *)disks[disk].list;
    memset(ct, 0, 256);
    memset(disks[disk].list, 0, 1024);
    header[0] = 5u | (write ? 1u << 6 : 0) | (bytes ? 1u << 16 : 0);
    header[2] = (uint32_t)(uintptr_t)ct;
    ct[0] = 0x27;
    ct[1] = 0x80;
    ct[2] = op;
    ct[4] = (uint8_t)lba;
    ct[5] = (uint8_t)(lba >> 8);
    ct[6] = (uint8_t)(lba >> 16);
    ct[7] = 0x40;
    ct[8] = (uint8_t)(lba >> 24);
    ct[9] = (uint8_t)(lba >> 32);
    ct[10] = (uint8_t)(lba >> 40);
    ct[12] = (uint8_t)sectors;
    ct[13] = (uint8_t)(sectors >> 8);
    if (op == 0xa0) {
        header[0] |= 1u << 5;
        ct[3] = 1;
        ct[4] = 0;
        ct[5] = 0;
        ct[6] = 0;
        ct[7] = 0;
        ct[8] = ct[9] = ct[10] = 0;
        ct[12] = ct[13] = 0;
        ct[64] = 0xa8;
        for (unsigned j = 0; j < 4; j++) {
            ct[66 + j] = (uint8_t)(lba >> (24 - 8 * j));
            ct[70 + j] = (uint8_t)(sectors >> (24 - 8 * j));
        }
    }
    if (bytes) {
        uint32_t *prdt = (uint32_t *)(void *)(ct + 128);
        prdt[0] = (uint32_t)(uintptr_t)disks[disk].bounce;
        prdt[3] = bytes - 1;
    }
    p[0x30 / 4] = UINT32_MAX;
    p[0x10 / 4] = UINT32_MAX;
    __asm__ volatile("mfence" ::: "memory");
    p[0x38 / 4] = 1;
    bool complete = wait_clear(p + 0x38 / 4, 1);
    __asm__ volatile("mfence" ::: "memory");
    if (!complete || (p[0x10 / 4] & (1u << 30)) || (p[0x20 / 4] & 0x21)) {
        disks[disk].ready = false;
        (void)stop(p);
        serial_write("[ahci] Command failed; port disabled, DMA buffer retained\n");
        return false;
    }
    return !bytes || header[1] == bytes;
}
unsigned ahci_init(void) {
    if (searched)
        return count;
    searched = true;
    for (unsigned bus = 0; bus < 256 && !count; bus++)
        for (unsigned slot = 0; slot < 32 && !count; slot++) {
            uint32_t base = (bus << 16) | (slot << 11);
            if ((cfg(base, 0) & 0xffff) == 0xffff)
                continue;
            unsigned functions = (cfg(base, 0x0c) & 0x800000) ? 8 : 1;
            for (unsigned fn = 0; fn < functions && !count; fn++) {
                uint32_t bdf = base | (fn << 8);
                if ((cfg(bdf, 8) >> 8) != 0x010601)
                    continue;
                uint32_t bar = cfg(bdf, 0x24);
                if ((bar & 15) || !(bar & ~15u))
                    continue;
                uint64_t mmio = bar & ~15u;
                if (!platform_map_mmio(mmio, 0x1100))
                    continue;
                uint16_t cmd = (uint16_t)cfg(bdf, 4);
                out32(0xcf8, 0x80000000u | bdf | 4);
                __asm__ volatile("outw %0,%1" ::"a"((uint16_t)(cmd | 6 | 0x400)),
                                 "Nd"((uint16_t)0xcfc));
                volatile uint32_t *h = (volatile uint32_t *)(uintptr_t)mmio;
                if (h[0x24 / 4] & 1) {
                    h[0x28 / 4] |= 2;
                    if (!wait_clear(h + 0x28 / 4, 0x11)) {
                        serial_write("[ahci] BIOS ownership timeout\n");
                        continue;
                    }
                }
                h[1] = 1u << 31; /* Preserve established SATA links; stop/rebase each owned port. */
                uint32_t ports = h[3];
                for (unsigned port = 0; port < 32; port++)
                    if (ports & (1u << port)) {
                        volatile uint32_t *p = h + 0x100 / 4 + port * 0x80 / 4;
                        uint32_t status = p[0x28 / 4];
                        if ((status & 15) != 3 || ((status >> 8) & 15) != 1)
                            continue;
                        bool cd = p[0x24 / 4] == 0xeb140101;
                        if ((!cd && p[0x24 / 4] != 0x101) || (cd && cd_ready) ||
                            (!cd && count >= 2))
                            continue;
                        if (!stop(p))
                            continue;
                        unsigned i = cd ? CD_SLOT : count;
                        disks[i].p = p;
                        p[0] = (uint32_t)(uintptr_t)disks[i].list;
                        p[1] = 0;
                        p[2] = (uint32_t)(uintptr_t)disks[i].fis;
                        p[3] = 0;
                        p[0x14 / 4] = 0;
                        p[0x30 / 4] = UINT32_MAX;
                        p[0x10 / 4] = UINT32_MAX;
                        if (cd)
                            p[0x18 / 4] |= 1u << 24;
                        else
                            p[0x18 / 4] &= ~(1u << 24);
                        p[0x18 / 4] |= 16;
                        p[0x18 / 4] |= 1;
                        if (!command(i, cd ? 0xa1 : 0xec, 0, 0, false, 512))
                            continue;
                        if (cd) {
                            cd_ready = true;
                            disks[i].ready = true;
                            serial_write("[ahci] Native ATAPI optical drive ready\n");
                            continue;
                        }
                        uint16_t *id = (uint16_t *)(void *)disks[i].bounce;
                        if (!(id[83] & (1u << 10)) || !(id[83] & (1u << 12)))
                            continue;
                        if ((id[106] & 0xd000) == 0x5000 &&
                            ((uint32_t)id[117] | (uint32_t)id[118] << 16) != 256)
                            continue;
                        uint64_t sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 |
                                           (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
                        if (!sectors || sectors > (1ull << 48))
                            continue;
                        disks[i].sectors = sectors;
                        disks[i].ready = true;
                        count++;
                        serial_write("[ahci] Native SATA DMA disk port=");
                        char n[24];
                        uint_to_str(port, n);
                        serial_write(n);
                        serial_write(" sectors=");
                        uint_to_str(sectors, n);
                        serial_write(n);
                        serial_write("\n");
                    }
            }
        }
    return count;
}
uint64_t ahci_sectors(unsigned id) {
    return id < count && disks[id].ready ? disks[id].sectors : 0;
}
bool ahci_transfer(unsigned id, uint64_t lba, uint32_t sectors, void *buffer, bool write) {
    if (id >= count || !disks[id].ready || !buffer || !sectors || lba >= disks[id].sectors ||
        sectors > disks[id].sectors - lba)
        return false;
    uint8_t *p = buffer;
    while (sectors) {
        unsigned n = sectors > 128 ? 128 : sectors;
        if (write)
            memcpy(disks[id].bounce, p, n * 512);
        if (!command(id, write ? 0x35 : 0x25, lba, n, write, n * 512))
            return false;
        if (!write)
            memcpy(p, disks[id].bounce, n * 512);
        p += n * 512;
        lba += n;
        sectors -= n;
    }
    return true;
}
bool ahci_flush(unsigned id) {
    return id < count && disks[id].ready && command(id, 0xea, 0, 0, false, 0);
}

bool ahci_cd_read(uint32_t lba, unsigned count, void *buffer) {
    if (!cd_ready || !buffer || !count || count > 32)
        return false;
    if (!command(CD_SLOT, 0xa0, lba, count, false, count * 2048))
        return false;
    memcpy(buffer, disks[CD_SLOT].bounce, count * 2048);
    return true;
}
