/* Exercises the real ArkFS parser/serializer with an injectable sector device.
 * Run via tests/test-storage.sh; production ATA transport is tested in QEMU. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "ark.h"
#include "storage.h"
#include "arkfs2.h"
#include <string.h>

#define DISK_BYTES (32u * 1024u * 1024u)
#define BANK0 (8u * 512u)
#define BANK1 (2088u * 512u)
static unsigned char disk[DISK_BYTES];
static unsigned char before[DISK_BYTES];
static bool fail_payload, fail_header, fail_flush;
static unsigned writes;

bool ark_test_identify(uint32_t *sectors) {
    *sectors = DISK_BYTES / 512u;
    return true;
}
bool ark_test_transfer(uint32_t lba, uint32_t count, uint8_t *buffer, bool write) {
    assert(count && lba < DISK_BYTES / 512u && count <= DISK_BYTES / 512u - lba);
    size_t offset = (size_t)lba * 512u, length = (size_t)count * 512u;
    if (write) {
        ++writes;
        if (fail_payload && count > 1) {
            memcpy(disk + offset, buffer, 512); /* sudden power loss after one sector */
            fail_payload = false;
            return false;
        }
        if (fail_header && (offset == BANK0 || offset == BANK1)) {
            /* A sector with some newly written and some stale header bytes. */
            memcpy(disk + offset, buffer, 32);
            fail_header = false;
            return false;
        }
        memcpy(disk + offset, buffer, length);
    } else
        memcpy(buffer, disk + offset, length);
    return true;
}
bool ark_test_flush(void) {
    if (fail_flush) {
        fail_flush = false;
        return false;
    }
    return true;
}
void serial_write(const char *text) {
    (void)text;
}
uint64_t platform_ticks(void) {
    return 0;
}

