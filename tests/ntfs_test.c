#include "ntfs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *image;
static size_t image_bytes;
static uint64_t offset_sectors;
static unsigned reads;
static bool disk(void *ctx, uint64_t lba, uint32_t sectors, void *out) {
    (void)ctx;
    ++reads;
    assert(lba >= offset_sectors);
    uint64_t off = (lba - offset_sectors) * 512;
    assert(off <= image_bytes && (uint64_t)sectors * 512 <= image_bytes - off);
    memcpy(out, image + off, (size_t)sectors * 512);
    return true;
}
static uint16_t u16(const unsigned char *p) {
    return p[0] | (uint16_t)p[1] << 8;
}
static uint32_t u32(const unsigned char *p) {
    return u16(p) | (uint32_t)u16(p + 2) << 16;
}
static uint64_t u64(const unsigned char *p) {
    return u32(p) | (uint64_t)u32(p + 4) << 32;
}
static unsigned attr(unsigned char *rec, unsigned type) {
    unsigned pos = u16(rec + 20);
    while (pos + 16 < 1024 && u32(rec + pos) != UINT32_MAX) {
        if (u32(rec + pos) == type && rec[pos + 9] == 0)
            return pos;
        unsigned n = u32(rec + pos + 4);
        if (n < 24 || n > 1024 - pos)
            break;
        pos += n;
    }
    return 0;
}
static void check(bool ok) {
    if (!ok)
        fprintf(stderr, "NTFS error: %s\n", ntfs_error());
    assert(ok);
}
static bool mounted(void) {
    return ntfs_mount(disk, 0, offset_sectors, image_bytes / 512);
}
static unsigned listed;
static bool saw_utf8, saw_large, saw_extend;
static bool emit(void *ctx, const char *name, bool dir, uint64_t size) {
    (void)ctx;
    ++listed;
    if (!strcmp(name, "中文🚀.txt"))
        saw_utf8 = true;
    if (!strcmp(name, "large.bin")) {
        saw_large = true;
        assert(!dir);
        assert(size == 100000);
    }
    if (!strcmp(name, "$Extend")) {
        saw_extend = true;
        assert(dir);
    }
    return true;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    assert(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    assert(n > 0);
    rewind(f);
    image_bytes = (size_t)n;
    image = malloc(image_bytes);
    assert(image);
    assert(fread(image, 1, image_bytes, f) == image_bytes);
    fclose(f);
    offset_sectors = 2048;
    check(mounted());
    bool dir;
    uint64_t size;
    check(ntfs_stat("/", &dir, &size));
    assert(dir);
    check(ntfs_stat("/resident.txt", &dir, &size));
    assert(!dir);
    const char *text = "Hello from native NTFS!\n你好，ArkOS！\n";
    assert(size == strlen(text));
    char small[1024] = {0};
    size_t got;
    check(ntfs_read("/resident.txt", 0, small, sizeof(small), &got));
    assert(got == strlen(text));
    assert(!memcmp(small, text, got));
    check(ntfs_read("/中文🚀.txt", 0, small, sizeof(small), &got));
    assert(got == strlen(text));
    check(ntfs_read("/$Extend/nested.txt", 0, small, sizeof(small), &got));
    assert(got == strlen(text));
    check(ntfs_read("/entry-095.txt", 0, small, sizeof(small), &got));
    assert(got == strlen(text));
    check(ntfs_list("/", emit, 0));
    assert(listed >= 100 && saw_utf8 && saw_large && saw_extend);
    unsigned char *big = malloc(100000);
    assert(big);
    check(ntfs_read("/large.bin", 0, big, 100000, &got));
    assert(got == 100000);
    for (unsigned i = 0; i < 100000; ++i)
        assert(big[i] == (unsigned char)((i * 29 + 7) % 251));
    check(ntfs_read("/large.bin", 4093, big, 6000, &got));
    assert(got == 6000);
    for (unsigned i = 0; i < got; ++i)
        assert(big[i] == (unsigned char)(((i + 4093) * 29 + 7) % 251));
    check(ntfs_read("/large.bin", 99997, big, 100, &got));
    assert(got == 3);
    check(ntfs_read("/large.bin", UINT64_MAX, big, 100, &got));
    assert(got == 0);
    check(ntfs_read("/resident.txt", 0, 0, 0, &got));
    assert(!got);
    assert(!ntfs_read("/", 0, small, sizeof(small), &got));
    assert(!ntfs_stat("/missing", &dir, &size));
    assert(!ntfs_stat("/RESIDENT.TXT", &dir, &size));
    assert(!ntfs_stat("/../resident.txt", &dir, &size));
    /* Malformed metadata must fail without block reads outside the volume. */
    unsigned char boot[512];
    memcpy(boot, image, 512);
    image[11] = 0;
    image[12] = 16;
    assert(!mounted());
    memcpy(image, boot, 512);
    image[13] = 3;
    assert(!mounted());
    memcpy(image, boot, 512);
    image[64] = 0x80;
    assert(!mounted());
    memcpy(image, boot, 512);
    memset(image + 40, 0xff, 8);
    assert(!mounted());
    memcpy(image, boot, 512);
    uint64_t mft = u64(image + 48) * image[13] * 512;
    unsigned char backup[1024];
    memcpy(backup, image + mft, sizeof(backup));
    image[mft + 510] ^= 1;
    assert(!mounted());
    memcpy(image + mft, backup, sizeof(backup));
    image[mft + 4] = 0xfe;
    image[mft + 5] = 0xff;
    assert(!mounted());
    memcpy(image + mft, backup, sizeof(backup));
    unsigned data = attr(image + mft, 0x80);
    assert(data);
    memset(image + mft + data + 4, 0xff, 4);
    assert(!mounted());
    memcpy(image + mft, backup, sizeof(backup));
    image[mft + data + 12] = 1;
    assert(!mounted());
    memcpy(image + mft, backup, sizeof(backup));
    unsigned run = u16(image + mft + data + 32);
    image[mft + data + run] = 0x10;
    assert(!mounted());
    memcpy(image + mft, backup, sizeof(backup));
    image[mft + data + run] = 0x91;
    assert(!mounted());
    memcpy(image + mft, backup, sizeof(backup));
    image[mft + data + 32] = 0xff;
    image[mft + data + 33] = 0xff;
    assert(!mounted());
    memcpy(image + mft, backup, sizeof(backup));
    memset(image + mft + data + 48, 0xff, 8);
    assert(!mounted());
    memcpy(image + mft, backup, sizeof(backup));
    /* Split the MFT mapping into two physical runs without changing content. */
    {
        unsigned char *a = image + mft + data;
        unsigned mapping = u16(a + 32);
        uint64_t clusters = u64(a + 40) / (image[13] * 512u), lcn = u64(image + 48);
        if (clusters > 2 && clusters < 256 && lcn < 65536 && mapping + 8 <= u32(a + 4)) {
            unsigned char runs[8] = {0x21,
                                     2,
                                     (unsigned char)lcn,
                                     (unsigned char)(lcn >> 8),
                                     0x11,
                                     (unsigned char)(clusters - 2),
                                     2,
                                     0};
            memcpy(a + mapping, runs, 8);
            check(mounted());
            check(ntfs_read("/entry-095.txt", 0, small, sizeof(small), &got));
            assert(got == strlen(text));
            memcpy(image + mft, backup, sizeof(backup));
            /* Relocate the first two MFT clusters upward; second run delta is negative. */
            if (lcn == 4 && image[13] == 8) {
                unsigned char saved[8192];
                memcpy(saved, image + 100 * 4096, sizeof(saved));
                memcpy(image + 100 * 4096, image + mft, sizeof(saved));
                unsigned char negative[8] = {
                    0x11, 2, 100, 0x11, (unsigned char)(clusters - 2), (unsigned char)(6 - 100),
                    0,    0};
                memcpy(a + mapping, negative, 8);
                check(mounted());
                check(ntfs_read("/entry-095.txt", 0, small, sizeof(small), &got));
                assert(got == strlen(text));
                memcpy(image + mft, backup, sizeof(backup));
                memcpy(image + 100 * 4096, saved, sizeof(saved));
            }
        }
    }
    check(mounted());
    /* Locate an actual root-directory INDX block; corrupt its fixups and bounds. */
    {
        uint64_t index = 0;
        for (uint64_t at = 0; at + 4096 <= image_bytes; at += 512) {
            if (memcmp(image + at, "INDX", 4))
                continue;
            uint32_t first = u32(image + at + 24);
            if (first >= 16 && first + 24 + 16 + 8 < 4096 &&
                (u64(image + at + 24 + first + 16) & UINT64_C(0xffffffffffff)) == 5) {
                index = at;
                break;
            }
        }
        assert(index);
        unsigned char save[4096];
        memcpy(save, image + index, sizeof(save));
        image[index + 510] ^= 1;
        assert(!ntfs_list("/", emit, 0));
        memcpy(image + index, save, sizeof(save));
        memset(image + index + 24, 0xff, 4);
        assert(!ntfs_list("/", emit, 0));
        memcpy(image + index, save, sizeof(save));
        unsigned entry = 24 + u32(image + index + 24);
        image[index + entry + 8] = 0;
        image[index + entry + 9] = 0;
        assert(!ntfs_list("/", emit, 0));
        memcpy(image + index, save, sizeof(save));
    }
    /* Locate the resident fixture's physical FILE record in this generated image. */
    uint64_t resident = 0;
    for (uint64_t at = 0; at + 1024 <= image_bytes; at += 1024) {
        if (memcmp(image + at, "FILE", 4))
            continue;
        for (unsigned p = u16(image + at + 20);
             p + 24 < 1024 && u32(image + at + p) != UINT32_MAX;) {
            unsigned len = u32(image + at + p + 4);
            if (len < 24 || len > 1024 - p)
                break;
            if (u32(image + at + p) == 0x30 && !image[at + p + 8]) {
                unsigned off = u16(image + at + p + 20);
                const unsigned char *key = image + at + p + off;
                if (off + 66 + 24 <= len && key[64] == 12) {
                    const char *want = "resident.txt";
                    bool match = true;
                    for (unsigned j = 0; j < 12; ++j)
                        if (u16(key + 66 + j * 2) != (unsigned char)want[j])
                            match = false;
                    if (match)
                        resident = at;
                }
            }
            p += len;
        }
    }
    assert(resident);
    memcpy(backup, image + resident, 1024);
    data = attr(image + resident, 0x80);
    assert(data);
    image[resident + data + 13] |= 0x40;
    assert(!ntfs_read("/resident.txt", 0, small, sizeof(small), &got));
    memcpy(image + resident, backup, 1024);
    image[resident + data + 13] |= 0x80;
    assert(!ntfs_read("/resident.txt", 0, small, sizeof(small), &got));
    memcpy(image + resident, backup, 1024);
    image[resident + data + 20] = 0xff;
    image[resident + data + 21] = 0xff;
    assert(!ntfs_read("/resident.txt", 0, small, sizeof(small), &got));
    memcpy(image + resident, backup, 1024);
    check(ntfs_read("/resident.txt", 0, small, sizeof(small), &got));
    printf("NTFS real-image tests passed: %u directory entries, %u bounded sector reads, UTF16 + "
           "resident/nonresident + index tree + malformed metadata\n",
           listed, reads);
    free(big);
    free(image);
    return 0;
}
