/* Real package/ELF/SHA/COW modules; only storage transport and task launch are modeled. */
#define ARK_KERNEL
#include "package.h"
#include "blob.h"
#include "elf.h"
#include "sha256.h"
#include "accounts.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t disk[32 * 1024 * 1024], user[128 * 1024];
static uint32_t uid = 1000, pid = 1;
static uint64_t caps = ARK_CAP_SYSTEM;
static AccountState state = ACCOUNT_ACTIVE;
static bool tear, alive[32];
static VFile external_entry;
static unsigned spawned;
VFile vfs_files[VFS_MAX_FILES];
int64_t blob_request(ArkBlobRequest *);
void blob_test_reset(void);
void package_test_reset(void);
bool storage_mounted(void) {
    return true;
}
uint32_t storage_volume_sectors(void) {
    return sizeof disk / 512;
}
bool storage_volume_flush(void) {
    return true;
}
bool storage_volume_io(uint32_t lba, uint32_t n, void *p, bool write) {
    assert(lba >= 8192 && lba + n <= sizeof disk / 512);
    if (write && tear && (lba == 8192 || lba == 8200)) {
        memcpy(disk + lba * 512, p, 100);
        tear = false;
        return false;
    }
    if (write)
        memcpy(disk + lba * 512, p, n * 512);
    else
        memcpy(p, disk + lba * 512, n * 512);
    return true;
}
bool process_has_cap(uint64_t c) {
    return (caps & ARK_CAP_SYSTEM) || (caps & c) == c;
}
uint32_t process_current_pid(void) {
    return pid;
}
uint32_t process_current_uid(void) {
    return uid;
}
uint32_t accounts_current_uid(void) {
    return uid;
}
AccountState accounts_state(void) {
    return state;
}
bool process_user_range(uint64_t address, size_t n, bool write) {
    (void)write;
    return address >= 0x40000000 && address <= 0x40000000 + sizeof user &&
           n <= 0x40000000 + sizeof user - address;
}
bool process_copy_from_user(void *d, uint64_t s, size_t n) {
    if (!process_user_range(s, n, false))
        return false;
    memcpy(d, user + (s - 0x40000000), n);
    return true;
}
bool process_copy_to_user(uint64_t d, const void *s, size_t n) {
    if (!process_user_range(d, n, true))
        return false;
    memcpy(user + (d - 0x40000000), s, n);
    return true;
}
int process_spawn_elf(const void *p, size_t n, const char *name, uint32_t owner, uint64_t grants) {
    assert(process_validate_elf(p, n, grants) && owner == 1000 && !strcmp(name, "pkg.example") &&
           !(grants & (ARK_CAP_SYSTEM | ARK_CAP_PROCESS)));
    assert(spawned < 32);
    alive[spawned] = true;
    return 42 + spawned++;
}
bool process_get_info(uint32_t p, ProcessInfo *out) {
    if (p < 42 || p >= 42 + spawned || !alive[p - 42])
        return false;
    memset(out, 0, sizeof *out);
    out->pid = p;
    out->uid = uid;
    out->state = PROCESS_RUNNING;
    strcpy(out->name, "pkg.example");
    return true;
}
void process_update_caps(const char *name, uint32_t owner, uint64_t grants) {
    assert(!strcmp(name, "pkg.example") && owner == 1000);
    if (!(grants & ARK_CAP_UI)) {
        for (unsigned i = 0; i < spawned; i++)
            if (alive[i]) {
                alive[i] = false;
                package_process_exit(42 + i);
            }
    }
}
void services_permissions_changed(const char *name, uint32_t owner, uint64_t grants) {
    (void)name;
    (void)owner;
    (void)grants;
}
bool accounts_path_allowed(uint32_t owner, const char *p, bool write) {
    (void)write;
    return owner == uid && (!strncmp(p, "/home/test/", 11) || !strncmp(p, "/mnt/fat32/", 11));
}
bool vfs_path_canonical(char out[128], const char *p) {
    if (p[0] != '/' || strlen(p) >= 128)
        return false;
    strcpy(out, p);
    return true;
}
int vfs_find(const char *p) {
    return !strcmp(p, "/mnt/fat32/example.arkpkg") ? VFS_MAX_FILES + 1 : -1;
}
VFile *vfs_entry(int slot) {
    return slot == VFS_MAX_FILES + 1 ? &external_entry : NULL;
}
bool extfs_path(const char *p) {
    return !strncmp(p, "/mnt/fat32/", 11);
}
bool extfs_read_bytes(int slot, uint64_t offset, void *p, size_t n, size_t *count) {
    assert(slot == 1 && offset + n <= external_entry.size);
    memcpy(p, user + offset, n);
    *count = n;
    return true;
}
void strcopy(char *d, const char *s, size_t n) {
    if (n)
        snprintf(d, n, "%s", s);
}
void serial_write(const char *s) {
    fputs(s, stdout);
}
static void wr(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (8 * i));
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
static void package(unsigned version, unsigned maximum) {
    memset(user, 0, 12288);
    uint8_t *elf = user + 256;
    ELFHeader h = {.ident = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0},
                   .type = 2,
                   .machine = 62,
                   .version = 1,
                   .entry = PROCESS_USER_BASE,
                   .phoff = 64,
                   .ehsize = 64,
                   .phentsize = 56,
                   .phnum = 2};
    memcpy(elf, &h, sizeof h);
    ELFProgram text = {.type = 1,
                       .flags = 5,
                       .offset = 4096,
                       .vaddr = PROCESS_USER_BASE,
                       .filesz = 16,
                       .memsz = 4096,
                       .align = 4096};
    ELFProgram data = {.type = 1,
                       .flags = 6,
                       .offset = 8192,
                       .vaddr = PROCESS_USER_BASE + 8192,
                       .filesz = 16,
                       .memsz = 4096,
                       .align = 4096};
    memcpy(elf + 64, &text, sizeof text);
    memcpy(elf + 120, &data, sizeof data);
    memset(elf + 4096, 0x90, 16);
    elf[8192] = (uint8_t)version;
    memcpy(user, "ARKPKG1\0", 8);
    user[8] = 1;
    user[10] = 62;
    wr(user + 12, 256);
    wr(user + 16, 256 + 8208);
    wr(user + 20, 1);
    wr(user + 24, maximum);
    wr(user + 28, version);
    wr(user + 40, 8208);
    strcpy((char *)user + 48, "example");
    strcpy((char *)user + 80, "Example package");
    strcpy((char *)user + 144, "Test isolated admission");
    ark_sha256(elf, 8208, user + 208);
    wr(user + 252, crc(user, 252));
}
static void source_write(void) {
    ArkBlobRequest q = {.op = ARK_BLOB_WRITE, .buffer = 0x40000000, .capacity = 256 + 8208};
    strcpy(q.name, "example.arkpkg");
    assert(blob_request(&q) == 0);
}
static ArkPackageRequest request(unsigned op) {
    ArkPackageRequest q = {.op = op};
    strcpy(q.path, "blob:example.arkpkg");
    strcpy(q.id, "example");
    return q;
}
int main(void) {
    uint8_t hash[32];
    ark_sha256("abc", 3, hash);
    static const uint8_t abc[] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                  0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                  0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    assert(!memcmp(hash, abc, 32));
    package(1, ARK_CAP_UI | ARK_CAP_FILES);
    external_entry.used = true;
    external_entry.size = 256 + 8208;
    ArkPackageRequest ext = {.op = ARK_PACKAGE_INSPECT};
    strcpy(ext.path, "/mnt/fat32/example.arkpkg");
    assert(package_request(&ext) == 0);
    source_write();
    ArkPackageRequest q = request(ARK_PACKAGE_INSPECT);
    assert(package_request(&q) == 0 && !q.info.installed && q.info.maximum == 6);
    uint8_t expected[32];
    memcpy(expected, q.info.manifest_sha256, 32);
    q = request(ARK_PACKAGE_INSTALL);
    memcpy(q.expected_manifest_sha256, expected, 32);
    assert(package_request(&q) == 0 && q.info.slot == 0 && q.info.grants == 4);
    assert(package_request(&q) == -17);
    q = request(ARK_PACKAGE_GRANTS);
    q.grants = 6;
    assert(package_request(&q) == 0);
    ArkSpawn spawn = {0};
    strcpy(spawn.program, "pkg.example");
    assert(package_spawn(&spawn) == 0 && spawn.pid == 42 && spawned == 1);
    assert(package_spawn(&spawn) == 0 && spawned == 1);
    spawn.flags = ARK_SPAWN_NEW;
    strcpy(spawn.argument, "second");
    assert(package_spawn(&spawn) == 0 && spawn.pid == 43 && spawned == 2);
    ArkLaunchInfo launch = {0};
    assert(package_launch_info(43, &launch) == 0 && !strcmp(launch.argument, "second"));
    q = request(ARK_PACKAGE_REMOVE);
    assert(package_request(&q) == -16);
    alive[0] = false;
    package_process_exit(42);
    q = request(ARK_PACKAGE_REMOVE);
    assert(package_request(&q) == -16);
    alive[1] = false;
    package_process_exit(43);
    q = request(ARK_PACKAGE_GRANTS);
    q.grants = ARK_CAP_SYSTEM | ARK_CAP_UI;
    assert(package_request(&q) == -1);
    package(2, 46);
    source_write();
    q = request(ARK_PACKAGE_UPGRADE);
    memcpy(q.expected_manifest_sha256, expected, 32);
    assert(package_request(&q) == -11);
    q = request(ARK_PACKAGE_UPGRADE);
    tear = true;
    assert(package_request(&q) == -5);
    blob_test_reset();
    package_test_reset();
    q = (ArkPackageRequest){.op = ARK_PACKAGE_LIST, .index = 0};
    assert(package_request(&q) == 0 && q.info.major == 1 && q.info.grants == 6);
    q = request(ARK_PACKAGE_UPGRADE);
    assert(package_request(&q) == 0 && q.info.major == 2 && q.info.maximum == 46 &&
           q.info.grants == 6);
    package(3, ARK_CAP_UI);
    source_write();
    q = request(ARK_PACKAGE_UPGRADE);
    assert(package_request(&q) == 0 && q.info.grants == 4);
    q = request(ARK_PACKAGE_UPGRADE);
    assert(package_request(&q) == -22);
    package(4, 6);
    user[256 + 4096] ^= 1;
    source_write();
    q = request(ARK_PACKAGE_UPGRADE);
    assert(package_request(&q) == -22);
    package(4, ARK_CAP_SYSTEM | 4);
    source_write();
    q = request(ARK_PACKAGE_INSPECT);
    assert(package_request(&q) == -22);
    package(4, 6);
    wr(user + 256 + 64 + 4, 7);
    ark_sha256(user + 256, 8208, user + 208);
    wr(user + 252, crc(user, 252));
    source_write();
    q = request(ARK_PACKAGE_INSPECT);
    assert(package_request(&q) == -22);
    ArkBlobRequest b = {.op = ARK_BLOB_READ, .buffer = 0x40000000, .capacity = 256};
    strcpy(b.name, "@pkg.example");
    assert(blob_request(&b) == -1);
    b.op = ARK_BLOB_REMOVE;
    assert(blob_request(&b) == -1);
    uid = 1001;
    package_test_reset();
    q = (ArkPackageRequest){.op = ARK_PACKAGE_LIST, .index = 0};
    assert(package_request(&q) == -2);
    uid = 1000;
    package_test_reset();
    q = (ArkPackageRequest){.op = ARK_PACKAGE_LIST, .index = 0};
    assert(package_request(&q) == 0 && q.info.major == 3 && q.info.grants == 4);
    caps = ARK_CAP_UI;
    q = request(ARK_PACKAGE_REMOVE);
    assert(package_request(&q) == -1);
    caps = ARK_CAP_SYSTEM;
    state = ACCOUNT_LOCKED;
    assert(package_request(&q) == -1);
    state = ACCOUNT_ACTIVE;
    assert(package_request(&q) == 0);
    blob_test_reset();
    package_test_reset();
    q = (ArkPackageRequest){.op = ARK_PACKAGE_LIST, .index = 0};
    assert(package_request(&q) == -2);
    /* Installation count grows beyond the former eight slots; stable identities
     * and grants survive a complete cache/store reload. */
    for (unsigned i = 0; i < 24; i++) {
        package(1, ARK_CAP_UI);
        snprintf((char *)user + 48, 32, "many%02u", i);
        wr(user + 252, crc(user, 252));
        source_write();
        q = request(ARK_PACKAGE_INSTALL);
        assert(package_request(&q) == 0 && q.info.slot == i);
    }
    blob_test_reset();
    package_test_reset();
    unsigned listed = 0;
    while (1) {
        q = (ArkPackageRequest){.op = ARK_PACKAGE_LIST, .index = listed};
        int64_t r = package_request(&q);
        if (r == -2)
            break;
        assert(r == 0 && q.info.installed == 1 && q.info.grants == ARK_CAP_UI);
        listed++;
    }
    assert(listed == 24);
    q = (ArkPackageRequest){.op = ARK_PACKAGE_FIND};
    strcpy(q.id, "many19");
    assert(package_request(&q) == 0 && q.info.slot == 19);
    q.op = ARK_PACKAGE_REMOVE;
    assert(package_request(&q) == 0);
    blob_test_reset();
    package_test_reset();
    q.op = ARK_PACKAGE_FIND;
    assert(package_request(&q) == -2);
    puts("PASS ArkPkg: SHA vector, manifest, W^X ELF, inspection pinning, UI-only defaults, "
         "grants, upgrade, downgrade rejection, COW failure recovery, private UID/store, lock "
         "persistent removal and 24 installed packages");
}
