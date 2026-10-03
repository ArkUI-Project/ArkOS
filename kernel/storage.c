/* ATA PIO + two-bank ArkFS. Original ArkOS code; MIT license. */
#include "ark.h"
#include "storage.h"

#define SECTOR_BYTES 512u
#define BANK_SECTORS 2080u
#define BANK0_LBA 8u
#define BANK1_LBA (BANK0_LBA + BANK_SECTORS)
#define RECORD_BYTES 136u
#define PAYLOAD_MAX (VFS_MAX_FILES * (RECORD_BYTES + VFS_FILE_CAP - 1u))
#define PAYLOAD_BUFFER (((PAYLOAD_MAX + 511u) / 512u) * 512u)
#define COMMIT_TAG 0x41524b31u
#define ATA_BASE 0x1f0
#define ATA_ALT 0x3f6
#define ATA_BSY 0x80
#define ATA_DRQ 0x08
#define ATA_ERR 0x01
#define ATA_DF 0x20

extern bool vfs_path_canonical(char out[128], const char *path);

static uint8_t payload[PAYLOAD_BUFFER] __attribute__((aligned(16)));
static uint8_t sector[SECTOR_BYTES] __attribute__((aligned(16)));
static uint32_t disk_sectors;
static bool mounted, dirty;
static int active_bank;
static uint64_t generation;
static const char *last_error = "No data disk mounted";
static const char *mount_status = "RAM only: no data disk";

#ifdef ARK_STORAGE_HOST_TEST
/* Deterministic test backend replaces only hardware transport, not ArkFS. */
extern bool ark_test_identify(uint32_t *sectors);
extern bool ark_test_transfer(uint32_t lba, uint32_t count, uint8_t *buffer, bool write);
extern bool ark_test_flush(void);
static bool ata_identify(void) {
    return ark_test_identify(&disk_sectors);
}
static bool ata_transfer(uint32_t lba, uint32_t count, uint8_t *buffer, bool write) {
    return ark_test_transfer(lba, count, buffer, write);
}
static bool ata_flush(void) {
    return ark_test_flush();
}
#else
#include "block.h"
static BlockDevice *data_disk;
static uint64_t data_offset;
static uint32_t part32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool find_data(BlockDevice *d) {
    uint8_t b[512];
    if (!d || !d->present || !block_read(d, 0, 1, b))
        return false;
    data_offset = 0;
    disk_sectors = d->sectors > UINT32_MAX ? UINT32_MAX : (uint32_t)d->sectors;
    if (!strncmp((char *)b, "ARKFS1", 6)) {
        data_disk = d;
        return true;
    }
    if (b[510] != 0x55 || b[511] != 0xaa)
        return false;
    for (unsigned i = 0; i < 4; i++) {
        uint8_t *e = b + 446 + i * 16;
        if (e[4] != 0xda)
            continue;
        uint32_t start = part32(e + 8), n = part32(e + 12);
        if (start < 2048 || n < 16384 || (uint64_t)start + n > d->sectors)
            continue;
        uint8_t sb[512];
        if (!block_read(d, start, 1, sb) || strncmp((char *)sb, "ARKFS1", 6))
            continue;
        data_disk = d;
        data_offset = start;
        disk_sectors = n;
        return true;
    }
    return false;
}

static bool ata_identify(void) {
    block_init();
    for (unsigned i = 0; i < 2; i++)
        if (find_data(block_device(i)))
            return true;
    last_error = "No ArkFS volume found; disks untouched";
    return false;
}
static bool ata_transfer(uint32_t lba, uint32_t count, uint8_t *buffer, bool write) {
    bool okay = write ? block_write(data_disk, data_offset + lba, count, buffer)
                      : block_read(data_disk, data_offset + lba, count, buffer);
    if (!okay)
        last_error = block_error();
    return okay;
}
static bool ata_flush(void) {
    bool okay = block_flush(data_disk);
    if (!okay)
        last_error = block_error();
    return okay;
}
#endif

