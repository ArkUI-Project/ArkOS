#include "arkfs2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t *img;
static uint8_t *protect;
static uint32_t sectors;
static int fails, protect_hits;

static uint32_t crc32(const uint8_t *bytes, size_t length) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        c ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return c ^ 0xffffffffu;
}
static void put32(uint8_t *b, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) b[i] = (uint8_t)(n >> (8 * i));
}
static void put64(uint8_t *b, uint64_t n) {
    put32(b, (uint32_t)n);
    put32(b + 4, (uint32_t)(n >> 32));
}
static bool rd(uint32_t lba, uint32_t count, uint8_t *buf) {
    if ((uint64_t)lba + count > sectors) return false;
    memcpy(buf, img + (uint64_t)lba * 512, (size_t)count * 512);
    return true;
}
static bool wr(uint32_t lba, uint32_t count, const uint8_t *buf) {
    if ((uint64_t)lba + count > sectors) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (protect[lba + i]) protect_hits++;
    memcpy(img + (uint64_t)lba * 512, buf, (size_t)count * 512);
    return true;
}
static bool fl(void) { return true; }
static Arkfs2Disk disk;
static void open_disk(uint32_t mb) {
    sectors = mb * 2048u;
    img = calloc(1, (size_t)sectors * 512);
    protect = calloc(1, sectors);
    disk.read = rd; disk.write = wr; disk.flush = fl; disk.sectors = sectors;
}
static void close_disk(void) { free(img); free(protect); img = protect = 0; }
static void check(int cond, const char *msg) {
    if (!cond) { fprintf(stderr, "FAIL %s\n", msg); fails++; }
}
static void protect_block(uint32_t block) {
    if (!block) return;
    for (uint32_t s = 0; s < 8; ++s) protect[block * 8u + s] = 1;
}

static void write_v1(const char *note) {
    uint8_t sb[512] = {0};
    memcpy(sb, "ARKFS1\0\0", 8);
    put32(sb + 8, 1); put32(sb + 12, 512); put32(sb + 16, sectors);
    put32(sb + 20, 2080); put32(sb + 24, 8); put32(sb + 28, 2088);
    put32(sb + 32, 64); put32(sb + 36, 128); put32(sb + 40, 16384);
    put32(sb + 508, crc32(sb, 508));
    memcpy(img, sb, 512);
    uint8_t payload[512];
    memset(payload, 0, sizeof payload);
    uint32_t off = 0, count = 0;
    struct { uint32_t kind, size; const char *name; const char *data; } recs[] = {
        {2, 0, "/", 0},
        {2, 0, "/docs", 0},
        {1, (uint32_t)strlen(note), "/docs/note", note},
    };
    for (unsigned i = 0; i < 3; ++i) {
        put32(payload + off, recs[i].kind);
        put32(payload + off + 4, recs[i].size);
        memcpy(payload + off + 8, recs[i].name, strlen(recs[i].name));
        if (recs[i].size) memcpy(payload + off + 136, recs[i].data, recs[i].size);
        off += 136 + recs[i].size;
        count++;
    }
    uint8_t bank[512] = {0};
    memcpy(bank, "ARKBANK1", 8);
    put64(bank + 8, 1);
    put32(bank + 16, off);
    put32(bank + 20, crc32(payload, off));
    put32(bank + 24, count);
    put32(bank + 28, 1);
    put32(bank + 32, 0x41524b31u);
    put32(bank + 508, crc32(bank, 508));
    memcpy(img + 8 * 512, bank, 512);
    memcpy(img + 9 * 512, payload, off);
}

