#include "fat32.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static FILE *disk;
static uint64_t total;
static unsigned writes;
static unsigned fault;
static uint32_t fat_lba, root_lba, root_cluster;
static uint32_t r32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static bool read_blocks(void *ctx, uint64_t lba, uint32_t n, void *b) {
    (void)ctx;
    assert(lba < total && n <= total - lba);
    bool ok = !fseek(disk, (long)(lba * 512), SEEK_SET) && fread(b, 512, n, disk) == n;
    if (!ok)
        return false;
    for (unsigned i = 0; i < n; i++) {
        unsigned char *p = (unsigned char *)b + i * 512;
        if (lba + i == 0) {
            if (fault == 1) {
                p[11] = 0;
                p[12] = 4;
            }
            if (fault == 2) {
                p[36] = p[37] = p[38] = p[39] = 255;
            }
            if (fault == 3) {
                p[44] = p[45] = p[46] = p[47] = 255;
            }
        }
        if (fault == 4 && lba + i == fat_lba + root_cluster / 128) {
            unsigned off = (root_cluster % 128) * 4;
            for (unsigned j = 0; j < 4; j++)
                p[off + j] = (unsigned char)(root_cluster >> (j * 8));
        }
        if (fault == 4 && lba + i == root_lba) {
            memset(p, 0, 512);
            for (unsigned j = 0; j < 512; j += 32)
                p[j] = 0xe5;
        }
    }
    return true;
}
static bool write_blocks(void *ctx, uint64_t lba, uint32_t n, const void *b) {
    (void)ctx;
    assert(lba < total && n <= total - lba);
    writes++;
    return !fseek(disk, (long)(lba * 512), SEEK_SET) && fwrite(b, 512, n, disk) == n;
}
static bool flush_blocks(void *ctx) {
    (void)ctx;
    return !fflush(disk) && !fsync(fileno(disk));
}
static void expect(const char *p, const char *text) {
    char data[16384];
    size_t got;
    bool dir;
    uint64_t size;
    assert(fat32_stat(p, &dir, &size) && !dir && size == strlen(text));
    assert(fat32_read(p, 0, data, sizeof(data), &got));
    assert(got == size);
    data[got] = 0;
    assert(!strcmp(data, text));
}
static bool listed(void *ctx, const char *name, bool dir, uint64_t size) {
    (void)dir;
    (void)size;
    unsigned *n = ctx;
    assert(name[0]);
    (*n)++;
    return true;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    disk = fopen(argv[1], "r+b");
    assert(disk);
    fseek(disk, 0, SEEK_END);
    total = (uint64_t)ftell(disk) / 512;
    unsigned char boot[512];
    fseek(disk, 0, SEEK_SET);
    assert(fread(boot, 1, 512, disk) == 512);
    fat_lba = boot[14] | ((uint32_t)boot[15] << 8);
    root_cluster = r32(boot + 44);
    root_lba = fat_lba + boot[16] * r32(boot + 36) + (root_cluster - 2) * boot[13];
    assert(fat32_mount(read_blocks, write_blocks, flush_blocks, 0, 0, total));
    assert(fat32_writable());
    expect("/README.TXT", "Host-created FAT32 file\n");
    expect("/Documents/中文文件.txt", "中文内容，由宿主 mtools 创建。\n");
    unsigned count = 0;
    assert(fat32_list("/", listed, &count) && count >= 2);
    assert(fat32_create("/新目录", true));
    assert(fat32_create("/新目录/原生写入.txt", false));
    assert(fat32_write("/新目录/原生写入.txt", "你好，FAT32！\n", strlen("你好，FAT32！\n")));
    expect("/新目录/原生写入.txt", "你好，FAT32！\n");
    assert(!fat32_remove("/新目录"));
    assert(fat32_write("/README.TXT", "Updated by ArkOS native FAT32\n",
                       strlen("Updated by ArkOS native FAT32\n")));
    expect("/readme.txt", "Updated by ArkOS native FAT32\n");
    static char full[16384];
    for (unsigned i = 0; i < 16383; i++)
        full[i] = (char)('a' + i % 26);
    full[16383] = 0;
    assert(fat32_create("/最大文件.txt", false));
    assert(fat32_write("/最大文件.txt", full, 16383));
    expect("/最大文件.txt", full);
    char offset[37];
    size_t got;
    assert(fat32_read("/最大文件.txt", 510, offset, sizeof(offset), &got) && got == sizeof(offset));
    assert(!strncmp(offset, full + 510, sizeof(offset)));
    for (unsigned i = 0; i < 24; i++) {
        char p[128];
        snprintf(p, sizeof(p), "/新目录/long-file-number-%02u.txt", i);
        assert(fat32_create(p, false));
        assert(fat32_write(p, "entry\n", 6));
    }
    count = 0;
    assert(fat32_list("/新目录", listed, &count) && count == 25);
    assert(fat32_rename("/新目录/原生写入.txt", "/新目录/重命名.txt"));
    expect("/新目录/重命名.txt", "你好，FAT32！\n");
    bool dir;
    uint64_t size;
    assert(!fat32_stat("/新目录/原生写入.txt", &dir, &size));
    assert(fat32_create("/empty-dir", true));
    assert(fat32_create("/empty-dir/delete-me.txt", false));
    assert(fat32_write("/empty-dir/delete-me.txt", "temporary", 9));
    assert(fat32_remove("/empty-dir/delete-me.txt"));
    assert(fat32_remove("/empty-dir"));
    assert(!fat32_create("/invalid?.txt", false));
    assert(!fat32_create("/../escape.txt", false));
    assert(fat32_sync());
    assert(writes);
    unsigned prior = writes;
    assert(!fat32_mount(read_blocks, write_blocks, flush_blocks, 0, 1, total - 1));
    assert(!fat32_write("/README.TXT", "bad", 3) && writes == prior);
    assert(fat32_mount(read_blocks, write_blocks, flush_blocks, 0, 0, total));
    expect("/README.TXT", "Updated by ArkOS native FAT32\n");
    expect("/新目录/重命名.txt", "你好，FAT32！\n");
    prior = writes;
    for (fault = 1; fault <= 3; fault++) {
        assert(!fat32_mount(read_blocks, write_blocks, flush_blocks, 0, 0, total));
        assert(writes == prior);
    }
    fault = 4;
    assert(fat32_mount(read_blocks, write_blocks, flush_blocks, 0, 0, total));
    count = 0;
    assert(!fat32_list("/", listed, &count));
    assert(writes == prior);
    fault = 0;
    assert(fat32_mount(read_blocks, write_blocks, flush_blocks, 0, 0, total));
    fclose(disk);
    puts("PASS FAT32: independent mkfs/mtools input, LFN Chinese, mkdir, cross-sector directories, "
         "COW writes, maximum file, offset reads, rename, delete, malformed BPB no-write, cyclic "
         "directory bounded, remount");
}