static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t get64(const uint8_t *b) {
    return get32(b) | ((uint64_t)get32(b + 4) << 32);
}
static void put32(uint8_t *b, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i)
        b[i] = (uint8_t)(n >> (i * 8));
}
static void put64(uint8_t *b, uint64_t n) {
    put32(b, (uint32_t)n);
    put32(b + 4, (uint32_t)(n >> 32));
}
static bool same(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; ++i)
        if (x[i] != y[i])
            return false;
    return true;
}
static uint32_t crc32(const uint8_t *bytes, size_t length) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        c ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return c ^ 0xffffffffu;
}

static bool valid_superblock(void) {
    if (!same(sector, "ARKFS1\0\0", 8) || get32(sector + 8) != 1 ||
        get32(sector + 12) != SECTOR_BYTES || get32(sector + 16) > disk_sectors ||
        get32(sector + 16) < BANK1_LBA + BANK_SECTORS || get32(sector + 20) != BANK_SECTORS ||
        get32(sector + 24) != BANK0_LBA || get32(sector + 28) != BANK1_LBA ||
        get32(sector + 32) != VFS_MAX_FILES || get32(sector + 36) != 128 ||
        get32(sector + 40) != VFS_FILE_CAP || get32(sector + 508) != crc32(sector, 508)) {
        last_error = "Disk is not a supported ArkFS volume; left untouched";
        return false;
    }
    return true;
}

typedef struct {
    bool valid;
    uint64_t gen;
    uint32_t length, crc, count;
} BankInfo;
static BankInfo inspect_bank(unsigned bank) {
    BankInfo info = {0};
    if (!ata_transfer(bank ? BANK1_LBA : BANK0_LBA, 1, sector, false))
        return info;
    if (!same(sector, "ARKBANK1", 8) || get32(sector + 28) != 1 ||
        get32(sector + 32) != COMMIT_TAG || get32(sector + 508) != crc32(sector, 508))
        return info;
    info.gen = get64(sector + 8);
    info.length = get32(sector + 16);
    info.crc = get32(sector + 20);
    info.count = get32(sector + 24);
    info.valid = info.gen > 0 && info.length <= PAYLOAD_MAX && info.count <= VFS_MAX_FILES &&
                 info.length >= info.count * RECORD_BYTES;
    return info;
}

static bool valid_namespace(void) {
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (!vfs_files[i].used)
            continue;
        VFile *f = &vfs_files[i];
        if (!strcmp(f->name, "/")) {
            if (!f->is_dir)
                return false;
        } else {
            char parent[128];
            strcopy(parent, f->name, sizeof(parent));
            size_t n = strlen(parent);
            while (n > 1 && parent[n - 1] != '/')
                --n;
            if (n > 1)
                --n;
            parent[n] = 0;
            int p = -1;
            for (int j = 0; j < VFS_MAX_FILES; ++j)
                if (vfs_files[j].used && !strcmp(vfs_files[j].name, parent)) {
                    p = j;
                    break;
                }
            if (p < 0 || !vfs_files[p].is_dir)
                return false;
        }
        for (int j = i + 1; j < VFS_MAX_FILES; ++j)
            if (vfs_files[j].used && !strcmp(f->name, vfs_files[j].name))
                return false;
    }
    return true;
}

static bool load_bank(unsigned bank, const BankInfo *info) {
    if (!info->valid)
        return false;
    uint32_t sectors = (info->length + SECTOR_BYTES - 1) / SECTOR_BYTES;
    if (sectors && !ata_transfer((bank ? BANK1_LBA : BANK0_LBA) + 1, sectors, payload, false))
        return false;
    if (crc32(payload, info->length) != info->crc)
        return false;
    memset(vfs_files, 0, sizeof(vfs_files));
    uint32_t offset = 0;
    for (unsigned i = 0; i < info->count; ++i) {
        if (info->length - offset < RECORD_BYTES)
            return false;
        uint32_t kind = get32(payload + offset), size = get32(payload + offset + 4);
        if ((kind != 1 && kind != 2) || size >= VFS_FILE_CAP || (kind == 2 && size) ||
            size > info->length - offset - RECORD_BYTES)
            return false;
        const char *name = (const char *)payload + offset + 8;
        size_t length = 0;
        while (length < 128 && name[length])
            ++length;
        if (length == 128 || name[0] != '/')
            return false;
        char canonical[128];
        if (!vfs_path_canonical(canonical, name) || strcmp(canonical, name))
            return false;
        VFile *f = &vfs_files[i];
        f->used = true;
        f->is_dir = kind == 2;
        strcopy(f->name, name, sizeof(f->name));
        f->size = size;
        memcpy(f->data, payload + offset + RECORD_BYTES, size);
        f->data[size] = 0;
        /* Text VFS cannot round-trip embedded NULs, so reject such snapshots. */
        if (strlen(f->data) != size)
            return false;
        offset += RECORD_BYTES + size;
    }
    return offset == info->length && valid_namespace();
}

