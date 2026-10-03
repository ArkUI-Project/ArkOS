#include "ark_api.h"
#include "package.h"
#include "ark.h"
extern const uint8_t fixture_package[], fixture_package_end[];
static uint8_t package[65536];
static void log_text(const char *s) {
    ark_syscall6(ARK_SYS_LOG, (uintptr_t)s, strlen(s), 0, 0, 0, 0);
}
static void check(bool okay, unsigned code) {
    if (!okay) {
        char n[32];
        uint_to_str(code, n);
        log_text("[package-count] FAIL ");
        log_text(n);
        log_text("\n");
        ark_exit(1);
    }
}
static void id(char out[32], unsigned i) {
    strcopy(out, "many", 32);
    char n[32];
    uint_to_str(i, n);
    strcopy(out + 4, n, 28);
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
int main(void) {
    ArkAccountRequest a = {.op = ARK_ACCOUNT_STATUS};
    check(ark_account(&a) == 0, 1);
    unsigned phase = a.status;
    a = (ArkAccountRequest){.op = phase == 0 ? ARK_ACCOUNT_ENROLL : ARK_ACCOUNT_LOGIN,
                            .secret = "Count-Test42!",
                            .user = "ark",
                            .display = "Count fixture"};
    check(ark_account(&a) == 0, 2);
    ArkPackageRequest q = {.op = ARK_PACKAGE_FIND, .id = "many47"};
    bool existing = ark_package(&q) == 0;
    size_t bytes = (size_t)(fixture_package_end - fixture_package);
    check(bytes <= sizeof package, 3);
    if (!existing) {
        memcpy(package, fixture_package, bytes);
        for (unsigned i = 0; i < 48; i++) {
            char name[32];
            id(name, i);
            memset(package + 48, 0, 32);
            strcopy((char *)package + 48, name, 32);
            uint32_t c = crc(package, 252);
            memcpy(package + 252, &c, 4);
            ArkBlobRequest b = {.op = ARK_BLOB_WRITE,
                                .capacity = (uint32_t)bytes,
                                .buffer = (uintptr_t)package,
                                .name = "count.arkpkg"};
            check(ark_call(ARK_SYS_BLOB, &b, sizeof b) == 0, 10 + i);
            q = (ArkPackageRequest){.op = ARK_PACKAGE_INSTALL, .path = "blob:count.arkpkg"};
            check(ark_package(&q) == 0 && q.info.slot == i, 70 + i);
        }
        q = (ArkPackageRequest){
            .op = ARK_PACKAGE_GRANTS, .grants = ARK_CAP_UI | ARK_CAP_FILES, .id = "many19"};
        check(ark_package(&q) == 0, 120);
    }
    for (unsigned i = 0; i < 48; i++) {
        q = (ArkPackageRequest){.op = ARK_PACKAGE_FIND};
        id(q.id, i);
        check(ark_package(&q) == 0 && q.info.slot == i && q.info.installed == 1 &&
                  q.info.grants == (i == 19 ? 6u : 4u),
              130 + i);
    }
    unsigned count = 0;
    for (;; count++) {
        q = (ArkPackageRequest){.op = ARK_PACKAGE_LIST, .index = count};
        int64_t r = ark_package(&q);
        if (r == -2)
            break;
        check(r == 0, 200);
    }
    check(count == 65, 201);
    ArkSpawn spawn = {.program = "pkg.many47"};
    check(ark_spawn(&spawn) == 0 && spawn.pid > 1, 202);
    log_text(existing ? "[package-count] PASS persisted 48 packages, stable slots and grants\n"
                      : "[package-count] PASS installed 48 packages across linked index pages\n");
    for (;;)
        ark_wait_ms(100);
}
