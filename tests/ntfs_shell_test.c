/* Real NTFS -> external facade -> VFS -> shell integration; hardware only mocked. */
#include "ark.h"
#include "storage.h"
#include "extfs.h"
#include "block.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *image;
static size_t image_size;
static unsigned writes;
static char serial[65536];
static BlockDevice device = {1, true, 0};
void serial_write(const char *s) {
    size_t n = strlen(serial), m = strlen(s);
    if (n + m < sizeof(serial))
        memcpy(serial + n, s, m + 1);
}
void strcopy(char *d, const char *s, size_t cap) {
    if (!cap)
        return;
    size_t n = strlen(s);
    if (n >= cap)
        n = cap - 1;
    memmove(d, s, n);
    d[n] = 0;
}
void uint_to_str(uint64_t n, char *out) {
    snprintf(out, 24, "%llu", (unsigned long long)n);
}
void platform_time(int *h, int *m, int *s) {
    *h = *m = *s = 0;
}
uint64_t platform_ticks(void) {
    return 0;
}
void platform_reboot(void) {
}
void platform_poweroff(void) {
}
bool storage_init(void) {
    return false;
}
bool storage_mounted(void) {
    return false;
}
const char *storage_status(void) {
    return "Home RAM mode";
}
const char *storage_error(void) {
    return "";
}
uint64_t storage_capacity_bytes(void) {
    return 0;
}
uint64_t storage_used_bytes(void) {
    return 0;
}
bool storage_sync(void) {
    return true;
}
void storage_mark_dirty(void) {
}
void block_init(void) {
}
BlockDevice *block_device(unsigned id) {
    return id == 1 ? &device : 0;
}
bool block_read(BlockDevice *d, uint64_t lba, uint32_t sectors, void *buffer) {
    assert(d == &device);
    assert(lba <= device.sectors && sectors <= device.sectors - lba);
    memcpy(buffer, image + lba * 512, (size_t)sectors * 512);
    return true;
}
bool block_write(BlockDevice *d, uint64_t lba, uint32_t sectors, const void *buffer) {
    (void)d;
    (void)lba;
    (void)sectors;
    (void)buffer;
    ++writes;
    return false;
}
bool block_flush(BlockDevice *d) {
    (void)d;
    return true;
}
const char *block_error(void) {
    return "test block error";
}
static void run(const char *s) {
    serial[0] = 0;
    shell_execute(s);
}
static void expect(const char *s) {
    if (!strstr(serial, s)) {
        fprintf(stderr, "Wanted [%s] got [%s]\n", s, serial);
        assert(0);
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    image_size = (size_t)ftell(f);
    rewind(f);
    image = malloc(image_size);
    assert(image);
    assert(fread(image, 1, image_size, f) == image_size);
    fclose(f);
    device.sectors = image_size / 512;
    vfs_init();
    BootInfo boot = {0};
    shell_init(&boot);
    run("mounts");
    expect("NTFS read-only");
    run("ls -l /mnt/ntfs");
    expect("README.txt");
    expect("中文说明.txt");
    expect("large.txt");
    run("cat /mnt/ntfs/README.txt");
    expect("real NTFS volume");
    run("cat /mnt/ntfs/中文说明.txt");
    expect("原生 NTFS 只读驱动");
    run("cp /mnt/ntfs/README.txt /home/ark/copied.txt");
    int i = vfs_find("copied.txt");
    assert(i >= 0);
    assert(strstr(vfs_entry(i)->data, "real NTFS volume"));
    run("cat /mnt/ntfs/README.txt | grep ArkOS | wc -l > count.txt");
    i = vfs_find("count.txt");
    assert(i >= 0);
    assert(!strcmp(vfs_entry(i)->data, "1\n"));
    run("cat /mnt/ntfs/large.txt");
    expect("16383-byte");
    run("echo overwrite > /mnt/ntfs/README.txt");
    expect("read-only");
    run("echo overwrite > /mnt/ntfs/NEW.TXT");
    expect("read-only");
    run("rm /mnt/ntfs/README.txt");
    expect("read-only");
    run("mkdir /mnt/ntfs/newdir");
    expect("read-only");
    run("cat /mnt/ntfs/README.txt");
    expect("real NTFS volume");
    run("cd /mnt/ntfs");
    assert(!strcmp(shell_cwd(), "/mnt/ntfs"));
    run("cat README.txt");
    expect("real NTFS volume");
    run("sync");
    expect("synchronized");
    expect("RAM-only");
    assert(!writes);
    puts("NTFS full shell/facade integration passed; no disk write callbacks invoked");
    free(image);
    return 0;
}
