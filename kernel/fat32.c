/* Bounded native FAT32 implementation, written for ArkOS. MIT license. */
#include "fat32.h"
#define EOC 0x0ffffff8u
#define MAX_CHAIN 65536u
#define MAX_DIR_SECTORS 4096u
#define MAX_WRITE 16383u
static struct {
    FsReadBlocks read;
    FsWriteBlocks write;
    FsFlush flush;
    void *ctx;
    uint64_t start, sectors;
    uint32_t total, reserved, fats, fat_sectors, data, clusters, root, spc, active, fsinfo, backup,
        free_count, next_free;
    bool mounted, mirrored, writable;
} fs;
static const char *error_text = "FAT32 not mounted";
typedef struct {
    uint64_t lba;
    uint16_t offset;
} Location;
typedef struct {
    uint8_t raw[32];
    uint32_t first, size, parent;
    Location at, lfn[20];
    unsigned lfn_count;
    bool directory;
} Entry;
static uint8_t fat_cache[512];
static uint64_t fat_cache_lba = UINT64_MAX;
static uint16_t u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void p16(uint8_t *p, uint16_t n) {
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}
static void p32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (i * 8));
}
static bool fail(const char *s) {
    error_text = s;
    return false;
}
static bool rd(uint64_t l, uint32_t n, void *b) {
    if (!fs.read || l >= fs.sectors || n > fs.sectors - l)
        return fail("FAT32 block outside volume");
    if (!fs.read(fs.ctx, fs.start + l, n, b))
        return fail("FAT32 read I/O failed");
    return true;
}
static bool wr(uint64_t l, uint32_t n, const void *b) {
    if (!fs.mounted || !fs.writable)
        return fail("FAT32 volume is read-only");
    if (l >= fs.sectors || n > fs.sectors - l)
        return fail("FAT32 write outside volume");
    if (!fs.write(fs.ctx, fs.start + l, n, b)) {
        fs.writable = false;
        return fail("FAT32 write failed; volume switched read-only");
    }
    fat_cache_lba = UINT64_MAX;
    return true;
}
static bool flush(void) {
    if (!fs.flush || !fs.flush(fs.ctx)) {
        fs.writable = false;
        return fail("FAT32 flush failed; volume switched read-only");
    }
    return true;
}
static bool cluster_valid(uint32_t c) {
    return c >= 2 && c < fs.clusters + 2;
}
static uint64_t cluster_sector(uint32_t c) {
    return fs.data + (uint64_t)(c - 2) * fs.spc;
}
static bool getfat(uint32_t c, uint32_t *v) {
    if (c >= fs.clusters + 2)
        return fail("FAT32 invalid cluster index");
    uint64_t l = fs.reserved + (uint64_t)fs.active * fs.fat_sectors + c / 128;
    if (l != fat_cache_lba) {
        if (!rd(l, 1, fat_cache))
            return false;
        fat_cache_lba = l;
    }
    *v = u32(fat_cache + (c % 128) * 4) & 0x0fffffff;
    return true;
}
static bool setfat(uint32_t c, uint32_t value) {
    if (c >= fs.clusters + 2)
        return fail("FAT32 invalid allocation index");
    uint8_t b[512];
    for (uint32_t i = 0; i < fs.fats; i++) {
        if (!fs.mirrored && i != fs.active)
            continue;
        uint64_t l = fs.reserved + (uint64_t)i * fs.fat_sectors + c / 128;
        if (!rd(l, 1, b))
            return false;
        unsigned off = (c % 128) * 4;
        p32(b + off, (u32(b + off) & 0xf0000000) | (value & 0x0fffffff));
        if (!wr(l, 1, b))
            return false;
    }
    return true;
}
static bool next_cluster(uint32_t c, uint32_t *next) {
    if (!cluster_valid(c) || !getfat(c, next))
        return false;
    if (*next >= EOC)
        return true;
    if (!cluster_valid(*next))
        return fail("FAT32 broken cluster chain");
    return true;
}
static bool ascii_equal(const char *a, const char *b) {
    while (*a && *b) {
        unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return false;
    }
    return *a == *b;
}
static uint8_t checksum(const uint8_t *n) {
    uint8_t s = 0;
    for (unsigned i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) ? 128 : 0) + (s >> 1) + n[i]);
    return s;
}
static bool utf16_name(const uint16_t *s, unsigned count, char out[128]) {
    size_t n = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t c = s[i];
        if (!c || c == 0xffff)
            break;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (i + 1 >= count || s[i + 1] < 0xdc00 || s[i + 1] > 0xdfff)
                return false;
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        } else if (c >= 0xdc00 && c <= 0xdfff)
            return false;
        unsigned need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (n + need >= 128 || c < 32 || c == '/' || c == '\\')
            return false;
        if (need == 1)
            out[n++] = (char)c;
        else {
            if (need == 2)
                out[n++] = (char)(0xc0 | (c >> 6));
            else if (need == 3) {
                out[n++] = (char)(0xe0 | (c >> 12));
                out[n++] = (char)(0x80 | ((c >> 6) & 63));
            } else {
                out[n++] = (char)(0xf0 | (c >> 18));
                out[n++] = (char)(0x80 | ((c >> 12) & 63));
                out[n++] = (char)(0x80 | ((c >> 6) & 63));
            }
            out[n++] = (char)(0x80 | (c & 63));
        }
    }
    out[n] = 0;
    return n > 0;
}
static bool to_utf16(const char *s, uint16_t out[128], unsigned *count) {
    unsigned n = 0;
    while (*s) {
        uint32_t c = (uint8_t)*s++;
        unsigned more = 0;
        uint32_t min = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            c &= 31;
            more = 1;
            min = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            c &= 15;
            more = 2;
            min = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            c &= 7;
            more = 3;
            min = 0x10000;
        } else if (c >= 0x80)
            return false;
        while (more--) {
            uint8_t q = (uint8_t)*s++;
            if ((q & 0xc0) != 0x80)
                return false;
            c = (c << 6) | (q & 63);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || c < 32 || c == '"' ||
            c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' ||
            c == '|')
            return false;
        if (c >= 0x10000) {
            if (n + 2 >= 128)
                return false;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xd800 + (c >> 10));
            out[n++] = (uint16_t)(0xdc00 + (c & 1023));
        } else {
            if (n + 1 >= 128)
                return false;
            out[n++] = (uint16_t)c;
        }
    }
    if (!n || out[n - 1] == ' ' || out[n - 1] == '.')
        return false;
    *count = n;
    return true;
}
static void short_name(const uint8_t *b, char *out) {
    unsigned n = 0;
    for (unsigned i = 0; i < 8 && b[i] != ' '; i++) {
        unsigned char c = b[i];
        if (i == 0 && c == 5)
            c = 0xe5;
        if ((b[12] & 8) && c >= 'A' && c <= 'Z')
            c += 32;
        out[n++] = c < 128 ? (char)c : '?';
    }
    if (b[8] != ' ') {
        out[n++] = '.';
        for (unsigned i = 8; i < 11 && b[i] != ' '; i++) {
            unsigned char c = b[i];
            if ((b[12] & 16) && c >= 'A' && c <= 'Z')
                c += 32;
            out[n++] = c < 128 ? (char)c : '?';
        }
    }
    out[n] = 0;
}
/* scan returns true for successful traversal (found may remain false). */
static bool scan(uint32_t directory, const char *target, Entry *result, bool *found, FsEmit emit,
                 void *ctx) {
    uint32_t c = directory;
    unsigned budget = 0;
    uint16_t lname[260];
    Location locations[20];
    unsigned pending = 0, total = 0;
    uint8_t sum = 0;
    bool lvalid = false;
    *found = false;
    for (unsigned chain = 0; chain < MAX_CHAIN && cluster_valid(c); chain++) {
        for (unsigned sec = 0; sec < fs.spc; sec++) {
            if (++budget > MAX_DIR_SECTORS)
                return fail("FAT32 directory scan limit reached");
            uint64_t lba = cluster_sector(c) + sec;
            uint8_t b[512];
            if (!rd(lba, 1, b))
                return false;
            for (unsigned off = 0; off < 512; off += 32) {
                uint8_t *e = b + off;
                if (!e[0])
                    return true;
                if (e[0] == 0xe5) {
                    lvalid = false;
                    continue;
                }
                if (e[11] == 0x0f) {
                    unsigned seq = e[0] & 31;
                    if (e[0] & 0x40) {
                        lvalid = seq > 0 && seq <= 20;
                        pending = total = seq;
                        sum = e[13];
                        memset(lname, 0xff, sizeof(lname));
                    }
                    if (!lvalid || seq != pending || e[13] != sum || e[12] || u16(e + 26) ||
                        e[0] & 0x80) {
                        lvalid = false;
                        continue;
                    }
                    const unsigned positions[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    for (unsigned j = 0; j < 13; j++)
                        lname[(seq - 1) * 13 + j] = u16(e + positions[j]);
                    locations[seq - 1] = (Location){lba, (uint16_t)off};
                    --pending;
                    continue;
                }
                char name[128], shortn[16];
                short_name(e, shortn);
                bool longok =
                    lvalid && !pending && checksum(e) == sum && utf16_name(lname, total * 13, name);
                if (!longok)
                    strcopy(name, shortn, sizeof(name));
                lvalid = false;
                if ((e[11] & 8) || !strcmp(shortn, ".") || !strcmp(shortn, ".."))
                    continue;
                if (target && (ascii_equal(name, target) || ascii_equal(shortn, target))) {
                    memset(result, 0, sizeof(*result));
                    memcpy(result->raw, e, 32);
                    result->first = ((uint32_t)u16(e + 20) << 16) | u16(e + 26);
                    result->first &= 0x0fffffff;
                    result->size = u32(e + 28);
                    result->directory = (e[11] & 16) != 0;
                    result->parent = directory;
                    result->at = (Location){lba, (uint16_t)off};
                    if (longok) {
                        result->lfn_count = total;
                        memcpy(result->lfn, locations, total * sizeof(Location));
                    }
                    *found = true;
                    return true;
                }
                if (emit && !emit(ctx, name, (e[11] & 16) != 0, u32(e + 28)))
                    return true;
            }
        }
        uint32_t next;
        if (!next_cluster(c, &next))
            return false;
        if (next >= EOC)
            return true;
        c = next;
    }
    return fail("FAT32 cyclic or oversized directory chain");
}
static bool lookup(const char *path, Entry *out) {
    if (!fs.mounted)
        return fail("FAT32 not mounted");
    if (!path || path[0] != '/')
        return fail("FAT32 requires absolute volume path");
    memset(out, 0, sizeof(*out));
    out->directory = true;
    out->first = fs.root;
    const char *p = path;
    unsigned depth = 0;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        if (++depth > 32)
            return fail("FAT32 path too deep");
        char part[128];
        unsigned n = 0;
        while (*p && *p != '/') {
            if (n + 1 >= sizeof(part))
                return fail("FAT32 name too long");
            part[n++] = *p++;
        }
        part[n] = 0;
        if (!strcmp(part, ".") || !strcmp(part, ".."))
            return fail("FAT32 requires normalized paths");
        if (!out->directory)
            return fail("FAT32 parent is not a directory");
        uint32_t dir = out->first;
        bool found;
        if (!scan(dir, part, out, &found, 0, 0))
            return false;
        if (!found)
            return fail("FAT32 path not found");
        if (out->directory && !cluster_valid(out->first))
            return fail("FAT32 invalid directory cluster");
        if (!out->directory && out->size && !cluster_valid(out->first))
            return fail("FAT32 invalid data cluster");
    }
    return true;
}
static bool parent_name(const char *path, uint32_t *parent, char name[128]) {
    if (!path || path[0] != '/' || strlen(path) >= 128)
        return fail("FAT32 invalid path");
    char buf[128];
    strcopy(buf, path, sizeof(buf));
    size_t n = strlen(buf);
    while (n && buf[n - 1] != '/')
        --n;
    if (!buf[n])
        return fail("FAT32 empty file name");
    strcopy(name, buf + n, 128);
    if (n <= 1)
        buf[1] = 0;
    else
        buf[n - 1] = 0;
    Entry e;
    if (!lookup(buf, &e) || !e.directory)
        return fail("FAT32 parent directory not found");
    uint16_t encoded[128];
    unsigned count;
    if (!to_utf16(name, encoded, &count))
        return fail("FAT32 invalid Unicode file name");
    *parent = e.first;
    return true;
}
static bool modify(Location where, const uint8_t entry[32]) {
    uint8_t b[512];
    if (where.offset > 480 || where.offset % 32 || !rd(where.lba, 1, b))
        return false;
    memcpy(b + where.offset, entry, 32);
    return wr(where.lba, 1, b);
}
static bool info_unknown(void) {
    if (fs.fsinfo && fs.fsinfo < fs.reserved) {
        uint8_t b[512];
        if (!rd(fs.fsinfo, 1, b))
            return false;
        if (u32(b) == 0x41615252 && u32(b + 484) == 0x61417272 && u32(b + 508) == 0xaa550000) {
            p32(b + 488, fs.free_count);
            p32(b + 492, fs.next_free);
            if (!wr(fs.fsinfo, 1, b))
                return false;
            if (fs.backup && fs.backup + fs.fsinfo < fs.reserved &&
                !wr(fs.backup + fs.fsinfo, 1, b))
                return false;
        }
    }
    return true;
}
static bool chain_collect(uint32_t first, uint32_t *clusters, unsigned cap, unsigned *count) {
    *count = 0;
    if (!first)
        return true;
    uint32_t c = first;
    while (cluster_valid(c)) {
        if (*count >= cap)
            return fail("FAT32 chain exceeds mutation bound");
        for (unsigned i = 0; i < *count; i++)
            if (clusters[i] == c)
                return fail("FAT32 cyclic file chain");
        clusters[(*count)++] = c;
        uint32_t next;
        if (!next_cluster(c, &next))
            return false;
        if (next >= EOC)
            return true;
        c = next;
    }
    return fail("FAT32 corrupt file chain");
}
static bool allocate(unsigned count, uint32_t *out) {
    unsigned got = 0;
    for (uint32_t c = 2; c < fs.clusters + 2 && c < 1048578 && got < count; c++) {
        uint32_t v;
        if (!getfat(c, &v))
            return false;
        if (!v)
            out[got++] = c;
    }
    if (got != count)
        return fail("FAT32 disk full or allocation scan bound reached");
    for (unsigned i = 0; i < count; i++)
        if (!setfat(out[i], i + 1 < count ? out[i + 1] : 0x0fffffff))
            return false;
    if (fs.free_count != 0xffffffff) {
        if (fs.free_count < count)
            fs.free_count = 0xffffffff;
        else
            fs.free_count -= count;
    }
    if (count)
        fs.next_free = out[count - 1] + 1 < fs.clusters + 2 ? out[count - 1] + 1 : 2;
    return true;
}
static bool free_clusters(const uint32_t *c, unsigned count) {
    for (unsigned i = 0; i < count; i++)
        if (!setfat(c[i], 0))
            return false;
    if (fs.free_count != 0xffffffff) {
        if (count > fs.clusters - fs.free_count)
            fs.free_count = 0xffffffff;
        else
            fs.free_count += count;
    }
    if (count && (fs.next_free == 0xffffffff || c[0] < fs.next_free))
        fs.next_free = c[0];
    return true;
}
static bool zero_cluster(uint32_t c) {
    uint8_t z[512];
    memset(z, 0, sizeof(z));
    for (unsigned s = 0; s < fs.spc; s++)
        if (!wr(cluster_sector(c) + s, 1, z))
            return false;
    return true;
}
/* Locate contiguous free directory entries, extending a bounded directory chain. */
static bool free_slots(uint32_t directory, unsigned need, Location *out) {
    unsigned run = 0, budget = 0;
    uint32_t c = directory;
    bool after_end = false;
    for (unsigned chain = 0; chain < MAX_CHAIN; chain++) {
        for (unsigned s = 0; s < fs.spc; s++) {
            if (++budget > MAX_DIR_SECTORS)
                return fail("FAT32 directory capacity limit");
            uint64_t l = cluster_sector(c) + s;
            uint8_t b[512];
            if (!rd(l, 1, b))
                return false;
            for (unsigned off = 0; off < 512; off += 32) {
                if (!b[off])
                    after_end = true;
                if (after_end || b[off] == 0xe5) {
                    out[run++] = (Location){l, (uint16_t)off};
                    if (run == need)
                        return true;
                } else
                    run = 0;
            }
        }
        uint32_t next;
        if (!next_cluster(c, &next))
            return false;
        if (next >= EOC) {
            uint32_t fresh;
            if (!allocate(1, &fresh) || !zero_cluster(fresh) || !flush() || !setfat(c, fresh) ||
                !flush())
                return false;
            next = fresh;
            after_end = true;
        }
        c = next;
    }
    return fail("FAT32 directory chain limit");
}
static bool short_exists(uint32_t directory, const uint8_t name[11]) {
    char printable[16];
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    memcpy(e, name, 11);
    short_name(e, printable);
    Entry found;
    bool yes;
    if (!scan(directory, printable, &found, &yes, 0, 0))
        return true;
    return yes;
}
static bool new_record(uint32_t directory, const char *name, const uint8_t template[32],
                       Entry *out) {
    uint16_t unicode[128];
    unsigned count;
    if (!to_utf16(name, unicode, &count))
        return fail("FAT32 invalid long name");
    unsigned lfns = (count + 12) / 13;
    Location slots[11];
    if (lfns > 10)
        return fail("FAT32 name too long");
    uint8_t shortn[11];
    bool available = false; /* Stable unique aliases; true Unicode spelling lives in LFN entries. */
    for (unsigned serial = 1; serial <= 999999; serial++) {
        memset(shortn, ' ', 11);
        shortn[0] = 'A';
        shortn[1] = 'R';
        shortn[2] = 'K';
        shortn[3] = '~';
        unsigned n = serial;
        for (int i = 7; i >= 4; i--) {
            shortn[i] = (uint8_t)('0' + n % 10);
            n /= 10;
        }
        if (serial > 9999)
            break;
        shortn[8] = 'D';
        shortn[9] = 'A';
        shortn[10] = 'T';
        if (!short_exists(directory, shortn)) {
            available = true;
            break;
        }
    }
    if (!available)
        return fail("FAT32 short-name namespace exhausted");
    if (!free_slots(directory, lfns + 1, slots))
        return false;
    uint8_t sum = checksum(shortn);
    const unsigned positions[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (unsigned i = 0; i < lfns; i++) {
        unsigned seq = lfns - i;
        uint8_t e[32];
        memset(e, 0xff, 32);
        e[0] = (uint8_t)(seq | (i ? 0 : 0x40));
        e[11] = 15;
        e[12] = 0;
        e[13] = sum;
        p16(e + 26, 0);
        for (unsigned j = 0; j < 13; j++) {
            unsigned at = (seq - 1) * 13 + j;
            p16(e + positions[j], at < count ? unicode[at] : at == count ? 0 : 0xffff);
        }
        if (!modify(slots[i], e))
            return false;
    }
    if (!flush())
        return false;
    uint8_t e[32];
    memcpy(e, template, 32);
    memcpy(e, shortn, 11);
    e[12] = 0; /* Deterministic valid DOS timestamp: 2026-01-01 00:00. */
    p16(e + 16, 0x5c21);
    p16(e + 18, 0x5c21);
    p16(e + 24, 0x5c21);
    if (!modify(slots[lfns], e) || !flush())
        return false;
    memset(out, 0, sizeof(*out));
    memcpy(out->raw, e, 32);
    out->at = slots[lfns];
    out->parent = directory;
    out->first = ((uint32_t)u16(e + 20) << 16) | u16(e + 26);
    out->size = u32(e + 28);
    out->directory = (e[11] & 16) != 0;
    out->lfn_count = lfns;
    for (unsigned i = 0; i < lfns; i++)
        out->lfn[i] = slots[i];
    return true;
}

bool fat32_mount(FsReadBlocks read, FsWriteBlocks write, FsFlush fl, void *ctx, uint64_t start,
                 uint64_t sectors) {
    memset(&fs, 0, sizeof(fs));
    fat_cache_lba = UINT64_MAX;
    fs.read = read;
    fs.write = write;
    fs.flush = fl;
    fs.ctx = ctx;
    fs.start = start;
    fs.sectors = sectors;
    uint8_t b[512];
    if (!sectors || !rd(0, 1, b))
        return false;
    uint32_t spc = b[13], reserved = u16(b + 14), fats = b[16], fat = u32(b + 36),
             total = u32(b + 32);
    uint16_t flags = u16(b + 40);
    if (b[510] != 0x55 || b[511] != 0xaa || u16(b + 11) != 512 || !spc || spc > 128 ||
        (spc & (spc - 1)) || !reserved || !fats || fats > 2 || u16(b + 17) || u16(b + 19) ||
        u16(b + 22) || u16(b + 42) || !fat || total > sectors ||
        (uint64_t)reserved + (uint64_t)fats * fat >= total)
        return fail("Not a supported FAT32 volume");
    uint32_t data = reserved + fats * fat, clusters = (total - data) / spc;
    if (clusters < 65525 || clusters >= 0x0ffffff5 || (uint64_t)fat * 128 < clusters + 2)
        return fail("Invalid FAT32 cluster geometry");
    fs.total = total;
    fs.sectors = total;
    fs.reserved = reserved;
    fs.fats = fats;
    fs.fat_sectors = fat;
    fs.spc = spc;
    fs.data = data;
    fs.clusters = clusters;
    fs.root = u32(b + 44) & 0x0fffffff;
    fs.active = (flags & 128) ? flags & 15 : 0;
    fs.mirrored = !(flags & 128);
    fs.fsinfo = u16(b + 48);
    fs.backup = u16(b + 50);
    if (!cluster_valid(fs.root) || fs.active >= fs.fats)
        return fail("Invalid FAT32 root or active FAT");
    uint32_t f0, f1;
    if (!getfat(0, &f0) || !getfat(1, &f1) || (f0 & 0x0ffffff8) != 0x0ffffff8)
        return fail("Invalid FAT32 reserved allocation entries");
    fs.free_count = fs.next_free = 0xffffffff;
    if (fs.fsinfo && fs.fsinfo < fs.reserved) {
        uint8_t info[512];
        if (!rd(fs.fsinfo, 1, info))
            return false;
        if (u32(info) == 0x41615252 && u32(info + 484) == 0x61417272 &&
            u32(info + 508) == 0xaa550000) {
            uint32_t free = u32(info + 488), next = u32(info + 492);
            if (free <= fs.clusters)
                fs.free_count = free;
            if (cluster_valid(next))
                fs.next_free = next;
        }
    }
    fs.mounted = true;
    fs.writable = write && fl && (f1 & 0x0c000000) == 0x0c000000;
    error_text = fs.writable ? "" : "FAT32 dirty/error flag: mounted read-only";
    return true;
}
bool fat32_stat(const char *path, bool *directory, uint64_t *size) {
    Entry e;
    if (!lookup(path, &e))
        return false;
    *directory = e.directory;
    *size = e.size;
    return true;
}
bool fat32_list(const char *path, FsEmit emit, void *ctx) {
    Entry e;
    if (!lookup(path, &e) || !e.directory)
        return fail("FAT32 not a directory");
    bool found;
    return scan(e.first, 0, 0, &found, emit, ctx);
}
bool fat32_read(const char *path, uint64_t offset, void *buffer, size_t bytes, size_t *read_count) {
    *read_count = 0;
    Entry e;
    if (!lookup(path, &e) || e.directory)
        return fail("FAT32 not a regular file");
    if (offset >= e.size || !bytes)
        return true;
    if (bytes > e.size - offset)
        bytes = (size_t)(e.size - offset);
    uint64_t cbytes = (uint64_t)fs.spc * 512;
    uint32_t c = e.first;
    unsigned steps = 0;
    while (offset >= cbytes) {
        uint32_t next;
        if (++steps > MAX_CHAIN || !next_cluster(c, &next) || next >= EOC)
            return fail("FAT32 short or cyclic data chain");
        c = next;
        offset -= cbytes;
    }
    uint8_t *out = buffer;
    while (bytes) {
        if (!cluster_valid(c) || ++steps > MAX_CHAIN)
            return fail("FAT32 invalid data chain");
        for (unsigned s = (unsigned)(offset / 512); s < fs.spc && bytes; s++) {
            uint8_t b[512];
            if (!rd(cluster_sector(c) + s, 1, b))
                return false;
            unsigned skip = (unsigned)(offset % 512);
            size_t n = 512 - skip;
            if (n > bytes)
                n = bytes;
            memcpy(out, b + skip, n);
            out += n;
            bytes -= n;
            *read_count += n;
            offset = 0;
        }
        if (bytes) {
            uint32_t next;
            if (!next_cluster(c, &next) || next >= EOC)
                return fail("FAT32 file shorter than directory size");
            c = next;
        }
    }
    return true;
}
bool fat32_create(const char *path, bool directory) {
    if (!fs.writable)
        return fail("FAT32 volume is read-only");
    Entry exists;
    if (lookup(path, &exists))
        return exists.directory == directory;
    uint32_t parent;
    char name[128];
    if (!parent_name(path, &parent, name))
        return false;
    bool present;
    if (!scan(parent, name, &exists, &present, 0, 0))
        return false;
    if (present)
        return exists.directory == directory;
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[11] = directory ? 16 : 32;
    uint32_t cluster = 0;
    if (directory) {
        if (!allocate(1, &cluster) || !zero_cluster(cluster))
            return false;
        uint8_t b[512];
        memset(b, 0, sizeof(b));
        memset(b, ' ', 11);
        b[0] = '.';
        b[11] = 16;
        p16(b + 20, (uint16_t)(cluster >> 16));
        p16(b + 26, (uint16_t)cluster);
        memset(b + 32, ' ', 11);
        b[32] = '.';
        b[33] = '.';
        b[43] = 16;
        uint32_t up = parent == fs.root ? 0 : parent;
        p16(b + 52, (uint16_t)(up >> 16));
        p16(b + 58, (uint16_t)up);
        if (!wr(cluster_sector(cluster), 1, b) || !flush())
            return false;
        p16(e + 20, (uint16_t)(cluster >> 16));
        p16(e + 26, (uint16_t)cluster);
    }
    Entry created;
    if (!new_record(parent, name, e, &created))
        return false;
    return info_unknown() && flush();
}
bool fat32_write(const char *path, const void *buffer, size_t bytes) {
    if (!fs.writable)
        return fail("FAT32 volume is read-only");
    if (bytes > MAX_WRITE)
        return fail("FAT32 editor limit is 16383 bytes");
    Entry e;
    if (!lookup(path, &e) || e.directory || (e.raw[11] & 1))
        return fail("FAT32 file is missing, directory or read-only");
    uint32_t old[4096];
    unsigned oldcount;
    if (!chain_collect(e.first, old, 4096, &oldcount))
        return false;
    unsigned needed = (unsigned)((bytes + (size_t)fs.spc * 512 - 1) / ((size_t)fs.spc * 512));
    uint32_t fresh[32];
    if (needed > 32 || !allocate(needed, fresh))
        return false;
    const uint8_t *src = buffer;
    size_t left = bytes;
    for (unsigned i = 0; i < needed; i++)
        for (unsigned s = 0; s < fs.spc; s++) {
            uint8_t b[512];
            size_t n = left > 512 ? 512 : left;
            memset(b, 0, 512);
            if (n)
                memcpy(b, src, n);
            if (!wr(cluster_sector(fresh[i]) + s, 1, b))
                return false;
            src += n;
            left -= n;
        }
    if (!flush())
        return false;
    uint32_t first = needed ? fresh[0] : 0;
    p16(e.raw + 20, (uint16_t)(first >> 16));
    p16(e.raw + 26, (uint16_t)first);
    p32(e.raw + 28, (uint32_t)bytes);
    e.raw[11] |= 32;
    if (!modify(e.at, e.raw) || !flush())
        return false;
    if (!free_clusters(old, oldcount) || !info_unknown() || !flush())
        return false;
    error_text = "";
    return true;
}
static bool any_child(void *ctx, const char *name, bool directory, uint64_t size) {
    (void)name;
    (void)directory;
    (void)size;
    *(bool *)ctx = true;
    return false;
}
bool fat32_remove(const char *path) {
    if (!fs.writable)
        return fail("FAT32 volume is read-only");
    if (!strcmp(path, "/"))
        return fail("Cannot remove FAT32 root");
    Entry e;
    if (!lookup(path, &e) || (e.raw[11] & 1))
        return fail("FAT32 path unavailable or read-only");
    if (e.directory) {
        bool found = false, dummy;
        if (!scan(e.first, 0, 0, &dummy, any_child, &found))
            return false;
        if (found)
            return fail("FAT32 directory not empty");
    }
    uint32_t chain[4096];
    unsigned count;
    if (!chain_collect(e.first, chain, 4096, &count))
        return false;
    e.raw[0] = 0xe5;
    if (!modify(e.at, e.raw) || !flush())
        return false;
    for (unsigned i = 0; i < e.lfn_count; i++) {
        uint8_t b[512];
        Location a = e.lfn[i];
        if (!rd(a.lba, 1, b))
            return false;
        b[a.offset] = 0xe5;
        if (!wr(a.lba, 1, b))
            return false;
    }
    return free_clusters(chain, count) && info_unknown() && flush();
}
bool fat32_rename(const char *oldpath, const char *newpath) {
    if (!fs.writable)
        return fail("FAT32 volume is read-only");
    Entry e, other;
    if (!lookup(oldpath, &e) || e.directory)
        return fail("FAT32 move currently supports regular files only");
    if (lookup(newpath, &other))
        return fail("FAT32 destination exists");
    if (e.size > MAX_WRITE)
        return fail("FAT32 move file exceeds editor limit");
    static uint8_t copy[MAX_WRITE];
    size_t n;
    if (!fat32_read(oldpath, 0, copy, e.size, &n) || n != e.size || !fat32_create(newpath, false) ||
        !fat32_write(newpath, copy, n))
        return false;
    return fat32_remove(oldpath);
}
bool fat32_sync(void) {
    return !fs.mounted || flush();
}
bool fat32_writable(void) {
    return fs.mounted && fs.writable;
}
const char *fat32_error(void) {
    return error_text;
}
