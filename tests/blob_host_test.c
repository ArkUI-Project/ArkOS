#define ARK_KERNEL
#include "ark_api.h"
#include "ark.h"
#include "accounts.h"
#include "process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static unsigned char disk[32 * 1024 * 1024], user[256 * 1024];
static uint32_t uid = 1000;
static uint64_t caps = ARK_CAP_FILES;
static bool tear;
static AccountState state = ACCOUNT_ACTIVE;
int64_t blob_request(ArkBlobRequest *);
void blob_test_reset(void);
bool storage_mounted(void) {
    return true;
}
uint32_t storage_volume_sectors(void) {
    return sizeof disk / 512;
}
bool storage_volume_flush(void) {
    return true;
}
bool storage_volume_io(uint32_t lba, uint32_t n, void *b, bool write) {
    assert(lba >= 8192 && lba + n <= sizeof disk / 512);
    if (write && tear && (lba == 8192 || lba == 8200)) {
        memcpy(disk + lba * 512, b, 100);
        tear = false;
        return false;
    }
    if (write)
        memcpy(disk + lba * 512, b, n * 512);
    else
        memcpy(b, disk + lba * 512, n * 512);
    return true;
}
bool process_has_cap(uint64_t mask) {
    return (caps & ARK_CAP_SYSTEM) || (caps & mask) == mask;
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
bool process_user_range(uint64_t addr, size_t n, bool write) {
    (void)write;
    return addr >= 0x40000000 && addr <= 0x40000000 + sizeof user &&
           n <= 0x40000000 + sizeof user - addr;
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
void strcopy(char *d, const char *s, size_t n) {
    if (n) {
        snprintf(d, n, "%s", s);
    }
}
static void wr32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (i * 8));
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
static void migration(void) {
    uint8_t *root = disk + 8192u * 512, *record = root + 32;
    memcpy(root, "ARKBLOB1", 8);
    root[8] = 1;
    wr32(record, 1000);
    wr32(record + 4, 8208);
    wr32(record + 8, 7);
    memcpy(disk + 8208u * 512, "olddata", 7);
    wr32(record + 12, crc(disk + 8208u * 512, 7));
    strcpy((char *)record + 24, "legacy.bin");
    wr32(root + 4092, crc(root, 4092));
    ArkBlobRequest q = {.op = ARK_BLOB_READ, .buffer = 0x40000000, .capacity = 7};
    strcpy(q.name, "legacy.bin");
    assert(blob_request(&q) == 0 && !memcmp(user, "olddata", 7));
    q.op = ARK_BLOB_WRITE;
    memcpy(user, "newdata", 7);
    tear = true;
    assert(blob_request(&q) == -5);
    blob_test_reset();
    q.op = ARK_BLOB_READ;
    assert(blob_request(&q) == 0 && !memcmp(user, "olddata", 7));
    q.op = ARK_BLOB_WRITE;
    memcpy(user, "newdata", 7);
    assert(blob_request(&q) == 0);
    blob_test_reset();
    q.op = ARK_BLOB_READ;
    assert(blob_request(&q) == 0 && !memcmp(user, "newdata", 7));
    assert(!memcmp(disk + 8200u * 512, "ARKBLOB2", 8));
    blob_test_reset();
    memset(disk, 0, sizeof disk);
}
int main(void) {
    migration();
    ArkBlobRequest q = {.op = ARK_BLOB_WRITE, .buffer = 0x40000000, .capacity = 70001};
    strcpy(q.name, "native.bin");
    memset(user, 0x51, q.capacity);
    assert(blob_request(&q) == 0);
    q.op = ARK_BLOB_READ;
    memset(user, 0, q.capacity);
    assert(blob_request(&q) == 0 && user[70000] == 0x51);
    q.op = ARK_BLOB_WRITE;
    memset(user, 0x72, q.capacity);
    tear = true;
    assert(blob_request(&q) == -5);
    blob_test_reset();
    q.op = ARK_BLOB_READ;
    assert(blob_request(&q) == 0 && user[0] == 0x51 && user[70000] == 0x51);
    uid = 1001;
    q.op = ARK_BLOB_LIST;
    q.index = 0;
    assert(blob_request(&q) == -2);
    uid = 1000;
    caps = 0;
    assert(blob_request(&q) == -1);
    caps = ARK_CAP_FILES;
    state = ACCOUNT_LOCKED;
    assert(blob_request(&q) == -1);
    state = ACCOUNT_ACTIVE;
    q.op = ARK_BLOB_WRITE;
    q.buffer = 0x1000;
    assert(blob_request(&q) == -14);
    q.buffer = 0x40000000;
    q.capacity = ARK_BLOB_MAX + 1;
    assert(blob_request(&q) == -14);
    strcpy(q.name, "../other");
    assert(blob_request(&q) == -22);
    strcpy(q.name, "native.bin");
    q.op = ARK_BLOB_REMOVE;
    assert(blob_request(&q) == 0);
    blob_test_reset();
    q.op = ARK_BLOB_READ;
    assert(blob_request(&q) == -2);
    for (unsigned i = 0; i < 70; i++) {
        q = (ArkBlobRequest){.op = ARK_BLOB_WRITE, .buffer = 0x40000000, .capacity = 3};
        snprintf(q.name, sizeof q.name, "item%02u", i);
        user[0] = (uint8_t)i;
        assert(blob_request(&q) == 0);
    }
    blob_test_reset();
    for (unsigned i = 0; i < 70; i++) {
        q = (ArkBlobRequest){.op = ARK_BLOB_READ, .buffer = 0x40000000, .capacity = 3};
        snprintf(q.name, sizeof q.name, "item%02u", i);
        assert(blob_request(&q) == 0 && user[0] == i);
    }
    q = (ArkBlobRequest){.op = ARK_BLOB_LIST, .index = 69};
    assert(blob_request(&q) == 0 && q.count == 70);
    q.index = 70;
    assert(blob_request(&q) == -2);
    puts("PASS native binary store: multi-chunk bytes, private UID, capability, lock, bad pointer, "
         "bounds, COW torn-index recovery persistent deletion, legacy migration and 70 objects "
         "across linked index pages");
}
