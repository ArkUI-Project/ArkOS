/* Native account logic + real VFS, with an independent persisted snapshot.
 * No OpenSSL is linked. Test-only inclusion exposes private crypto/parser
 * routines for standard vectors; production exposes no verifier/KDF syscall.
 * gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   -DARK_STORAGE_HOST_TEST -Iinclude tests/accounts_test.c kernel/vfs.c \
 *   -o build/accounts-test && ASAN_OPTIONS=detect_leaks=0 ./build/accounts-test
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define ARK_ACCOUNTS_HOST_TEST 1
#include "../kernel/accounts.c"

static VFile durable[VFS_MAX_FILES];
static bool mounted = true, commit_failure, system_caller = true;
static uint64_t test_ticks;
static unsigned commits;

uint64_t platform_ticks(void) {
    return test_ticks;
}
bool accounts_caller_is_system(void) {
    return system_caller;
}
bool storage_init(void) {
    if (mounted)
        memcpy(vfs_files, durable, sizeof(durable));
    return mounted;
}
bool storage_mounted(void) {
    return mounted;
}
bool storage_sync(void) {
    if (commit_failure)
        return false;
    assert(mounted);
    memcpy(durable, vfs_files, sizeof(durable));
    ++commits;
    return true;
}
void storage_mark_dirty(void) {
}
const char *storage_error(void) {
    return commit_failure ? "Injected storage failure" : "";
}
void strcopy(char *dst, const char *src, size_t cap) {
    if (!cap)
        return;
    size_t n = 0;
    while (n + 1 < cap && src[n]) {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = 0;
}
void uint_to_str(uint64_t value, char *out) {
    (void)sprintf(out, "%llu", (unsigned long long)value);
}

static void expect_hex(const uint8_t *bytes, const char *hex, size_t n) {
    uint8_t expected[64];
    assert(n <= sizeof(expected));
    assert(decode_hex(hex, strlen(hex), expected, n));
    assert(!memcmp(bytes, expected, n));
}
static void crypto_vectors(void) {
    Sha256 sha;
    uint8_t digest[32];
    sha_init(&sha);
    sha_update(&sha, "abc", 3);
    sha_final(&sha, digest);
    expect_hex(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32);
    sha_init(&sha);
    sha_final(&sha, digest);
    expect_hex(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 32);
    char thousand[1000];
    memset(thousand, 'a', sizeof(thousand));
    sha_init(&sha);
    for (unsigned i = 0; i < 1000; ++i)
        sha_update(&sha, thousand, sizeof(thousand));
    sha_final(&sha, digest);
    expect_hex(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", 32);
    uint8_t key[131];
    memset(key, 0x0b, 20);
    Hmac256 h;
    hmac_init(&h, key, 20);
    hmac_digest(&h, "Hi There", 8, digest);
    expect_hex(digest, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32);
    memset(key, 0xaa, sizeof(key));
    hmac_init(&h, key, sizeof(key));
    const char *long_message = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_digest(&h, long_message, strlen(long_message), digest);
    expect_hex(digest, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 32);
    pbkdf2("password", 8, (const uint8_t *)"salt", 4, 1, digest);
    expect_hex(digest, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b", 32);
    pbkdf2("password", 8, (const uint8_t *)"salt", 4, 2, digest);
    expect_hex(digest, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43", 32);
    pbkdf2("password", 8, (const uint8_t *)"salt", 4, 4096, digest);
    expect_hex(digest, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a", 32);
    pbkdf2("passwordPASSWORDpassword", 24, (const uint8_t *)"saltSALTsaltSALTsaltSALTsaltSALTsalt",
           36, 4096, digest);
    expect_hex(digest, "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1", 32);
    for (unsigned i = 0; i < 16; ++i)
        key[i] = (uint8_t)i;
    clock_t begin = clock();
    pbkdf2("abc", 3, key, 16, 60000, digest);
    double seconds = (double)(clock() - begin) / CLOCKS_PER_SEC;
    expect_hex(digest, "c13f2d6ff4f577a1960ceed1881411364bd6857cd6c40557201fcea1b8e0d4eb", 32);
    uint8_t other[32];
    memcpy(other, digest, 32);
    assert(equal_verifier(digest, other));
    for (unsigned i = 0; i < 32; ++i) {
        other[i] ^= 0x80;
        assert(!equal_verifier(digest, other));
        other[i] ^= 0x80;
    }
    printf("PASS crypto: SHA-256, RFC 4231 HMAC, independent PBKDF2 vectors; 60k rounds %.3f host "
           "CPU seconds\n",
           seconds);
}
static void fresh(void) {
    mounted = true;
    commit_failure = false;
    system_caller = true;
    test_ticks = 100;
    memset(durable, 0, sizeof(durable));
    vfs_init();
    assert(accounts_init());
}
static void reboot_accounts(void) {
    vfs_init();
    assert(accounts_init());
    assert(accounts_state() == ACCOUNT_LOGGED_OUT);
}
static const char *database(void) {
    int file = vfs_find(ACCOUNTS_DB_PATH);
    assert(file >= 0);
    return vfs_files[file].data;
}

static void lifecycle(void) {
    fresh();
    assert(accounts_state() == ACCOUNT_NEEDS_SETUP);
    assert(accounts_current_uid() == ACCOUNTS_UID_NONE);
    system_caller = false;
    assert(!accounts_enroll("Autumn-42!"));
    assert(!accounts_logout());
    system_caller = true;
    assert(!accounts_enroll(""));
    assert(!accounts_enroll("password"));
    assert(!accounts_enroll("12345678"));
    assert(!accounts_enroll("aaaaaaaa"));
    assert(!accounts_enroll("bad\npassword"));
    assert(!accounts_enroll("invalid\xc0\xafpassword"));
    char huge[66];
    memset(huge, 'a', 65);
    huge[65] = 0;
    assert(!accounts_enroll(huge));
    assert(accounts_enroll("Autumn-42!"));
    assert(accounts_count() == 1);
    assert(accounts_current_admin());
    assert(accounts_persistent());
    assert(accounts_current_uid() == 1000);
    assert(!strcmp(accounts_current_home(), "/home/ark"));
    assert(!accounts_enroll("Another-42!"));
    assert(!strstr(database(), "Autumn-42!"));
    AccountProfile p;
    assert(accounts_list(0, &p));
    assert(!strcmp(p.slug, "ark"));
    assert(p.flags == ACCOUNT_ADMIN);
    assert(!accounts_list(1, &p));
    assert(!accounts_list(0, 0));
    uint8_t old_salt[16];
    memcpy(old_salt, records[0].salt, 16);
    assert(!accounts_create_user("../escape", "Escape", "Child-123!", false));
    assert(!accounts_create_user("Uppercase", "Bad", "Child-123!", false));
    assert(!accounts_create_user("ark", "Duplicate", "Child-123!", false));
    assert(!accounts_create_user("root", "Reserved", "Child-123!", false));
    assert(!accounts_create_user("badname", "\xed\xa0\x80", "Child-123!", false));
    assert(vfs_mkdir("/home/occupied"));
    assert(!accounts_create_user("occupied", "Existing directory", "Child-123!", false));
    assert(accounts_create_user("mei", "小梅", "Child-123!", false));
    assert(accounts_create_user("rescue", "Backup Admin", "Backup-246!", true));
    assert(accounts_count() == 3);
    assert(accounts_profile(1001, &p));
    assert(!strcmp(p.home, "/home/mei"));
    assert(!strcmp(p.display_name, "小梅"));
    assert(memcmp(records[0].salt, records[1].salt, 16));
    assert(!strstr(database(), "Child-123!"));
    assert(accounts_path_allowed(1000, "/home/ark/Documents/new.txt", true));
    assert(!accounts_path_allowed(1000, "/home/ark", true));
    assert(accounts_path_allowed(1000, "/home/ark", false));
    assert(accounts_path_allowed(1000, "/etc/os-release", false));
    assert(!accounts_path_allowed(1000, "/etc/os-release", true));
    assert(accounts_path_allowed(1000, "/mnt/fat32/hello.txt", true));
    assert(accounts_path_allowed(1000, "/home", false));
    assert(!accounts_path_allowed(1000, "/home", true));
    assert(!accounts_path_allowed(1000, "/.system/accounts", false));
    assert(!accounts_path_allowed(1000, "/home/ark/../../.system/accounts", false));
    assert(!accounts_path_allowed(1000, "/home/ark/../mei/notes.txt", false));
    assert(!accounts_path_allowed(1000, "/home/arkevil/file", true));
    assert(!accounts_path_allowed(1001, "/home/mei/a", false));
    assert(!accounts_path_allowed(1000, "relative", true));
    uint64_t g = accounts_session_generation();
    assert(accounts_lock());
    assert(accounts_session_generation() > g);
    assert(accounts_current_uid() == ACCOUNTS_UID_NONE);
    assert(accounts_session_profile(&p) && p.uid == 1000);
    assert(!accounts_path_allowed(1000, "/home/ark/file", false));
    assert(!accounts_login("rescue", "Backup-246!"));
    assert(!accounts_unlock("wrong-last-character?"));
    assert(accounts_unlock("Autumn-42!"));
    assert(!accounts_change_password("bad password", "Second-42!"));
    assert(accounts_change_password("Autumn-42!", "Second-42!"));
    assert(memcmp(old_salt, records[0].salt, 16));
    assert(!strstr(database(), "Second-42!"));
    assert(accounts_set_disabled(1001, true));
    assert(!accounts_set_disabled(1000, true));
    reboot_accounts();
    assert(!accounts_login("ark", "Autumn-42!"));
    assert(accounts_login("ark", "Second-42!"));
    assert(accounts_count() == 3);
    assert(accounts_profile(1001, &p) && p.flags == ACCOUNT_DISABLED);
    assert(accounts_logout());
    assert(!accounts_login("mei", "Child-123!"));
    assert(accounts_login("ark", "Second-42!"));
    assert(accounts_set_disabled(1001, false));
    assert(accounts_logout());
    assert(accounts_login("mei", "Child-123!"));
    assert(!accounts_current_admin());
    assert(!accounts_create_user("unauthorized", "Bad", "Secret-123!", false));
    assert(!accounts_set_disabled(1000, true));
    assert(accounts_path_allowed(1001, "/home/mei/file", true));
    assert(!accounts_path_allowed(1001, "/home/ark/file", false));
    system_caller = false;
    assert(!accounts_lock());
    assert(!accounts_logout());
    assert(!accounts_change_password("Child-123!", "Changed-123!"));
    assert(accounts_state() == ACCOUNT_ACTIVE);
    system_caller = true;
    assert(accounts_logout());
    assert(accounts_login("rescue", "Backup-246!"));
    assert(accounts_set_disabled(1000, true));
    assert(!accounts_set_disabled(1002, true));
    assert(accounts_set_disabled(1000, false));
    puts("PASS lifecycle: setup, no stored plaintext, profiles, UTF-8, private homes, lock/unlock, "
         "password changes, reboot, admin controls");
}
static void throttling(void) {
    assert(accounts_logout());
    test_ticks = 10000;
    for (unsigned i = 0; i < 5; ++i)
        assert(!accounts_login(i & 1 ? "ark" : "unknown", "wrong-password"));
    assert(accounts_retry_after_ticks() == 3000);
    assert(!accounts_login("ark", "Second-42!"));
    test_ticks = 9000;
    assert(accounts_retry_after_ticks() == 3000);
    test_ticks = 12999;
    assert(accounts_retry_after_ticks() == 1);
    test_ticks = 13000;
    assert(!accounts_retry_after_ticks());
    assert(accounts_login("ark", "Second-42!"));
    for (unsigned i = 3; i < ACCOUNTS_MAX; ++i) {
        char slug[24], name[64];
        sprintf(slug, "user%u", i);
        sprintf(name, "User %u", i);
        assert(accounts_create_user(slug, name, "Valid-Password9!", false));
    }
    assert(accounts_count() == ACCOUNTS_MAX);
    assert(!accounts_create_user("overflow", "Ninth", "Valid-Password9!", false));
    reboot_accounts();
    assert(accounts_count() == ACCOUNTS_MAX);
    assert(accounts_login("ark", "Second-42!"));
    puts("PASS rate limit and bounds: shared unknown-user cooldown, monotonic timer recovery, "
         "eight accounts persist");
}
static void malformed_database(void) {
    char valid[DB_CAP];
    strcopy(valid, database(), sizeof(valid));
    int f = vfs_find(ACCOUNTS_DB_PATH);
    assert(f >= 0);
    const char *bad[] = {"", "ARKACCT1|0000000000000000\n",
                         "ARKACCT1|0000000000000001\n1000|1|../ark|41726b|00|00|60000\n"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(vfs_write(f, bad[i]));
        assert(!accounts_init());
        assert(accounts_state() == ACCOUNT_ERROR);
        assert(!accounts_enroll("Reset-123!"));
    }
    char edited[DB_CAP];
    strcopy(edited, valid, sizeof(edited));
    char *rounds = strstr(edited, "|60000\n");
    assert(rounds);
    rounds[1] = '9';
    assert(vfs_write(f, edited));
    assert(!accounts_init()); /* bounded KDF work factor */
    strcopy(edited, valid, sizeof(edited));
    char *second = strstr(edited, "1001|");
    assert(second);
    second[3] = '0';
    assert(vfs_write(f, edited));
    assert(!accounts_init()); /* duplicate UID */
    strcopy(edited, valid, sizeof(edited));
    second = strstr(edited, "|mei|");
    assert(second);
    memcpy(second + 1, "ark", 3);
    assert(vfs_write(f, edited));
    assert(!accounts_init()); /* duplicate slug */
    strcopy(edited, valid, sizeof(edited));
    second = strstr(edited, "|mei|");
    assert(second);
    memcpy(second + 1, "../", 3);
    assert(vfs_write(f, edited));
    assert(!accounts_init()); /* traversal */
    strcopy(edited, valid, sizeof(edited));
    second = strstr(edited, "|41726b|");
    assert(second);
    memcpy(second + 1, "c08000", 6);
    assert(vfs_write(f, edited));
    assert(!accounts_init()); /* overlong UTF-8/NUL */
    strcopy(edited, valid, sizeof(edited));
    size_t length = strlen(edited);
    for (size_t cut = 0; cut < length; cut += 17) {
        edited[cut] = 0;
        assert(vfs_write(f, edited));
        (void)accounts_init();
        assert(accounts_count() <= 8);
        edited[cut] = valid[cut];
    }
    /* Embedded NUL/declared size and maximum-sized malformed records. */
    assert(vfs_write(f, valid));
    vfs_files[f].data[35] = 0;
    assert(!accounts_init());
    memset(vfs_files[f].data, 'X', VFS_FILE_CAP - 1);
    vfs_files[f].data[VFS_FILE_CAP - 1] = 0;
    vfs_files[f].size = VFS_FILE_CAP - 1;
    assert(!accounts_init());
    assert(vfs_write(f, valid));
    assert(accounts_init());
    assert(!accounts_session_profile(&(AccountProfile){0}));
    assert(accounts_login("ark", "Second-42!"));
    puts("PASS malformed storage: corrupt header/lengths/UTF-8, duplicate users, traversal, "
         "excessive KDF rounds rejected; no silent reset");
}
static void write_failures(void) {
    unsigned old_commits = commits;
    commit_failure = true;
    assert(!accounts_change_password("Second-42!", "Uncommitted-99!"));
    assert(commits == old_commits);
    assert(accounts_state() == ACCOUNT_ERROR);
    assert(accounts_current_uid() == ACCOUNTS_UID_NONE);
    assert(!accounts_path_allowed(1000, "/home/ark/a", false));
    assert(!accounts_enroll("Reset-123!"));
    commit_failure = false;
    reboot_accounts();
    assert(!accounts_login("ark", "Uncommitted-99!"));
    assert(accounts_login("ark", "Second-42!"));
    fresh();
    commit_failure = true;
    assert(!accounts_enroll("First-Failure9!"));
    assert(accounts_state() == ACCOUNT_ERROR);
    commit_failure = false;
    vfs_init();
    assert(accounts_init());
    assert(accounts_state() == ACCOUNT_NEEDS_SETUP);
    mounted = false;
    vfs_init();
    assert(accounts_init());
    assert(!accounts_persistent());
    assert(accounts_enroll("Temporary-123!"));
    assert(accounts_state() == ACCOUNT_ACTIVE);
    vfs_init();
    assert(accounts_init());
    assert(accounts_state() == ACCOUNT_NEEDS_SETUP);
    puts("PASS storage failures: rejected commits close session; independent persisted snapshot "
         "reboots; RAM mode explicitly volatile");
}
int main(void) {
    crypto_vectors();
    lifecycle();
    throttling();
    malformed_database();
    write_failures();
    puts("ALL ACCOUNT TESTS PASSED");
    return 0;
}