void storage_mark_dirty(void) {
    dirty = true;
}

bool storage_init(void) {
    mounted = false;
    dirty = false;
    active_bank = -1;
    generation = 0;
    disk_sectors = 0;
    if (!ata_identify()) {
        mount_status = "RAM only: no supported data disk";
        serial_write("[storage] ");
        serial_write(last_error);
        serial_write("; RAM only\n");
        return false;
    }
    if (!ata_transfer(0, 1, sector, false) || !valid_superblock()) {
        mount_status = "RAM only: data disk not mounted";
        serial_write("[storage] ");
        serial_write(last_error);
        serial_write("; RAM only\n");
        return false;
    }
    BankInfo banks[2] = {inspect_bank(0), inspect_bank(1)};
    unsigned first = banks[1].valid && (!banks[0].valid || banks[1].gen > banks[0].gen) ? 1 : 0;
    int chosen = -1;
    if (load_bank(first, &banks[first]))
        chosen = (int)first;
    else if (load_bank(1 - first, &banks[1 - first]))
        chosen = (int)(1 - first);
    if (chosen < 0) {
        memset(vfs_files, 0, sizeof(vfs_files));
        last_error = "Both ArkFS snapshots are invalid; disk left untouched";
        mount_status = "RAM only: ArkFS recovery required";
        serial_write("[storage] Both ArkFS banks invalid; disk untouched; RAM only\n");
        return false;
    }
    active_bank = chosen;
    generation = banks[chosen].gen;
    mounted = true;
    dirty = false;
    last_error = "";
    mount_status = "ArkFS: persistent block disk";
    char number[24];
    serial_write("[storage] ArkFS mounted; generation=");
    uint_to_str(generation, number);
    serial_write(number);
    serial_write("; physical MiB=");
    uint_to_str((uint64_t)disk_sectors * SECTOR_BYTES / (1024 * 1024), number);
    serial_write(number);
    serial_write("; logical bytes=");
    uint_to_str(storage_capacity_bytes(), number);
    serial_write(number);
    serial_write("\n");
    if (chosen != (int)first)
        serial_write("[storage] Recovered previous valid snapshot after corrupt newest bank\n");
    return true;
}

bool storage_mounted(void) {
    return mounted;
}
const char *storage_status(void) {
    return mount_status;
}
const char *storage_error(void) {
    return last_error;
}
uint64_t storage_capacity_bytes(void) {
    return (uint64_t)VFS_MAX_FILES * (VFS_FILE_CAP - 1u);
}
uint64_t storage_used_bytes(void) {
    uint64_t total = 0;
    for (int i = 0; i < VFS_MAX_FILES; ++i)
        if (vfs_files[i].used && !vfs_files[i].is_dir)
            total += vfs_files[i].size;
    return total;
}

