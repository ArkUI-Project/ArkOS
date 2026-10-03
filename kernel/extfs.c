/* External volumes and a stable bounded metadata/text cache. MIT license. */
#include "extfs.h"
#include "block.h"
#include "fat32.h"
#include "ntfs.h"
static VFile files[EXTFS_MAX_ENTRIES];
static bool reserved[EXTFS_MAX_ENTRIES], loaded[EXTFS_MAX_ENTRIES];
static ExtVolumeInfo volumes[2];
static const char *error_text = "";
static const char *status_text = "No external FAT32/NTFS volume";
static bool device_read(void *ctx, uint64_t lba, uint32_t n, void *b) {
    return block_read(ctx, lba, n, b);
}
static bool device_write(void *ctx, uint64_t lba, uint32_t n, const void *b) {
    return block_write(ctx, lba, n, b);
}
static bool device_flush(void *ctx) {
    return block_flush(ctx);
}
static uint32_t u32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static bool fail(const char *s) {
    error_text = s;
    return false;
}
static int cached(const char *path) {
    for (int i = 0; i < EXTFS_MAX_ENTRIES; i++)
        if (reserved[i] && !strcmp(files[i].name, path))
            return i;
    return -1;
}
static int free_slot(void) {
    for (int i = 0; i < EXTFS_MAX_ENTRIES; i++)
        if (!reserved[i])
            return i;
    return -1;
}
static int cache(const char *path, bool dir, uint64_t size) {
    if (strlen(path) >= 128) {
        fail("External path exceeds 127 UTF-8 bytes");
        return -1;
    }
    int i = cached(path);
    if (i < 0)
        i = free_slot();
    if (i < 0) {
        fail("External cache limit reached (64 distinct paths per boot)");
        return -1;
    }
    if (!reserved[i]) {
        memset(&files[i], 0, sizeof(files[i]));
        strcopy(files[i].name, path, sizeof(files[i].name));
        reserved[i] = true;
    }
    if (files[i].size != size || files[i].is_dir != dir)
        loaded[i] = false;
    files[i].used = true;
    files[i].is_dir = dir;
    files[i].size = (size_t)size;
    return i;
}
static int volume_path(const char *path, const char **relative) {
    for (int i = 0; i < 2; i++) {
        const char *m = volumes[i].mountpoint;
        size_t n = strlen(m);
        if (!strncmp(path, m, n) && (!path[n] || path[n] == '/')) {
            *relative = path[n] ? path + n : "/";
            return volumes[i].mounted ? i : -1;
        }
    }
    return -1;
}
bool extfs_path(const char *p) {
    return p && !strncmp(p, "/mnt", 4) && (!p[4] || p[4] == '/');
}
bool extfs_read_bytes(int slot, uint64_t offset, void *buffer, size_t capacity, size_t *count) {
    *count = 0;
    if (slot < 0 || slot >= EXTFS_MAX_ENTRIES || !files[slot].used || files[slot].is_dir)
        return fail("Not a regular external file");
    const char *relative;
    int volume = volume_path(files[slot].name, &relative);
    if (volume < 0)
        return fail("External mount unavailable");
    uint64_t size = files[slot].size;
    if (offset >= size)
        return true;
    if (capacity > size - offset)
        capacity = (size_t)(size - offset);
    bool ok = volume == 0 ? fat32_read(relative, offset, buffer, capacity, count)
                          : ntfs_read(relative, offset, buffer, capacity, count);
    if (!ok)
        return fail(volume == 0 ? fat32_error() : ntfs_error());
    error_text = "";
    return true;
}
static bool stat_path(const char *p, bool *d, uint64_t *s) {
    if (!strcmp(p, "/mnt")) {
        *d = true;
        *s = 0;
        return true;
    }
    const char *relative;
    int vol = volume_path(p, &relative);
    if (vol < 0)
        return fail("External mount not available");
    if (vol == 0) {
        if (!fat32_stat(relative, d, s))
            return fail(fat32_error());
    } else if (!ntfs_stat(relative, d, s))
        return fail(ntfs_error());
    return true;
}
static void try_mount(BlockDevice *d, uint64_t start, uint64_t sectors, unsigned type) {
    if (!sectors || start >= d->sectors || sectors > d->sectors - start)
        return;
    if (!volumes[0].mounted && (type == 0 || type == 0x0b || type == 0x0c) &&
        fat32_mount(device_read, device_write, device_flush, d, start, sectors)) {
        volumes[0].mounted = true;
        volumes[0].read_only = !fat32_writable();
        volumes[0].capacity_bytes = sectors * 512;
    }
    if (!volumes[1].mounted && (type == 0 || type == 7) &&
        ntfs_mount(device_read, d, start, sectors)) {
        volumes[1].mounted = true;
        volumes[1].read_only = true;
        volumes[1].capacity_bytes = sectors * 512;
    }
}
void extfs_init(void) {
    memset(files, 0, sizeof(files));
    memset(reserved, 0, sizeof(reserved));
    memset(loaded, 0, sizeof(loaded));
    memset(volumes, 0, sizeof(volumes));
    volumes[0].mountpoint = "/mnt/fat32";
    volumes[1].mountpoint = "/mnt/ntfs";
    error_text = "";
    status_text = "No external FAT32/NTFS volume";
    cache("/mnt", true, 0);
    block_init();
    BlockDevice *d = block_device(1);
    if (!d || !d->present)
        return;
    uint8_t boot[512];
    if (!block_read(d, 0, 1, boot)) {
        error_text = block_error();
        return;
    }
    try_mount(d, 0, d->sectors, 0);
    if (!volumes[0].mounted && !volumes[1].mounted && boot[510] == 0x55 && boot[511] == 0xaa) {
        uint64_t starts[4], sizes[4];
        bool valid = true;
        for (unsigned i = 0; i < 4; i++) {
            const uint8_t *p = boot + 446 + i * 16;
            starts[i] = u32(p + 8);
            sizes[i] = u32(p + 12);
            if (p[4] && sizes[i] &&
                (!starts[i] || starts[i] >= d->sectors || sizes[i] > d->sectors - starts[i]))
                valid = false;
            for (unsigned j = 0; j < i; j++)
                if (p[4] && boot[446 + j * 16 + 4] && sizes[i] && sizes[j] &&
                    starts[i] < starts[j] + sizes[j] && starts[j] < starts[i] + sizes[i])
                    valid = false;
        }
        if (valid)
            for (unsigned i = 0; i < 4; i++)
                try_mount(d, starts[i], sizes[i], boot[446 + i * 16 + 4]);
        else
            error_text = "Invalid or overlapping MBR partitions; external disk untouched";
    }
    for (unsigned i = 0; i < 2; i++) {
        if (volumes[i].mounted)
            cache(volumes[i].mountpoint, true, 0);
    }
    if (volumes[0].mounted && volumes[1].mounted)
        status_text = "FAT32 mounted; NTFS read-only";
    else if (volumes[0].mounted)
        status_text = volumes[0].read_only ? "FAT32 read-only (dirty volume)" : "FAT32 read/write";
    else if (volumes[1].mounted)
        status_text = "NTFS read-only";
    else if (!error_text[0])
        error_text = "No supported external volume; disk untouched";
    serial_write("[extfs] ");
    serial_write(status_text);
    serial_write("\n");
}
int extfs_find(const char *p) {
    bool d;
    uint64_t s;
    if (!stat_path(p, &d, &s)) {
        int i = cached(p);
        if (i >= 0) {
            files[i].used = false;
            loaded[i] = false;
        }
        return -1;
    }
    return cache(p, d, s);
}
VFile *extfs_entry(int i) {
    return i >= 0 && i < EXTFS_MAX_ENTRIES ? &files[i] : 0;
}
bool extfs_read(int i) {
    if (i < 0 || i >= EXTFS_MAX_ENTRIES || !files[i].used || files[i].is_dir)
        return fail("External file is not readable");
    VFile *f = &files[i];
    if (f->size >= VFS_FILE_CAP)
        return fail("External file exceeds 16383-byte text editor limit");
    const char *relative;
    int v = volume_path(f->name, &relative);
    if (v < 0)
        return fail("External mount unavailable");
    size_t got = 0;
    bool ok = v == 0 ? fat32_read(relative, 0, f->data, f->size, &got)
                     : ntfs_read(relative, 0, f->data, f->size, &got);
    if (!ok)
        return fail(v == 0 ? fat32_error() : ntfs_error());
    if (got != f->size)
        return fail("External read returned incomplete data");
    f->data[got] = 0;
    if (strlen(f->data) != got) {
        f->data[0] = 0;
        return fail("Binary files cannot be opened in text applications");
    }
    loaded[i] = true;
    error_text = "";
    return true;
}
typedef struct {
    const char *parent;
    bool ok;
    bool seen[EXTFS_MAX_ENTRIES];
} Listing;
static bool emit(void *ctx, const char *name, bool directory, uint64_t size) {
    Listing *c = ctx;
    char path[128];
    size_t p = strlen(c->parent), n = strlen(name);
    if (!n || !strcmp(name, ".") || !strcmp(name, ".."))
        return true;
    for (size_t i = 0; i < n; i++)
        if (name[i] == '/')
            return true;
    if (p + 1 + n >= sizeof(path)) {
        c->ok = false;
        fail("External listing has paths longer than 127 bytes");
        return false;
    }
    strcopy(path, c->parent, sizeof(path));
    path[p++] = '/';
    memcpy(path + p, name, n + 1);
    int i = cache(path, directory, size);
    if (i < 0) {
        c->ok = false;
        return false;
    }
    c->seen[i] = true;
    return true;
}
static bool child(const char *p, const char *parent) {
    size_t n = strlen(parent);
    if (strncmp(p, parent, n) || p[n] != '/')
        return false;
    p += n + 1;
    if (!*p)
        return false;
    while (*p)
        if (*p++ == '/')
            return false;
    return true;
}
bool extfs_list(const char *p) {
    if (!strcmp(p, "/") || !strcmp(p, "/mnt"))
        return true;
    const char *r;
    int v = volume_path(p, &r);
    if (v < 0)
        return fail("External mount not available");
    Listing ctx = {.parent = p, .ok = true};
    bool ok = v == 0 ? fat32_list(r, emit, &ctx) : ntfs_list(r, emit, &ctx);
    if (!ok)
        return fail(v == 0 ? fat32_error() : ntfs_error());
    if (!ctx.ok)
        return false;
    for (int i = 0; i < EXTFS_MAX_ENTRIES; i++)
        if (files[i].used && child(files[i].name, p) && !ctx.seen[i]) {
            files[i].used = false;
            loaded[i] = false;
        }
    error_text = "";
    return true;
}
int extfs_create(const char *p, bool directory) {
    const char *r;
    int v = volume_path(p, &r);
    if (v != 0) {
        fail(v == 1 ? "NTFS is read-only" : "External destination unavailable");
        return -1;
    }
    if (cached(p) < 0 && free_slot() < 0) {
        fail("External cache full; no write performed");
        return -1;
    }
    if (!fat32_create(r, directory)) {
        fail(fat32_error());
        return -1;
    }
    return extfs_find(p);
}
bool extfs_write(int i, const char *text) {
    if (i < 0 || i >= EXTFS_MAX_ENTRIES || !files[i].used || files[i].is_dir || !text)
        return fail("Invalid external file handle");
    const char *r;
    int v = volume_path(files[i].name, &r);
    if (v != 0)
        return fail(v == 1 ? "NTFS is read-only" : "External mount unavailable");
    size_t n = 0;
    while (n < VFS_FILE_CAP && text[n])
        n++;
    if (n >= VFS_FILE_CAP)
        return fail("External text exceeds 16383 bytes");
    if (!fat32_write(r, text, n))
        return fail(fat32_error());
    memmove(files[i].data, text, n + 1);
    files[i].size = n;
    loaded[i] = true;
    error_text = "";
    return true;
}
bool extfs_remove(const char *p) {
    const char *r;
    int v = volume_path(p, &r);
    if (v != 0)
        return fail(v == 1 ? "NTFS is read-only" : "External mount unavailable");
    if (!fat32_remove(r))
        return fail(fat32_error());
    int i = cached(p);
    if (i >= 0) {
        files[i].used = false;
        loaded[i] = false;
    }
    error_text = "";
    return true;
}
bool extfs_rename(const char *a, const char *b) {
    const char *ra, *rb;
    int va = volume_path(a, &ra), vb = volume_path(b, &rb);
    if (va != 0 || vb != 0)
        return fail("External move requires two FAT32 file paths");
    if (cached(b) < 0 && free_slot() < 0)
        return fail("External cache full; move not performed");
    if (!fat32_rename(ra, rb))
        return fail(fat32_error());
    int i = cached(a);
    if (i >= 0) { /* Keep old handle reserved and invalid, never silently retarget open Notes. */
        files[i].used = false;
        loaded[i] = false;
    }
    return extfs_find(b) >= 0;
}
bool extfs_path_writable(const char *p) {
    const char *r;
    return volume_path(p, &r) == 0 && fat32_writable();
}
bool extfs_sync(void) {
    if (volumes[0].mounted && !fat32_sync())
        return fail(fat32_error());
    return true;
}
const char *extfs_status(void) {
    return status_text;
}
const char *extfs_error(void) {
    return error_text;
}
bool extfs_volume_info(unsigned i, ExtVolumeInfo *out) {
    if (i >= 2 || !out)
        return false;
    *out = volumes[i];
    if (i == 0 && out->mounted)
        out->read_only = !fat32_writable();
    return true;
}
