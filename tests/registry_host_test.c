/* Real Registry, SHA-256, native Blob COW storage and allocator. */
#define ARK_KERNEL
#include "ark_api.h"
#include "ark.h"
#include "accounts.h"
#include "process.h"
#include "blob.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t disk[32 * 1024 * 1024], user[4096];
static uint32_t uid = 1000, session_uid = 1000, pid = 50;
static uint64_t caps = ARK_CAP_UI;
static bool tear, admin = true;
static AccountState state = ACCOUNT_ACTIVE;
int64_t blob_request(ArkBlobRequest *), registry_request(ArkRegistryRequest *);
void blob_test_reset(void), registry_test_reset(void);
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
    return session_uid;
}
bool accounts_current_admin(void) {
    return admin;
}
AccountState accounts_state(void) {
    return state;
}
bool process_get_info(uint32_t p, ProcessInfo *out) {
    if (p != pid)
        return false;
    memset(out, 0, sizeof *out);
    strcpy(out->name, pid == 50 ? "calendar" : "reminders");
    return true;
}
bool process_user_range(uint64_t a, size_t n, bool write) {
    (void)write;
    return a >= 0x40000000 && a <= 0x40000000 + sizeof user && n <= 0x40000000 + sizeof user - a;
}
bool process_copy_from_user(void *d, uint64_t a, size_t n) {
    if (!process_user_range(a, n, false))
        return false;
    memcpy(d, user + a - 0x40000000, n);
    return true;
}
bool process_copy_to_user(uint64_t a, const void *s, size_t n) {
    if (!process_user_range(a, n, true))
        return false;
    memcpy(user + a - 0x40000000, s, n);
    return true;
}
void strcopy(char *d, const char *s, size_t n) {
    if (n)
        snprintf(d, n, "%s", s);
}
static ArkRegistryRequest request(unsigned op, const char *key) {
    ArkRegistryRequest q = {.op = op, .type = ARK_REG_STRING, .length = 3};
    strcpy(q.key, key);
    memcpy(q.value, "one", 3);
    return q;
}
static void reboot(void) {
    registry_test_reset();
    blob_test_reset();
}
int main(void) {
    ArkRegistryRequest q = request(ARK_REG_SET, "/apps/calendar/events/one");
    assert(registry_request(&q) == 0);
    uint64_t generation = q.generation;
    q.op = ARK_REG_GET;
    assert(registry_request(&q) == 0 && q.length == 3 && !memcmp(q.value, "one", 3));
    q.op = ARK_REG_SET;
    memcpy(q.value, "two", 3);
    tear = true;
    assert(registry_request(&q) == -5 && q.generation == generation);
    q.op = ARK_REG_GET;
    assert(registry_request(&q) == 0 && !memcmp(q.value, "one", 3));
    reboot();
    assert(registry_request(&q) == 0 && !memcmp(q.value, "one", 3));
    pid = 51;
    assert(registry_request(&q) == -1);
    caps = ARK_CAP_SYSTEM;
    assert(registry_request(&q) == 0);
    q = request(ARK_REG_SET, "/user/input/pinyin/learned/0");
    assert(registry_request(&q) == 0);
    caps = ARK_CAP_UI;
    assert(registry_request(&q) == -1);
    q.op = ARK_REG_GET;
    assert(registry_request(&q) == -1);
    caps = ARK_CAP_SYSTEM;
    q = request(ARK_REG_SET, "/user/appearance/dark");
    q.type = ARK_REG_INTEGER;
    q.length = 0;
    q.integer = 1;
    assert(registry_request(&q) == 0);
    caps = ARK_CAP_UI;
    assert(registry_request(&q) == -1);
    q.op = ARK_REG_GET;
    assert(registry_request(&q) == 0 && q.integer == 1);
    caps = ARK_CAP_SYSTEM;
    q = request(ARK_REG_SET, "/system/test");
    admin = false;
    assert(registry_request(&q) == -1);
    admin = true;
    assert(registry_request(&q) == 0);
    const char *bad[] = {"/apps//calendar",    "/apps/calendar/../x",
                         "/apps/calendar/./x", "/apps/calendar/",
                         "relative",           "/apps/calendar/\\x"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        q = request(ARK_REG_SET, bad[i]);
        assert(registry_request(&q) == -22);
    }
    q = request(ARK_REG_SET, "/user/bounds");
    q.length = 513;
    assert(registry_request(&q) == -22);
    q.length = 512;
    assert(registry_request(&q) == -22);
    q.type = ARK_REG_BINARY;
    assert(registry_request(&q) == 0);
    uid = session_uid = 1001;
    q = request(ARK_REG_GET, "/apps/calendar/events/one");
    assert(registry_request(&q) == -2);
    uid = session_uid = 1000;
    state = ACCOUNT_LOCKED;
    assert(registry_request(&q) == -1);
    state = ACCOUNT_ACTIVE;
    uid = 1001;
    caps = ARK_CAP_UI;
    assert(registry_request(&q) == -1);
    uid = 1000;
    caps = ARK_CAP_SYSTEM;
    q = request(ARK_REG_LIST, "/apps/calendar/events/");
    assert(registry_request(&q) == 0 && q.count == 1 &&
           !strcmp(q.key, "/apps/calendar/events/one"));
    q.op = ARK_REG_DELETE;
    assert(registry_request(&q) == 0);
    reboot();
    q.op = ARK_REG_GET;
    assert(registry_request(&q) == -2);
    ArkBlobRequest b = {.op = ARK_BLOB_LIST, .index = 0};
    caps = ARK_CAP_FILES;
    assert(blob_request(&b) == -2);
    strcpy(b.name, "@registry.fake");
    b.op = ARK_BLOB_READ;
    b.buffer = 0x40000000;
    b.capacity = 3;
    assert(blob_request(&b) == -1);
    puts(
        "PASS registry namespace ownership, shared read-only appearance, private IME, admin system "
        "keys, UID/lock, value bounds, COW failure/cache/reboot and hidden backing objects");
}
