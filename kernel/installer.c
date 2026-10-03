/* Native optical-to-disk installer. No firmware disk calls or host service. */
#include "ark.h"
#define ARK_KERNEL
#include "installer.h"
#include "ahci.h"
#include "block.h"
#include "storage.h"
#include "extfs.h"
static uint8_t buffer[65536] __attribute__((aligned(4096))), mbr[512], header[512], entries[32768];
static unsigned state, target;
static uint32_t blocks, at, data_start;
static uint64_t expected;
static char failure[128];
static uint32_t rd(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t *p) {
    return rd(p) | (uint64_t)rd(p + 4) << 32;
}
static void wr(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void wr64(uint8_t *p, uint64_t v) {
    wr(p, (uint32_t)v);
    wr(p + 4, (uint32_t)(v >> 32));
}
static uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}
static bool target_in_use(unsigned id) {
    if (storage_disk_in_use(id))
        return true;
    if (id == 1)
        for (unsigned i = 0; i < 2; i++) {
            ExtVolumeInfo v;
            if (extfs_volume_info(i, &v) && v.mounted)
                return true;
        }
    return false;
}
static bool source(void) {
    if (!ahci_cd_read(16, 1, buffer) || buffer[0] != 1 || strncmp((char *)buffer + 1, "CD001", 5) ||
        strncmp((char *)buffer + 40, "ARKOS0130", 9))
        return false;
    blocks = rd(buffer + 80);
    return blocks >= 1024 && blocks < 524288;
}
static bool finish(void) {
    BlockDevice *d = block_device(target);
    if (!block_read(d, 1, 1, header) || strncmp((char *)header, "EFI PART", 8))
        return false;
    unsigned h = rd(header + 12), n = rd(header + 80), bytes = rd(header + 84);
    uint64_t table = rd64(header + 72);
    if (h != 92 || bytes != 128 || n < 4 || n > 256 || table < 2 || table > 128)
        return false;
    uint32_t saved = rd(header + 16);
    wr(header + 16, 0);
    if (crc(header, h) != saved)
        return false;
    unsigned table_n = (n * 128 + 511) / 512;
    if (!block_read(d, table, table_n, entries) || crc(entries, n * 128) != rd(header + 88))
        return false;
    unsigned slot = 0;
    for (; slot < n; slot++) {
        bool empty = true;
        for (unsigned j = 0; j < 16; j++)
            if (entries[slot * 128 + j])
                empty = false;
        if (empty)
            break;
    }
    if (slot == n)
        return false;
    uint64_t last = d->sectors - 1, backup = last - table_n;
    if (backup <= data_start + 16384)
        return false;
    uint8_t *e = entries + slot * 128;
    memset(e, 0, 128); /* ArkFS private GUID, never mistaken for FAT/NTFS. */
    const uint8_t guid[16] = {0x41, 0x52, 0x4b, 0x46, 0x53, 0x37, 0x40, 0x70,
                              0x80, 0x10, 0x41, 0x52, 0x4b, 0x4f, 0x53, 0x31};
    memcpy(e, guid, 16);
    memcpy(e + 16, guid, 16);
    wr(e + 16, crc(mbr, 512) ^ data_start);
    wr64(e + 32, data_start);
    wr64(e + 40, backup - 1);
    const char *label = "ArkOS Data";
    for (unsigned i = 0; label[i]; i++)
        e[56 + i * 2] = (uint8_t)label[i];
    wr64(header + 32, last);
    wr64(header + 48, backup - 1);
    wr(header + 88, crc(entries, n * 128));
    wr(header + 16, 0);
    wr(header + 16, crc(header, h));
    if (!block_write(d, table, table_n, entries) || !block_write(d, 1, 1, header) ||
        !block_write(d, backup, table_n, entries))
        return false;
    wr64(header + 24, last);
    wr64(header + 32, 1);
    wr64(header + 72, backup);
    wr(header + 16, 0);
    wr(header + 16, crc(header, h));
    if (!block_write(d, last, 1, header))
        return false;
    uint32_t data_n = (uint32_t)(backup - data_start);
    if (!storage_install_format(target, data_start, data_n))
        return false;
    /* Hybrid MBR slot 4 advertises the data volume to the native mount driver. */
    e = mbr + 446 + 3 * 16;
    memset(e, 0, 16);
    e[4] = 0xda;
    e[1] = e[5] = 0xfe;
    e[2] = e[3] = e[6] = e[7] = 0xff;
    wr(e + 8, data_start);
    wr(e + 12, data_n);
    return block_flush(d) && block_write(d, 0, 1, mbr) && block_flush(d);
}
int64_t installer_request(ArkInstallRequest *q) {
    block_init();
    if (q->op == ARK_INSTALL_LIST) {
        BlockDevice *d = block_device(q->disk);
        q->present = d && d->present;
        q->sectors = q->present ? d->sectors : 0;
        q->in_use = target_in_use(q->disk);
        q->media = source();
        q->state = state;
        strcopy(q->message,
                q->media ? "Native AHCI installation media ready"
                         : "Boot the ArkOS ISO with an AHCI optical drive",
                sizeof q->message);
        return 0;
    }
    if (q->op == ARK_INSTALL_BEGIN) {
        BlockDevice *d = block_device(q->disk);
        if (state == 1 || !d || !d->present || target_in_use(q->disk) || d->sectors != q->sectors ||
            d->sectors < 262144 || d->sectors > UINT32_MAX || strncmp(q->confirm, "ERASE", 6) ||
            !source())
            return -1;
        data_start = (blocks * 4 + 2047) & ~2047u;
        if (data_start + 16384 >= d->sectors - 65)
            return -28;
        target = q->disk;
        expected = d->sectors;
        at = 0;
        state = 1;
        failure[0] = 0;
        memset(buffer, 0, 512);
        if (!block_write(d, 0, 1, buffer) || !block_flush(d)) {
            state = 4;
            return -5;
        }
        serial_write("[installer] Confirmed native disk installation started\n");
    } else if (q->op == ARK_INSTALL_STEP && state == 1) {
        BlockDevice *d = block_device(target);
        unsigned n = blocks - at;
        if (n > 32)
            n = 32;
        bool okay = d && d->present && d->sectors == expected && !target_in_use(target) &&
                    ahci_cd_read(at, n, buffer);
        if (okay) {
            if (!at) {
                memcpy(mbr, buffer, 512);
                okay = block_write(d, 1, n * 4 - 1, buffer + 512);
            } else
                okay = block_write(d, at * 4, n * 4, buffer);
        }
        if (okay) {
            at += n;
            if (at == blocks) {
                okay = finish();
                if (okay) {
                    state = 2;
                    serial_write(
                        "[installer] Native installation complete; remove ISO and boot disk\n");
                }
            }
        }
        if (!okay) {
            state = 4;
            strcopy(failure, "I/O or partition validation failed; target is incomplete",
                    sizeof failure);
        }
    } else if (q->op == ARK_INSTALL_CANCEL && state == 1) {
        state = 3;
        strcopy(failure, "Cancelled; target disk is incomplete", sizeof failure);
    } else if (q->op != ARK_INSTALL_STATUS && q->op != ARK_INSTALL_STEP)
        return -22;
    q->state = state;
    q->disk = target;
    q->done = at;
    q->total = blocks;
    q->sectors = expected;
    strcopy(q->message,
            state == 2   ? "Installed. Remove ISO and reboot."
            : state == 1 ? "Copying native boot media and user data"
                         : failure,
            sizeof q->message);
    return state == 4 ? -5 : 0;
}