bool storage_sync(void) {
    if (!mounted)
        return false;
    if (!dirty)
        return true;
    if (generation == UINT64_MAX) {
        last_error = "ArkFS generation exhausted";
        return false;
    }
    uint32_t length = 0, count = 0;
    if (!valid_namespace()) {
        last_error = "Invalid in-memory filesystem namespace";
        return false;
    }
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        const VFile *f = &vfs_files[i];
        if (!f->used)
            continue;
        if (f->size >= VFS_FILE_CAP || (f->is_dir && f->size) || f->name[0] != '/' ||
            strlen(f->data) != f->size) {
            last_error = "Invalid in-memory file record";
            return false;
        }
        memset(payload + length, 0, RECORD_BYTES);
        put32(payload + length, f->is_dir ? 2 : 1);
        put32(payload + length + 4, (uint32_t)f->size);
        memcpy(payload + length + 8, f->name, strlen(f->name));
        memcpy(payload + length + RECORD_BYTES, f->data, f->size);
        length += RECORD_BYTES + (uint32_t)f->size;
        ++count;
    }
    uint32_t blocks = (length + SECTOR_BYTES - 1) / SECTOR_BYTES;
    memset(payload + length, 0, blocks * SECTOR_BYTES - length);
    unsigned next = (unsigned)(1 - active_bank);
    uint32_t lba = next ? BANK1_LBA : BANK0_LBA;
    /* The active bank is never written. Persist payload before committing header. */
    if ((blocks && !ata_transfer(lba + 1, blocks, payload, true)) || !ata_flush())
        goto failed;
    memset(sector, 0, sizeof(sector));
    memcpy(sector, "ARKBANK1", 8);
    put64(sector + 8, generation + 1);
    put32(sector + 16, length);
    put32(sector + 20, crc32(payload, length));
    put32(sector + 24, count);
    put32(sector + 28, 1);
    put32(sector + 32, COMMIT_TAG);
    put32(sector + 508, crc32(sector, 508));
    if (!ata_transfer(lba, 1, sector, true) || !ata_flush())
        goto failed;
    active_bank = (int)next;
    ++generation;
    dirty = false;
    mount_status = "ArkFS: persistent block disk";
    last_error = "";
    serial_write("[storage] committed generation ");
    char number[24];
    uint_to_str(generation, number);
    serial_write(number);
    serial_write("\n");
    return true;
failed:
    mount_status = "ArkFS: sync failed; changes remain in RAM";
    serial_write("[storage] sync failed: ");
    serial_write(last_error);
    serial_write("\n");
    return false;
}

#ifndef ARK_STORAGE_HOST_TEST
bool storage_disk_in_use(unsigned id) {
    return mounted && data_disk && data_disk->id == id;
}
bool storage_volume_io(uint32_t lba, uint32_t n, void *buf, bool write) {
    return mounted && n && lba < disk_sectors && n <= disk_sectors - lba &&
           ata_transfer(lba, n, buf, write);
}
bool storage_volume_flush(void) {
    return mounted && ata_flush();
}
uint32_t storage_volume_sectors(void) {
    return mounted ? disk_sectors : 0;
}
/* Called only by the confirmed SYSTEM/admin installer on an unmounted target. */
bool storage_install_format(unsigned id, uint32_t start, uint32_t sectors) {
    if (storage_disk_in_use(id) || sectors < 16384)
        return false;
    BlockDevice *d = block_device(id);
    if (!d || !d->present || (uint64_t)start + sectors > d->sectors)
        return false;
    const char *old_error = last_error, *old_status = mount_status;
    BlockDevice *old_disk = data_disk;
    uint64_t old_off = data_offset, old_gen = generation;
    uint32_t old_size = disk_sectors;
    bool old_mount = mounted, old_dirty = dirty;
    int old_bank = active_bank;
    data_disk = d;
    data_offset = start;
    disk_sectors = sectors;
    mounted = true;
    generation = 0;
    active_bank = 1;
    dirty = true;
    memset(sector, 0, 512);
    memcpy(sector, "ARKFS1\0\0", 8);
    put32(sector + 8, 1);
    put32(sector + 12, 512);
    put32(sector + 16, sectors);
    put32(sector + 20, BANK_SECTORS);
    put32(sector + 24, BANK0_LBA);
    put32(sector + 28, BANK1_LBA);
    put32(sector + 32, VFS_MAX_FILES);
    put32(sector + 36, 128);
    put32(sector + 40, VFS_FILE_CAP);
    put32(sector + 508, crc32(sector, 508));
    bool okay = ata_transfer(0, 1, sector, true);
    memset(sector, 0, 512);
    for (unsigned i = 0; okay && i < 16; i++)
        okay = ata_transfer(8192 + i, 1, sector, true);
    if (okay)
        okay = storage_sync();
    if (okay) {
        dirty = true;
        okay = storage_sync();
    }
    data_disk = old_disk;
    data_offset = old_off;
    disk_sectors = old_size;
    mounted = old_mount;
    generation = old_gen;
    active_bank = old_bank;
    dirty = old_dirty;
    last_error = old_error;
    mount_status = old_status;
    return okay;
}
#endif