static void clear_ram(void) {
    memset(vfs_files, 0, sizeof(vfs_files));
}
static int file(const char *path, const char *content) {
    int n = vfs_create(path);
    assert(n >= 0 && vfs_write(n, content));
    return n;
}
static void expect(const char *path, const char *content) {
    int n = vfs_find(path);
    assert(n >= 0 && !vfs_files[n].is_dir && !strcmp(vfs_files[n].data, content));
}
static void reboot(void) {
    clear_ram();
    assert(storage_init());
}

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *image = fopen(argv[1], "rb");
    assert(image && fread(disk, 1, sizeof(disk), image) == sizeof(disk));
    fclose(image);
    arkfs2_fail_superblock(1000000);
    vfs_init();
    assert(storage_mounted() && writes > 0);
    assert(storage_capacity_bytes() == 64u * 16383u);
    assert(vfs_find("/home/ark/Documents/使用说明.txt") >= 0);
    assert(vfs_mkdir("test"));
    assert(vfs_mkdir("test/child"));
    file("test/child/中文.txt", "中文内容 survives reboot\n");
    assert(vfs_find("./test//child/../child/中文.txt") >= 0);
    assert(vfs_find("/../../home/ark/test/child/中文.txt") >= 0);
    assert(vfs_create("missing/child") < 0);
    assert(vfs_create("bad\xc0\xaf") < 0);
    assert(vfs_create("test/child") < 0);
    assert(!vfs_write(vfs_find("test"), "directory text"));
    assert(!vfs_remove("test") && !vfs_remove("/"));
    assert(!vfs_rename("test", "test/child/descendant"));
    assert(vfs_rename("test", "moved"));
    expect("moved/child/中文.txt", "中文内容 survives reboot\n");
    assert(vfs_copy("moved/child/中文.txt", "copy.txt"));
    assert(!vfs_copy("moved", "directory-copy"));
    assert(storage_sync());
    unsigned clean_writes = writes;
    assert(storage_sync() && writes == clean_writes);
    reboot();
    expect("moved/child/中文.txt", "中文内容 survives reboot\n");
    expect("copy.txt", "中文内容 survives reboot\n");
    puts("PASS: UTF-8 paths, namespace operations, unchanged sync, persistence");

    int n = vfs_find("copy.txt");
    assert(vfs_write(n, "last durable checkpoint"));
    assert(storage_sync());
    memcpy(before, disk, sizeof(disk));
    assert(vfs_write(n, "uncommitted text after torn payload"));
    fail_payload = true;
    assert(!storage_sync());
    reboot();
    expect("copy.txt", "last durable checkpoint");
    puts("PASS: interrupted payload write restores previous committed snapshot");

    memcpy(disk, before, sizeof(disk));
    reboot();
    n = vfs_find("copy.txt");
    assert(vfs_write(n, "uncommitted text after torn header"));
    fail_header = true;
    assert(!storage_sync());
    reboot();
    expect("copy.txt", "last durable checkpoint");
    puts("PASS: torn commit header restores previous committed snapshot");

    memcpy(disk, before, sizeof(disk));
    reboot();
    n = vfs_find("copy.txt");
    assert(vfs_write(n, "uncommitted text after failed payload flush"));
    fail_flush = true;
    assert(!storage_sync());
    reboot();
    expect("copy.txt", "last durable checkpoint");
    puts("PASS: failed payload flush leaves previous snapshot active");

    /* Destroy the newest valid bank and recover the older valid snapshot. */
    memcpy(disk, before, sizeof(disk));
    reboot();
    n = vfs_find("copy.txt");
    assert(vfs_write(n, "newest committed value"));
    assert(storage_sync());
    uint64_t ga = 0, gb = 0;
    for (unsigned i = 0; i < 8; ++i) {
        ga |= (uint64_t)disk[BANK0 + 8 + i] << (i * 8);
        gb |= (uint64_t)disk[BANK1 + 8 + i] << (i * 8);
    }
    unsigned newest = ga > gb ? BANK0 : BANK1;
    disk[newest + 512 + 30] ^= 0x20;
    reboot();
    expect("copy.txt", "last durable checkpoint");
    puts("PASS: corrupt newest payload falls back to older valid bank");

    /* Neither malformed media nor corruption authorizes a write/format. */
    disk[BANK0] = 0;
    disk[BANK1] = 0;
    unsigned prior_writes = writes;
    clear_ram();
    assert(!storage_init() && !storage_mounted() && writes == prior_writes);
    assert(!storage_sync() && writes == prior_writes);
    memcpy(disk, before, sizeof(disk));
    disk[0] = 0;
    clear_ram();
    assert(!storage_init() && writes == prior_writes);
    puts("PASS: damaged and unknown volumes never autoformat");

    /* Maximum file size, namespace exhaustion and safe recovery of deletions. */
    memcpy(disk, before, sizeof(disk));
    reboot();
    static char huge[VFS_FILE_CAP + 1];
    memset(huge, 'a', VFS_FILE_CAP - 1);
    huge[VFS_FILE_CAP - 1] = 0;
    n = file("full.txt", huge);
    huge[VFS_FILE_CAP - 1] = 'x';
    huge[VFS_FILE_CAP] = 0;
    assert(!vfs_write(n, huge));
    huge[VFS_FILE_CAP - 1] = 0;
    assert(storage_sync());
    reboot();
    expect("full.txt", huge);
    assert(vfs_remove("moved/child/中文.txt"));
    assert(vfs_remove("moved/child"));
    assert(vfs_remove("moved"));
    assert(storage_sync());
    reboot();
    assert(vfs_find("moved") < 0);
    for (unsigned i = 0; i < 64; ++i) {
        char name[40];
        snprintf(name, sizeof(name), "capacity-%u.txt", i);
        int idx = vfs_create(name);
        if (idx < 0)
            break;
        assert(vfs_write(idx, huge));
    }
    assert(vfs_create("no-slot.txt") < 0);
    assert(storage_sync());
    reboot();
    assert(vfs_find("full.txt") >= 0 && vfs_create("still-full.txt") < 0);
    puts("PASS: maximum file, capacity, deletion and full snapshot round-trip");

    memcpy(disk, before, sizeof(disk));
    arkfs2_fail_superblock(1);
    reboot();
    assert(storage_mounted() && !storage_is_v2());
    assert(!memcmp(disk, before, 4168u * 512u));
    expect("copy.txt", "last durable checkpoint");
    assert(strcmp(storage_status(), "未挂载 · 内存会话") != 0);
    puts("PASS: failed migration leaves the v1 volume mounted");

    memcpy(disk, before, sizeof(disk));
    arkfs2_fail_superblock(0);
    reboot();
    assert(storage_is_v2());
    assert(!memcmp(disk, "ARKFS2", 6));
    assert(!memcmp(disk + 512, before + 512, 4167u * 512u));
    expect("copy.txt", "last durable checkpoint");
    uint64_t used = storage_used_bytes(), freeb = storage_free_bytes();
    assert(used + freeb != (uint64_t)DISK_BYTES);
    puts("PASS: migration publishes ArkFS2 and keeps both v1 banks");

    memset(disk, 0, sizeof(disk));
    assert(storage_format_new() && storage_is_v2());
    assert(!memcmp(disk, "ARKFS2", 6));
    static unsigned char big[20000], got[20000];
    memset(big, 0x5a, sizeof(big));
    big[100] = 0;
    assert(vfs_store("/wide.bin", big, sizeof(big)));
    uint64_t nread = 0;
    assert(vfs_fetch("/wide.bin", got, sizeof(got), &nread));
    assert(nread == sizeof(big) && !memcmp(got, big, sizeof(big)));
    assert(vfs_rename("/wide.bin", "/wide2.bin"));
    assert(vfs_fetch("/wide2.bin", got, sizeof(got), &nread) && nread == sizeof(big));
    reboot();
    assert(storage_is_v2());
    assert(vfs_fetch("/wide2.bin", got, sizeof(got), &nread));
    assert(nread == sizeof(big) && !memcmp(got, big, sizeof(big)));
    assert(vfs_remove("/wide2.bin"));
    reboot();
    assert(!vfs_fetch("/wide2.bin", got, sizeof(got), &nread));
    used = storage_used_bytes();
    freeb = storage_free_bytes();
    assert(used + freeb != (uint64_t)DISK_BYTES);
    assert(vfs_store("/ro.txt", "abc", 3));
    assert(arkfs2_set_readonly("/ro.txt", true));
    int ro = vfs_find("/ro.txt");
    assert(ro >= 0 && !vfs_write(ro, "nope"));
    assert(!strcmp(storage_error(), "当前文件只读"));
    puts("PASS: new ArkFS2 mounts, and files over 16KB round-trip");
    return 0;
}