int main(void) {
    open_disk(16);
    check(arkfs2_format(&disk), "format");
    check(memcmp(img, "ARKFS2", 6) == 0, "magic v2");
    char card[128];
    check(arkfs2_card(card, sizeof card), "card");
    check(!strstr(card, "总共") && !strstr(card, "16KB") && !strstr(card, "64") && !strstr(card, "理论上限"), "card copy");
    check(arkfs2_used_bytes() + arkfs2_free_bytes() != (uint64_t)sectors * 512, "used+free != disk");
    check(arkfs2_free_bytes() % 4096 == 0, "free is blocks");
    const char *nul = "a\0b\0c";
    check(arkfs2_write("/pkg.bin", nul, 5), "nul write");
    uint8_t back[8];
    uint64_t n = 0;
    check(arkfs2_read("/pkg.bin", back, sizeof back, &n) && n == 5 && memcmp(back, nul, 5) == 0, "nul read");
    char big[20000];
    memset(big, 0x5a, sizeof big);
    check(arkfs2_write("/wide", big, sizeof big), "over 16KB");
    char *got = malloc(sizeof big);
    check(arkfs2_read("/wide", got, sizeof big, &n) && n == sizeof big && memcmp(got, big, sizeof big) == 0, "wide read");
    free(got);

    uint32_t before_bm = 0, before_ino = 0; uint64_t gen = 0;
    uint64_t free_live = arkfs2_free_bytes();
    check(arkfs2_meta(&before_bm, &before_ino, &gen), "meta");
    uint8_t bm_bytes[4096], ino_bytes[4096];
    memcpy(bm_bytes, img + (uint64_t)before_bm * 4096, 4096);
    memcpy(ino_bytes, img + (uint64_t)before_ino * 4096, 4096);
    uint32_t old_block = 0;
    check(arkfs2_block("/pkg.bin", 0, &old_block) && old_block, "old block");
    memset(protect, 0, sectors);
    protect_hits = 0;
    protect_block(before_bm);
    protect_block(before_ino);
    protect_block(old_block);
    arkfs2_fail_superblock(1);
    check(!arkfs2_write("/pkg.bin", "NEWFILE!!", 9), "commit dropped");
    check(protect_hits == 0, "old blocks not written in place");
    check(memcmp(img + (uint64_t)before_bm * 4096, bm_bytes, 4096) == 0, "bitmap bytes");
    check(memcmp(img + (uint64_t)before_ino * 4096, ino_bytes, 4096) == 0, "inode bytes");
    arkfs2_unmount();
    check(arkfs2_mount(&disk), "remount after drop");
    check(arkfs2_read("/pkg.bin", back, sizeof back, &n) && n == 5 && memcmp(back, nul, 5) == 0, "old file intact");
    check(!arkfs2_lookup("/nope", 0, 0, 0), "new file absent");
    uint32_t bm2 = 0, ino2 = 0; uint64_t gen2 = 0;
    check(arkfs2_meta(&bm2, &ino2, &gen2) && bm2 == before_bm && gen2 == gen, "superblock unchanged");
    check(arkfs2_free_bytes() == free_live, "dropped blocks returned");
    check(arkfs2_write("/other", "zzzz", 4), "reuse after drop");
    uint32_t other = 0;
    check(arkfs2_block("/other", 0, &other) && other != old_block, "old file block kept");
    check(arkfs2_read("/pkg.bin", back, sizeof back, &n) && n == 5, "still old");

    /* successful COW moves the bitmap block and leaves the old bytes */
    memcpy(bm_bytes, img + (uint64_t)bm2 * 4096, 4096);
    check(arkfs2_write("/other", "yyyyyyyy", 8), "second commit");
    uint32_t bm3 = 0;
    check(arkfs2_meta(&bm3, 0, 0) && bm3 != bm2, "bitmap relocated");
    check(memcmp(img + (uint64_t)bm2 * 4096, bm_bytes, 4096) == 0, "old bitmap not overwritten");

    /* corrupt the commit sector; published file remains */
    uint32_t journal = (uint32_t)img[20] | ((uint32_t)img[21] << 8) | ((uint32_t)img[22] << 16) | ((uint32_t)img[23] << 24);
    img[journal * 4096 + 10] ^= 0xff;
    arkfs2_unmount();
    check(arkfs2_mount(&disk), "mount bad commit crc");
    check(arkfs2_read("/other", back, sizeof back, &n) && n == 8 && memcmp(back, "yyyyyyyy", 8) == 0, "previous remains");

    /* directory entries do not cross a block */
    check(arkfs2_mkdir("/box"), "mkdir");
    for (int i = 0; i < 257; ++i) {
        char path[64];
        snprintf(path, sizeof path, "/box/n%02dabcdefg", i); /* 2 + 11 = name "nXXabcdefg" is 11 */
        /* n%02d is 3 chars + abcdefg is 7 = 10. Make 11: n%03d + abcdefgh = 3+8=11 if i<1000. */
        snprintf(path, sizeof path, "/box/n%03dabcdefg", i);
        check(arkfs2_write(path, "z", 1), path);
    }
    uint32_t d0 = 0, d1 = 0;
    check(arkfs2_block("/box", 0, &d0) && arkfs2_block("/box", 1, &d1) && d0 && d1, "dir blocks");
    uint8_t *b0 = img + (uint64_t)d0 * 4096;
    uint32_t off = 0, entries = 0;
    while (off + 5 <= 4096) {
        uint32_t eino = (uint32_t)b0[off] | ((uint32_t)b0[off+1]<<8) | ((uint32_t)b0[off+2]<<16) | ((uint32_t)b0[off+3]<<24);
        uint8_t elen = b0[off+4];
        if (!eino && !elen) break;
        check(elen && off + 5u + elen <= 4096, "entry fits in block");
        off += 5u + elen;
        entries++;
    }
    check(entries == 256 && off == 4096, "first dir block exactly full");

    uint32_t ino = 0; uint64_t sz = 0;
    check(arkfs2_lookup("/wide", &ino, &sz, 0), "lookup wide");
    uint64_t used = arkfs2_used_bytes();
    check(arkfs2_rename("/wide", "/wide2"), "rename");
    uint32_t ino2f = 0; uint64_t sz2 = 0;
    check(arkfs2_lookup("/wide2", &ino2f, &sz2, 0) && ino2f == ino && sz2 == sz, "same inode");
    check(arkfs2_used_bytes() == used, "rename does not copy");
    check(!arkfs2_lookup("/wide", 0, 0, 0), "old name gone");

    check(arkfs2_set_readonly("/wide2", true), "ro");
    uint8_t saved[16];
    memcpy(saved, img, 16);
    check(!arkfs2_write("/wide2", "nope", 4) && strcmp(arkfs2_error(), "当前文件只读") == 0, "ro string");

    uint8_t *snap = malloc((size_t)sectors * 512);
    memcpy(snap, img, (size_t)sectors * 512);
    check(!arkfs2_write("/bad\nname", "x", 1) && strcmp(arkfs2_error(), "名称无效") == 0, "control");
    check(memcmp(img, snap, (size_t)sectors * 512) == 0, "control writes nothing");
    check(!arkfs2_write("/\xff", "x", 1) && strcmp(arkfs2_error(), "名称无效") == 0, "bad utf8");
    check(memcmp(img, snap, (size_t)sectors * 512) == 0, "utf8 writes nothing");
    char longname[300];
    memset(longname, 'a', 256); longname[0] = '/'; longname[257] = 0;
    /* 256 a's after slash: component length 256 */
    memset(longname, 'a', 258); longname[0] = '/'; longname[257] = 0;
    check(!arkfs2_write(longname, "x", 1) && strcmp(arkfs2_error(), "文件名过长") == 0, "name length");
    check(memcmp(img, snap, (size_t)sectors * 512) == 0, "long name writes nothing");
    char *path = malloc(1200);
    path[0] = '/';
    uint32_t pos = 1;
    for (int i = 0; i < 205; ++i) {
        if (i) path[pos++] = '/';
        memcpy(path + pos, "abcd", 4); pos += 4;
    }
    path[pos] = 0;
    check(pos > 1023, "path fixture");
    check(!arkfs2_write(path, "x", 1) && strcmp(arkfs2_error(), "路径过长") == 0, "path length");
    check(memcmp(img, snap, (size_t)sectors * 512) == 0, "long path writes nothing");
    check(!arkfs2_write("/", "x", 1) && strcmp(arkfs2_error(), "名称不能为空") == 0, "empty");
    check(!arkfs2_write("", "x", 1) && strcmp(arkfs2_error(), "名称不能为空") == 0, "blank");
    char name255[257];
    name255[0] = '/';
    memset(name255 + 1, 'b', 255);
    name255[256] = 0;
    check(arkfs2_write(name255, "ok", 2), "255 byte name");
    free(path); free(snap);

    /* second indirect: 1032 blocks */
    size_t huge_n = 1032u * 4096u;
    uint8_t *huge = malloc(huge_n);
    for (size_t i = 0; i < huge_n; ++i) huge[i] = (uint8_t)(i * 17u);
    check(arkfs2_write("/huge", huge, huge_n), "second indirect write");
    uint8_t *huge2 = malloc(huge_n);
    check(arkfs2_read("/huge", huge2, huge_n, &n) && n == huge_n && memcmp(huge, huge2, huge_n) == 0, "second indirect read");
    uint32_t deep = 0;
    check(arkfs2_block("/huge", 1031, &deep) && deep, "block past first indirect");
    free(huge); free(huge2);
    close_disk();

    /* migration */
    open_disk(16);
    write_v1("hello");
    uint8_t *prefix = malloc(4168u * 512u);
    memcpy(prefix, img, 4168u * 512u);
    img[9 * 512 + 4] ^= 0x5a; /* break payload, crc no longer matches */
    uint8_t *broken = malloc((size_t)sectors * 512);
    memcpy(broken, img, (size_t)sectors * 512);
    check(!arkfs2_migrate_v1(&disk), "bad v1 does not migrate");
    check(memcmp(img, broken, (size_t)sectors * 512) == 0, "bad v1 untouched");
    memcpy(img, prefix, 4168u * 512);
    memset(img + 4168u * 512, 0, (size_t)sectors * 512 - 4168u * 512);
    arkfs2_fail_superblock(1);
    check(!arkfs2_migrate_v1(&disk), "migrate commit dropped");
    check(memcmp(img, prefix, 4168u * 512) == 0, "failed migrate stays v1");
    check(memcmp(img, "ARKFS1", 6) == 0, "still v1 magic");
    check(arkfs2_migrate_v1(&disk), "migrate");
    check(memcmp(img, "ARKFS2", 6) == 0, "switched");
    check(memcmp(img + 512, prefix + 512, 4167u * 512) == 0, "v1 banks kept");
    uint8_t note[8];
    check(arkfs2_read("/docs/note", note, sizeof note, &n) && n == 5 && memcmp(note, "hello", 5) == 0, "migrated file");
    check(arkfs2_used_bytes() + arkfs2_free_bytes() != (uint64_t)sectors * 512, "migrated gap");
    uint8_t *mid = malloc(4167u * 512);
    memcpy(mid, img + 512, 4167u * 512);
    check(arkfs2_write("/docs/extra", "more-bytes", 10), "write after migrate");
    check(memcmp(img + 512, mid, 4167u * 512) == 0, "v1 region not reused");
    free(mid); free(prefix); free(broken);
    close_disk();

    if (fails) {
        fprintf(stderr, "%d checks failed\n", fails);
        return 1;
    }
    puts("arkfs2 host test passed");
    return 0;
}

