/* Original ArkOS hierarchical configuration store. Each value is one atomic
 * ArkFS COW object; the full key is checked as well as its 160-bit object name.
 * Namespace ownership comes exclusively from the running process/session. */
#define ARK_KERNEL
#include "ark_api.h"
#include "ark.h"
#include "accounts.h"
#include "process.h"
#include "blob.h"
#include "sha256.h"
#include "alloc.h"
typedef struct {
    char magic[8], key[128];
    uint32_t type, length;
    int64_t integer;
    uint8_t value[512];
} Value;
typedef struct Cached {
    struct Cached *next;
    uint32_t uid;
    char name[64];
    Value value;
} Cached;
static Cached *cache;
static Cached *cache_find(const char *name, uint32_t uid) {
    for (Cached *c = cache; c; c = c->next)
        if (c->uid == uid && !strcmp(c->name, name))
            return c;
    return 0;
}
static void cache_put(const char *name, uint32_t uid, const Value *v) {
    Cached *c = cache_find(name, uid);
    if (!c) {
        c = ark_alloc(sizeof *c);
        if (!c)
            return;
        c->next = cache;
        cache = c;
        c->uid = uid;
        strcopy(c->name, name, 64);
    }
    c->value = *v;
}
static void cache_remove(const char *name, uint32_t uid) {
    Cached **at = &cache;
    while (*at) {
        Cached *c = *at;
        if (c->uid == uid && !strcmp(c->name, name)) {
            *at = c->next;
            ark_free(c);
            return;
        }
        at = &c->next;
    }
}
static bool key_ok(const char *s, bool prefix) {
    if (s[0] != '/')
        return false;
    unsigned segment = 0;
    for (unsigned i = 1; i < 128; i++) {
        unsigned c = (uint8_t)s[i];
        if (!c)
            return i > 1 && (segment || prefix);
        if (c == '/') {
            if (!segment)
                return false;
            segment = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.'))
                return false;
            if (c == '.' && (!segment && (s[i + 1] == '.' || !s[i + 1] || s[i + 1] == '/')))
                return false;
            segment++;
        }
    }
    return false;
}
static bool permitted(const char *key, bool write) {
    bool system = process_has_cap(ARK_CAP_SYSTEM);
    if (!strncmp(key, "/system/", 8))
        return system && (!write || accounts_current_admin());
    if (system)
        return !strncmp(key, "/user/", 6) || !strncmp(key, "/apps/", 6);
    if (process_current_uid() != accounts_current_uid())
        return false;
    if (!write && process_has_cap(ARK_CAP_UI) &&
        (!strncmp(key, "/user/appearance/", 17) || !strcmp(key, "/user/input/pinyin/enabled") ||
         !strcmp(key, "/user/input/keyboard") || !strcmp(key, "/user/input/pointer-scale") ||
         !strcmp(key, "/user/desktop/timezone")))
        return true;
    ProcessInfo p;
    if (!process_get_info(process_current_pid(), &p))
        return false;
    char root[64] = "/apps/";
    strcopy(root + 6, p.name, sizeof root - 7);
    size_t n = strlen(root);
    root[n++] = '/';
    root[n] = 0;
    return !strncmp(key, root, n);
}
static void object_name(char out[64], const char *key) {
    uint8_t digest[32];
    ark_sha256(key, strlen(key), digest);
    strcopy(out, "@registry.", 64);
    const char *h = "0123456789abcdef";
    for (unsigned i = 0; i < 20; i++) {
        out[10 + i * 2] = h[digest[i] >> 4];
        out[11 + i * 2] = h[digest[i] & 15];
    }
    out[50] = 0;
}
static int64_t read_value(ArkBlobRequest *b, Value *v, uint32_t uid) {
    Cached *cached = cache_find(b->name, uid);
    if (cached) {
        *v = cached->value;
        b->size = sizeof *v;
        return 0;
    }
    b->op = ARK_BLOB_READ;
    b->buffer = (uintptr_t)v;
    b->capacity = sizeof *v;
    b->offset = 0;
    int64_t r = blob_kernel_request(b, uid);
    if (r < 0)
        return r;
    if (b->size != sizeof *v || memcmp(v->magic, "ARKREG1", 8) || !key_ok(v->key, false) ||
        v->length > 512 || v->type < 1 || v->type > 3)
        return -5;
    char expected[64];
    object_name(expected, v->key);
    if (strcmp(expected, b->name))
        return -5;
    cache_put(b->name, uid, v);
    return 0;
}
static uint64_t revision = 1;
static int64_t registry_access(ArkRegistryRequest *q) {
    if (accounts_state() != ACCOUNT_ACTIVE)
        return -1;
    if (q->op > ARK_REG_REVISION ||
        !key_ok(q->key, q->op == ARK_REG_LIST || q->op == ARK_REG_REVISION))
        return -22;
    bool write = q->op == ARK_REG_SET || q->op == ARK_REG_DELETE;
    if (!permitted(q->key, write))
        return -1;
    if (q->op == ARK_REG_REVISION)
        return 0;
    uint32_t uid = !strncmp(q->key, "/system/", 8) ? 1000 : accounts_current_uid();
    ArkBlobRequest b = {0};
    Value v = {0};
    int64_t r = 0;
    q->error[0] = 0;
    if (q->op == ARK_REG_LIST) {
        char prefix[128];
        strcopy(prefix, q->key, 128);
        size_t n = strlen(prefix);
        unsigned matches = 0;
        bool found = false;
        for (unsigned i = 0;; i++) {
            memset(&b, 0, sizeof b);
            b.op = ARK_BLOB_LIST;
            b.index = i;
            r = blob_kernel_request(&b, uid);
            if (r == -2)
                break;
            if (r < 0)
                return r;
            if (strncmp(b.name, "@registry.", 10))
                continue;
            if (read_value(&b, &v, uid) < 0)
                return -5;
            if (strncmp(v.key, prefix, n) ||
                (prefix[n - 1] != '/' && v.key[n] && v.key[n] != '/') || !permitted(v.key, false))
                continue;
            if (matches++ == q->index) {
                strcopy(q->key, v.key, 128);
                q->type = v.type;
                q->length = v.length;
                q->integer = v.integer;
                memcpy(q->value, v.value, 512);
                found = true;
            }
        }
        q->count = matches;
        return found ? 0 : -2;
    }
    object_name(b.name, q->key);
    r = read_value(&b, &v, uid);
    if (r < 0 && r != -2)
        return r;
    if (!r && strcmp(v.key, q->key))
        return -17;
    if (q->op == ARK_REG_GET) {
        if (r < 0)
            return r;
        q->type = v.type;
        q->length = v.length;
        q->integer = v.integer;
        memcpy(q->value, v.value, 512);
        return 0;
    }
    if (q->op == ARK_REG_DELETE) {
        b.op = ARK_BLOB_REMOVE;
        r = blob_kernel_request(&b, uid);
        if (!r)
            cache_remove(b.name, uid);
        return r;
    }
    if (q->type < 1 || q->type > 3 || q->length > 512 ||
        (q->type == ARK_REG_INTEGER && q->length) ||
        (q->type == ARK_REG_STRING && q->length >= 512))
        return -22;
    memset(&v, 0, sizeof v);
    memcpy(v.magic, "ARKREG1", 8);
    strcopy(v.key, q->key, 128);
    v.type = q->type;
    v.length = q->length;
    v.integer = q->integer;
    memcpy(v.value, q->value, q->length);
    if (q->type == ARK_REG_STRING)
        for (unsigned i = 0; i < q->length; i++)
            if (!v.value[i])
                return -22;
    b.op = ARK_BLOB_WRITE;
    b.buffer = (uintptr_t)&v;
    b.capacity = sizeof v;
    r = blob_kernel_request(&b, uid);
    if (r < 0)
        strcopy(q->error, b.error, sizeof q->error);
    else
        cache_put(b.name, uid, &v);
    return r;
}
int64_t registry_request(ArkRegistryRequest *q) {
    int64_t r = registry_access(q);
    if (!r && (q->op == ARK_REG_SET || q->op == ARK_REG_DELETE))
        revision++;
    q->generation = revision;
    return r;
}
#ifdef ARK_REGISTRY_HOST_TEST
void registry_test_reset(void) {
    while (cache) {
        Cached *c = cache;
        cache = c->next;
        ark_free(c);
    }
    revision = 1;
}
#endif
