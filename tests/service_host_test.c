/* Real syscall dispatcher + VFS + account policy. Fake process memory uses
 * integer addresses and per-page permissions: no direct host-pointer shortcut.
 * gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   -DARK_STORAGE_HOST_TEST -DARK_ACCOUNTS_HOST_TEST -Iinclude \
 *   tests/service_host_test.c kernel/services.c kernel/vfs.c kernel/accounts.c \
 *   -o build/service-host-test
 * ASAN_OPTIONS=detect_leaks=0 ./build/service-host-test
 */
#define ARK_KERNEL
#include "ark_api.h"
#include "package.h"
#include "process.h"
#include "permissions.h"
#include "ark_catalog.h"
#include "accounts.h"
#include "gpu.h"
#include "block.h"
#include "storage.h"
#include "extfs.h"
#include "net.h"
#include "device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 4096u
#define ARENA_SIZE (16u * 1024u * 1024u)
#define BASE PROCESS_USER_BASE
#define REQUEST (BASE + PAGE)
#define PAYLOAD (BASE + 16u * PAGE)
static uint8_t arena[ARENA_SIZE], permissions[ARENA_SIZE / PAGE];
static uint32_t pid = 1, uid = ACCOUNTS_UID_NONE;
static uint64_t caps = ARK_CAP_SYSTEM;
static unsigned checks, failures, revocations, copies_from, presents, cursor_moves;
static uint32_t presented[32];
static GpuRect last_damage;
static bool animation;
static NetHttpState http_state;
static unsigned http_cancels;
static const char body[] = "private HTTP body";
static const NetStatus network = {.present = true, .link = true, .configured = true};
static const GpuStats stats = {0};
static const char password[] = "Autumn-42!";
static BootInfo boot = {.width = 4, .height = 3, .bpp = 32, .memory_mib = 512};
void services_init(const BootInfo *info);

#define CHECK(test)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(test)) {                                                                             \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test);                        \
        }                                                                                          \
    } while (0)
static void caller(uint32_t p, uint64_t c) {
    pid = p;
    caps = c;
    uid = accounts_current_uid();
}
bool process_user_range(uint64_t address, size_t bytes, bool write) {
    if (address < BASE || address >= BASE + ARENA_SIZE || bytes > ARENA_SIZE ||
        bytes > BASE + ARENA_SIZE - address)
        return false;
    if (!bytes)
        return true;
    size_t first = (size_t)(address - BASE) / PAGE,
           last = (size_t)(address - BASE + bytes - 1) / PAGE;
    for (size_t p = first; p <= last; p++)
        if (!(permissions[p] & 1) || (write && !(permissions[p] & 2)))
            return false;
    return true;
}
bool process_copy_from_user(void *dst, uint64_t src, size_t bytes) {
    if (!process_user_range(src, bytes, false))
        return false;
    ++copies_from;
    memcpy(dst, arena + (src - BASE), bytes);
    return true;
}
bool process_copy_to_user(uint64_t dst, const void *src, size_t bytes) {
    if (!process_user_range(dst, bytes, true))
        return false;
    memcpy(arena + (dst - BASE), src, bytes);
    return true;
}
static bool catalog_model;
static const char *model_names[] = {"notes", "browser", "timer"};
static uint64_t model_caps[] = {4, 4, 36};
bool process_get_info(uint32_t p, ProcessInfo *out) {
    if (!catalog_model || p < 50 || p > 52)
        return false;
    memset(out, 0, sizeof *out);
    out->pid = p;
    out->uid = 1000;
    out->state = PROCESS_RUNNING;
    strcopy(out->name, model_names[p - 50], sizeof out->name);
    return true;
}
uint32_t process_current_pid(void) {
    return pid;
}
uint32_t process_current_uid(void) {
    return uid;
}
uint64_t process_current_caps(void) {
    return caps;
}
bool process_has_cap(uint64_t c) {
    return (caps & ARK_CAP_SYSTEM) || (caps & c) == c;
}
bool process_set_current_uid(uint32_t value) {
    uid = value;
    return true;
}
unsigned process_revoke_user_tasks(void) {
    ++revocations;
    return 0;
}
int process_spawn_elf(const void *image, size_t bytes, const char *name, uint32_t owner,
                      uint64_t rights) {
    (void)image;
    (void)bytes;
    (void)name;
    CHECK(owner == ACCOUNTS_FIRST_UID);
    CHECK(rights == (!strcmp(name, "clock") ? ARK_CAP_UI : ARK_CAP_FILES | ARK_CAP_UI));
    return 42;
}
static uint64_t updated_caps = UINT64_MAX;
void process_update_caps(const char *name, uint32_t owner, uint64_t rights) {
    if (catalog_model) {
        for (unsigned i = 0; i < 3; i++)
            if (!strcmp(name, model_names[i]))
                model_caps[i] = rights;
    } else
        CHECK(!strcmp(name, "clock"));
    CHECK(owner == 1000);
    updated_caps = rights;
}
int64_t registry_request(ArkRegistryRequest *q) {
    (void)q;
    return -38;
}
int64_t blob_request(ArkBlobRequest *q) {
    (void)q;
    return -5;
}
/* Package transactions are exercised against the real module in package_host_test. */
void *process_kernel_alloc(size_t n) {
    return calloc(1, n);
}
void process_kernel_free(void *p, size_t n) {
    (void)n;
    free(p);
}
const uint8_t *package_system_image(unsigned catalog, size_t *bytes) {
    static const uint8_t image[] = {0};
    (void)catalog;
    *bytes = sizeof image;
    return image;
}
int64_t package_request(ArkPackageRequest *q) {
    (void)q;
    return -38;
}
/* The .arco loader is exercised against the real module in module_host_test;
 * here it only has to prove the syscall boundary validates its request the
 * same way every other service does. */
static ArkDriverRequest last_driver_request;
static int64_t driver_result;
int64_t module_request(ArkDriverRequest *q) {
    last_driver_request = *q;
    return driver_result;
}
int64_t package_spawn(ArkSpawn *q) {
    (void)q;
    return -2;
}
int64_t package_launch_info(uint32_t pid, ArkLaunchInfo *q) {
    (void)pid;
    (void)q;
    return -2;
}
void package_process_exit(uint32_t pid) {
    (void)pid;
}
int64_t package_permissions(ArkPermissionRequest *q) {
    (void)q;
    return -1;
}
int package_activity_identity(uint32_t pid, ArkActivityRequest *q) {
    (void)pid;
    (void)q;
    return -1;
}

int64_t installer_request(ArkInstallRequest *q) {
    (void)q;
    return -5;
}
#define ELF_STUB(name)                                                                             \
    __asm__(".pushsection .rodata\n.global _binary_build_apps_" #name                              \
            "_elf_start\n_binary_build_apps_" #name                                                \
            "_elf_start:\n.byte 0\n.global _binary_build_apps_" #name                              \
            "_elf_end\n_binary_build_apps_" #name "_elf_end:\n.popsection\n")
ELF_STUB(clock);
ELF_STUB(paint);
ELF_STUB(markdown);
ELF_STUB(notes);
ELF_STUB(browser);
ELF_STUB(calculator);
ELF_STUB(todo);
ELF_STUB(timer);
ELF_STUB(wasm);
ELF_STUB(calendar);
ELF_STUB(reminders);
uint64_t platform_ticks(void) {
    return 100;
}
static uint64_t now_ms = 1007;
uint64_t platform_millis(void) {
    return now_ms;
}
void platform_time(int *h, int *m, int *s) {
    *h = 12;
    *m = 34;
    *s = 56;
}
bool platform_datetime(int *y, int *mo, int *d, int *h, int *m, int *s, int *w) {
    *y = 2026;
    *mo = 10;
    *d = 3;
    *w = 6;
    platform_time(h, m, s);
    return true;
}
bool platform_next_event(Event *e) {
    (void)e;
    return false;
}
bool virtio_input_next_event(Event *e) {
    (void)e;
    return false;
}
const char *virtio_input_name(void) {
    return "host input stub";
}
unsigned virtio_input_contacts(void) {
    return 0;
}
void platform_reboot(void) {
    CHECK(false);
}
void platform_poweroff(void) {
    CHECK(false);
}
void serial_write(const char *s) {
    (void)s;
}
void gpu_present(const uint32_t *p, unsigned stride, const GpuRect *d) {
    ++presents;
    animation = false;
    last_damage = *d;
    if (boot.width * boot.height <= 32)
        for (unsigned y = 0; y < boot.height; y++)
            memcpy(presented + y * boot.width, p + y * stride, boot.width * 4);
}
void gpu_present_animation(const uint32_t *p, unsigned stride) {
    GpuRect d = {0, 0, (int)boot.width, (int)boot.height};
    gpu_present(p, stride, &d);
    animation = true;
}
void gpu_present_animation_damage(const uint32_t *p, unsigned stride, const GpuRect *d) {
    gpu_present(p, stride, d);
    animation = true;
}
bool gpu_cursor_move(int x, int y, bool visible) {
    (void)x;
    (void)y;
    (void)visible;
    ++cursor_moves;
    return true;
}
void gpu_cursor_set_scale(unsigned scale) {
    (void)scale;
}
void gpu_cursor_set_shape(unsigned shape) {
    CHECK(shape < ARK_CURSOR_COUNT);
}
void block_statistics(BlockStats *out) {
    memset(out, 0, sizeof *out);
}
/* Device model seams: unit 0 is the system volume, unit 1 a removable disk. */
static BlockDevice device_disks[2];
static unsigned device_reads, device_wakes;
BlockDevice *block_device(unsigned id) {
    return id < 2 ? &device_disks[id] : 0;
}
bool block_read(BlockDevice *d, uint64_t lba, uint32_t sectors, void *buffer) {
    if (!d || !d->present || lba >= d->sectors || sectors > d->sectors - lba)
        return false;
    device_reads++;
    memset(buffer, 0x5a, sectors * 512u);
    return true;
}
bool block_write(BlockDevice *d, uint64_t lba, uint32_t sectors, const void *buffer) {
    (void)buffer;
    return d && d->present && lba < d->sectors && sectors <= d->sectors - lba;
}
bool block_flush(BlockDevice *d) {
    return d && d->present;
}
const char *block_error(void) {
    return "host block stub";
}
void process_wake(uint32_t target) {
    device_wakes++;
    (void)target;
}
const char *gpu_backend_name(void) {
    return "host GPU stub";
}
const GpuStats *gpu_stats(void) {
    return &stats;
}
int64_t virtio_gpu_glass(ArkGlassRequest *q) {
    (void)q;
    return -19;
}
bool storage_init(void) {
    return false;
}
bool storage_mounted(void) {
    return false;
}
bool storage_sync(void) {
    return true;
}
void storage_mark_dirty(void) {
}
const char *storage_status(void) {
    return "host volatile storage";
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
bool extfs_read_bytes(int i, uint64_t off, void *b, size_t cap, size_t *n) {
    (void)i;
    (void)off;
    (void)b;
    (void)cap;
    *n = 0;
    return false;
}
bool extfs_path(const char *p) {
    return !strncmp(p, "/mnt/", 5);
}
bool extfs_path_writable(const char *p) {
    (void)p;
    return false;
}
bool extfs_volume_info(unsigned i, ExtVolumeInfo *v) {
    (void)i;
    (void)v;
    return false;
}
const char *extfs_status(void) {
    return "not mounted";
}
void strcopy(char *d, const char *s, size_t n) {
    if (n) {
        size_t i = 0;
        while (i + 1 < n && s[i]) {
            d[i] = s[i];
            ++i;
        }
        d[i] = 0;
    }
}
void uint_to_str(uint64_t v, char *out) {
    (void)sprintf(out, "%llu", (unsigned long long)v);
}
void net_poll(void) {
}
const NetStatus *net_status(void) {
    return &network;
}
bool net_http_get(const char *url) {
    http_state = !strncmp(url, "http://", 7) ? NET_HTTP_DONE : NET_HTTP_ERROR;
    return http_state != NET_HTTP_ERROR;
}
void net_http_cancel(void) {
    ++http_cancels;
    http_state = NET_HTTP_IDLE;
}
NetHttpState net_http_state(void) {
    return http_state;
}
const char *net_http_body(void) {
    return body;
}
size_t net_http_length(void) {
    return sizeof(body) - 1;
}
unsigned net_http_status(void) {
    return 200;
}
const char *net_http_error(void) {
    return "invalid URL";
}
const char *net_http_content_type(void) {
    return "text/plain";
}

static int64_t dispatch(uint64_t op, uint64_t ptr, size_t size) {
    return process_syscall_dispatch(op, ptr, size, 0, 0, 0, 0);
}
static int64_t call(uint64_t op, void *q, size_t size) {
    memcpy(arena + (REQUEST - BASE), q, size);
    int64_t result = dispatch(op, REQUEST, size);
    memcpy(q, arena + (REQUEST - BASE), size);
    return result;
}
#define CALL(op, q) call(op, &(q), sizeof(q))
static ArkFileRequest file_request(unsigned op, const char *path) {
    ArkFileRequest q = {.op = op, .buffer = PAYLOAD, .capacity = 64};
    strcopy(q.path, path, sizeof q.path);
    return q;
}
static int64_t account(unsigned op) {
    ArkAccountRequest q = {.op = op};
    strcopy(q.secret, password, sizeof q.secret);
    return CALL(ARK_SYS_ACCOUNT, q);
}
static void requests_and_accounts(void) {
    ArkAccountRequest a = {.op = ARK_ACCOUNT_STATUS};
    CHECK(CALL(ARK_SYS_ACCOUNT, a) == 0);
    CHECK(a.uid == ACCOUNTS_UID_NONE);
    CHECK(a.status == ACCOUNT_NEEDS_SETUP);
    CHECK(dispatch(ARK_SYS_ACCOUNT, 0, sizeof a) == -14);
    CHECK(dispatch(ARK_SYS_ACCOUNT, UINT64_MAX - 16, sizeof a) == -14);
    CHECK(dispatch(ARK_SYS_ACCOUNT, BASE + ARENA_SIZE - 8, sizeof a) == -14);
    CHECK(dispatch(ARK_SYS_ACCOUNT, REQUEST, sizeof a - 1) == -14);
    memcpy(arena + (REQUEST - BASE), &a, sizeof a);
    permissions[1] = 1;
    CHECK(dispatch(ARK_SYS_ACCOUNT, REQUEST, sizeof a) == -14);
    permissions[1] = 3;
    uint64_t crossing = BASE + 2 * PAGE - 8;
    memcpy(arena + (crossing - BASE), &a, sizeof a);
    permissions[2] = 0;
    CHECK(dispatch(ARK_SYS_ACCOUNT, crossing, sizeof a) == -14);
    permissions[2] = 3;
    caller(2, ARK_CAP_FILES | ARK_CAP_UI | ARK_CAP_NETWORK | ARK_CAP_PROCESS);
    CHECK(account(ARK_ACCOUNT_ENROLL) == -1);
    ArkFileRequest f = file_request(ARK_FILE_LIST, "/");
    CHECK(CALL(ARK_SYS_FILE, f) == -1);
    caller(1, ARK_CAP_SYSTEM);
    unsigned old = revocations;
    CHECK(account(ARK_ACCOUNT_ENROLL) == 0);
    CHECK(revocations == old + 1);
    CHECK(uid == 1000);
    a = (ArkAccountRequest){.op = ARK_ACCOUNT_STATUS};
    memset(a.secret, 'x', sizeof a.secret);
    CHECK(CALL(ARK_SYS_ACCOUNT, a) == -22);
    a = (ArkAccountRequest){.op = ARK_ACCOUNT_LIST, .index = 100};
    CHECK(CALL(ARK_SYS_ACCOUNT, a) == -1);
    CHECK(a.uid == ACCOUNTS_UID_NONE);
    a = (ArkAccountRequest){.op = ARK_ACCOUNT_STATUS};
    strcopy(a.secret, "untrusted secret", sizeof a.secret);
    strcopy(a.new_secret, "another secret", sizeof a.new_secret);
    CHECK(CALL(ARK_SYS_ACCOUNT, a) == 0);
    CHECK(a.secret[0] == 0 && a.new_secret[0] == 0);
    CHECK(a.uid == 1000);
    puts("checked request sizes, unmapped/readonly/cross-page pointers and account identity");
}
static void file_boundaries(void) {
    const char *blocked[] = {"/.system/accounts",      "//.system//accounts",
                             "/./.system/accounts",    "/home/ark/../../.system/accounts",
                             "../../.system/accounts", "/home/ark/../other/private",
                             "/home/arkevil/private"};
    const unsigned ops[] = {ARK_FILE_STAT,   ARK_FILE_READ,  ARK_FILE_READ_BYTES, ARK_FILE_WRITE,
                            ARK_FILE_CREATE, ARK_FILE_MKDIR, ARK_FILE_REMOVE,     ARK_FILE_RENAME};
    CHECK(vfs_mkdir("/home/other"));
    CHECK(vfs_create("/home/other/private") >= 0);
    for (unsigned sys = 0; sys < 2; sys++) {
        caller(sys ? 1 : 2, sys ? ARK_CAP_SYSTEM : ARK_CAP_FILES);
        for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; i++)
            for (size_t j = 0; j < sizeof ops / sizeof ops[0]; j++) {
                ArkFileRequest f = file_request(ops[j], blocked[i]);
                CHECK(CALL(ARK_SYS_FILE, f) == -1);
            }
    }
    caller(2, 0);
    ArkFileRequest f = file_request(ARK_FILE_STAT, "/");
    CHECK(CALL(ARK_SYS_FILE, f) == -1);
    caller(2, ARK_CAP_FILES);
    f = file_request(ARK_FILE_CREATE, "service.txt");
    CHECK(CALL(ARK_SYS_FILE, f) == 0);
    CHECK(!strcmp(f.info.path, "/home/ark/service.txt"));
    memcpy(arena + (PAYLOAD - BASE), "abc", 3);
    f = file_request(ARK_FILE_WRITE, "service.txt");
    f.capacity = 3;
    CHECK(CALL(ARK_SYS_FILE, f) == 0);
    CHECK(f.count == 3);
    f = file_request(ARK_FILE_WRITE, "service.txt");
    f.capacity = ARK_FILE_MAX + 1;
    CHECK(CALL(ARK_SYS_FILE, f) == -22);
    f = file_request(ARK_FILE_WRITE, "service.txt");
    f.capacity = 3;
    f.offset = UINT64_MAX;
    CHECK(CALL(ARK_SYS_FILE, f) == -22);
    f.offset = 0;
    f.buffer = UINT64_MAX - 1;
    CHECK(CALL(ARK_SYS_FILE, f) == -14);
    f.buffer = PAYLOAD;
    memcpy(arena + (PAYLOAD - BASE), "a\0c", 3);
    CHECK(CALL(ARK_SYS_FILE, f) == -22);
    CHECK(!strcmp(vfs_entry(vfs_find("/home/ark/service.txt"))->data, "abc"));
    f = file_request(ARK_FILE_READ, "service.txt");
    f.buffer = UINT64_MAX - 1;
    CHECK(CALL(ARK_SYS_FILE, f) == -14);
    f.buffer = PAYLOAD;
    permissions[(PAYLOAD - BASE) / PAGE] = 1;
    CHECK(CALL(ARK_SYS_FILE, f) == -14);
    permissions[(PAYLOAD - BASE) / PAGE] = 3;
    CHECK(CALL(ARK_SYS_FILE, f) == 0);
    CHECK(f.count == 3 && !memcmp(arena + (PAYLOAD - BASE), "abc", 3));
    f.offset = ARK_FILE_MAX;
    CHECK(CALL(ARK_SYS_FILE, f) == 0);
    CHECK(f.count == 0);
    f = file_request(ARK_FILE_READ_BYTES, "service.txt");
    f.offset = 1;
    f.capacity = 2;
    CHECK(CALL(ARK_SYS_FILE, f) == 0 && f.count == 2 && !memcmp(arena + (PAYLOAD - BASE), "bc", 2));
    f.offset = UINT64_MAX;
    CHECK(CALL(ARK_SYS_FILE, f) == 0 && f.count == 0);
    f.capacity = ARK_FILE_MAX + 1;
    CHECK(CALL(ARK_SYS_FILE, f) == -22);
    f.capacity = 1;
    f.buffer = UINT64_MAX;
    CHECK(CALL(ARK_SYS_FILE, f) == -14);
    f = file_request(ARK_FILE_READ, "/home/ark");
    CHECK(CALL(ARK_SYS_FILE, f) == -5);
    f = file_request(ARK_FILE_WRITE, "/etc/os-release");
    CHECK(CALL(ARK_SYS_FILE, f) == -1);
    for (unsigned op = ARK_FILE_RENAME; op <= ARK_FILE_COPY; op++) {
        f = file_request(op, "service.txt");
        strcopy(f.other, "../../.system/alias", sizeof f.other);
        CHECK(CALL(ARK_SYS_FILE, f) == -1);
    }
    f = file_request(ARK_FILE_SNAPSHOT, "");
    CHECK(CALL(ARK_SYS_FILE, f) == -1);
    f = file_request(ARK_FILE_LIST, "/home");
    f.capacity = ARK_FILE_SLOTS;
    CHECK(CALL(ARK_SYS_FILE, f) == 0);
    ArkFileInfo *items = (ArkFileInfo *)(void *)(arena + (PAYLOAD - BASE));
    bool own = false;
    for (unsigned i = 0; i < f.count; i++) {
        CHECK(strcmp(items[i].path, "/home/other") != 0);
        own |= !strcmp(items[i].path, "/home/ark");
    }
    CHECK(own);
    caller(1, ARK_CAP_SYSTEM);
    f = file_request(ARK_FILE_SNAPSHOT, "");
    f.capacity = ARK_FILE_SLOTS;
    CHECK(CALL(ARK_SYS_FILE, f) == 0);
    for (unsigned i = 0; i < f.count; i++)
        if (items[i].flags & ARK_FILE_USED) {
            CHECK(strncmp(items[i].path, "/.system", 8) != 0);
            CHECK(strncmp(items[i].path, "/home/other", 11) != 0);
        }
    f.capacity = UINT32_MAX;
    CHECK(CALL(ARK_SYS_FILE, f) == -14);
    puts(
        "checked scoped paths, .system aliases, metadata filtering and file length/pointer bounds");
}
static void surface_boundaries(void) {
    caller(10, ARK_CAP_UI);
    ArkSurfaceRequest s = {.op = ARK_SURFACE_QUERY, .id = 7};
    CHECK(CALL(ARK_SYS_SURFACE, s) == -1);
    s.op = ARK_SURFACE_COPY;
    CHECK(CALL(ARK_SYS_SURFACE, s) == -1);
    s.op = ARK_SURFACE_SEND_EVENT;
    CHECK(CALL(ARK_SYS_SURFACE, s) == -1);
    s = (ArkSurfaceRequest){.op = ARK_SURFACE_CREATE, .width = 0, .height = 2};
    CHECK(CALL(ARK_SYS_SURFACE, s) == -22);
    s.width = ARK_SURFACE_MAX_W + 1;
    CHECK(CALL(ARK_SYS_SURFACE, s) == -22);
    s.width = 2;
    memset(s.title, 'x', sizeof s.title);
    CHECK(CALL(ARK_SYS_SURFACE, s) == -22);
    memset(s.title, 0, sizeof s.title);
    s.pid = 999;
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    uint32_t id = s.id;
    CHECK(s.pid == 10 && s.generation == 1);
    s.op = ARK_SURFACE_CREATE;
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    uint32_t second = s.id;
    CHECK(second != id);
    s.op = ARK_SURFACE_INPUT;
    s.flags = ARK_SURFACE_TEXT_INPUT;
    s.damage = (ArkRect){0, 1, 1, 1};
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    s.op = ARK_SURFACE_RESIZE;
    s.width = 4;
    s.height = 3;
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0 && s.width == 4);
    s.op = ARK_SURFACE_CLOSE;
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    uint32_t pixels[] = {1, 2, 0xff, 3, 4, 0xff};
    memcpy(arena + (PAYLOAD - BASE), pixels, sizeof pixels);
    s = (ArkSurfaceRequest){.op = ARK_SURFACE_PRESENT,
                            .id = id,
                            .width = 2,
                            .height = 2,
                            .stride = 3,
                            .pixels = PAYLOAD};
    unsigned before = copies_from;
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    CHECK(s.generation == 2);
    CHECK(copies_from - before == 3);
    uint32_t want[] = {1, 2, 3, 4};
    memcpy(arena + (PAYLOAD - BASE), want, sizeof want);
    s.stride = 2;
    before = copies_from;
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    CHECK(s.generation == 3);
    CHECK(copies_from - before == 2);
    s.pixels = UINT64_MAX - 4;
    CHECK(CALL(ARK_SYS_SURFACE, s) == -14);
    s.pixels = PAYLOAD;
    s.stride = UINT32_MAX;
    CHECK(CALL(ARK_SYS_SURFACE, s) == -1);
    ArkCursorRegion region = {.rect = {0, 0, 2, 2}, .shape = ARK_CURSOR_TEXT};
    memcpy(arena + (PAYLOAD - BASE), &region, sizeof region);
    s = (ArkSurfaceRequest){.op = ARK_SURFACE_CURSORS, .id = id, .pixels = PAYLOAD, .capacity = 1};
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    s.capacity = 33;
    CHECK(CALL(ARK_SYS_SURFACE, s) == -22);
    s.capacity = 1;
    s.pixels = UINT64_MAX;
    CHECK(CALL(ARK_SYS_SURFACE, s) == -14);
    s.pixels = PAYLOAD;
    region.rect.w = INT32_MAX;
    memcpy(arena + (PAYLOAD - BASE), &region, sizeof region);
    CHECK(CALL(ARK_SYS_SURFACE, s) == -22);
    region.rect.w = 2;
    region.shape = ARK_CURSOR_COUNT;
    memcpy(arena + (PAYLOAD - BASE), &region, sizeof region);
    CHECK(CALL(ARK_SYS_SURFACE, s) == -22);
    caller(1, ARK_CAP_SYSTEM);
    s = (ArkSurfaceRequest){.op = ARK_SURFACE_CURSOR_AT, .id = id, .event = {.x = 1, .y = 1}};
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0 && s.flags == ARK_CURSOR_TEXT);
    s.event.x = 2;
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0 && s.flags == ARK_CURSOR_ARROW);
    caller(11, ARK_CAP_UI);
    unsigned forbidden[] = {ARK_SURFACE_PRESENT,    ARK_SURFACE_QUERY,      ARK_SURFACE_COPY,
                            ARK_SURFACE_SEND_EVENT, ARK_SURFACE_NEXT_EVENT, ARK_SURFACE_CLOSE,
                            ARK_SURFACE_CURSORS,    ARK_SURFACE_CURSOR_AT};
    for (size_t i = 0; i < sizeof forbidden / sizeof forbidden[0]; i++) {
        s.op = forbidden[i];
        CHECK(CALL(ARK_SYS_SURFACE, s) == -1);
    }
    caller(1, ARK_CAP_SYSTEM);
    s = (ArkSurfaceRequest){.op = ARK_SURFACE_COPY, .id = id, .pixels = PAYLOAD, .capacity = 3};
    CHECK(CALL(ARK_SYS_SURFACE, s) == -22);
    s.capacity = 4;
    s.pixels = UINT64_MAX;
    CHECK(CALL(ARK_SYS_SURFACE, s) == -14);
    s.pixels = PAYLOAD;
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    CHECK(s.generation == 3);
    CHECK(!memcmp(arena + (PAYLOAD - BASE), want, sizeof want));
    s.op = ARK_SURFACE_SEND_EVENT;
    s.event = (ArkEvent){.type = ARK_EV_KEY, .key = 'x'};
    for (unsigned i = 0; i < 31; i++)
        CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    CHECK(CALL(ARK_SYS_SURFACE, s) == -16);
    caller(10, ARK_CAP_UI);
    s.op = ARK_SURFACE_NEXT_EVENT;
    for (unsigned i = 0; i < 31; i++) {
        CHECK(CALL(ARK_SYS_SURFACE, s) == 1);
        CHECK(s.event.key == 'x');
    }
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    process_exit_notify(10);
    caller(1, ARK_CAP_SYSTEM);
    s.op = ARK_SURFACE_QUERY;
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    CHECK(s.flags == 0);
    puts("checked surface size/pointer limits, actual PID ownership, privileged queries/copies and "
         "event queue bounds");
}
static void drag_and_input(void) {
    catalog_model = true;
    caller(50, ARK_CAP_UI);
    ArkSurfaceRequest s = {.op = ARK_SURFACE_CREATE, .width = 16, .height = 16};
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    unsigned source = s.id;
    caller(51, ARK_CAP_UI);
    s = (ArkSurfaceRequest){.op = ARK_SURFACE_CREATE, .width = 16, .height = 16};
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    unsigned target = s.id;
    caller(50, ARK_CAP_UI);
    ArkDragRequest d = {.op = ARK_DRAG_BEGIN,
                        .source = source,
                        .kind = ARK_DRAG_TEXT,
                        .action = ARK_DRAG_COPY,
                        .length = 3};
    strcpy(d.data, "abc");
    strcpy(d.mime, "text/plain");
    CHECK(CALL(ARK_SYS_DRAG, d) == 0);
    unsigned token = d.token;
    caller(52, ARK_CAP_UI);
    d.op = ARK_DRAG_STATUS;
    CHECK(CALL(ARK_SYS_DRAG, d) == -1);
    d.op = ARK_DRAG_DELIVER;
    d.target = target;
    CHECK(CALL(ARK_SYS_DRAG, d) == -1);
    caller(1, ARK_CAP_SYSTEM);
    d.x = -1;
    CHECK(CALL(ARK_SYS_DRAG, d) == -22);
    d.x = 3;
    d.y = 4;
    CHECK(CALL(ARK_SYS_DRAG, d) == 0);
    caller(51, ARK_CAP_UI);
    s = (ArkSurfaceRequest){.op = ARK_SURFACE_NEXT_EVENT, .id = target};
    CHECK(CALL(ARK_SYS_SURFACE, s) == 1 && s.event.type == ARK_EV_DROP &&
          s.event.key == (int)token && s.event.x == 3 && s.event.y == 4);
    d = (ArkDragRequest){.op = ARK_DRAG_READ, .token = token, .target = target};
    CHECK(CALL(ARK_SYS_DRAG, d) == 0 && !strcmp(d.data, "abc"));
    caller(52, ARK_CAP_UI);
    CHECK(CALL(ARK_SYS_DRAG, d) == -1);
    caller(51, ARK_CAP_UI);
    d.op = ARK_DRAG_ACCEPT;
    d.action = ARK_DRAG_COPY;
    CHECK(CALL(ARK_SYS_DRAG, d) == 0);
    caller(50, ARK_CAP_UI);
    s = (ArkSurfaceRequest){.op = ARK_SURFACE_NEXT_EVENT, .id = source};
    CHECK(CALL(ARK_SYS_SURFACE, s) == 1 && s.event.type == ARK_EV_POINTER && !s.event.buttons);
    CHECK(CALL(ARK_SYS_SURFACE, s) == 1 && s.event.type == ARK_EV_DRAG_END);
    caller(1, ARK_CAP_SYSTEM);
    s = (ArkSurfaceRequest){.op = ARK_SURFACE_SEND_EVENT,
                            .id = target,
                            .event = {.type = ARK_EV_POINTER, .buttons = 1}};
    for (int i = 0; i < 40; i++) {
        s.event.x = i;
        CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    }
    s.event = (ArkEvent){.type = ARK_EV_KEY, .key = 'x'};
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    s.event = (ArkEvent){.type = ARK_EV_POINTER};
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    caller(51, ARK_CAP_UI);
    s.op = ARK_SURFACE_NEXT_EVENT;
    CHECK(CALL(ARK_SYS_SURFACE, s) == 1 && s.event.x == 39 && s.event.buttons == 1);
    CHECK(CALL(ARK_SYS_SURFACE, s) == 1 && s.event.type == ARK_EV_KEY);
    CHECK(CALL(ARK_SYS_SURFACE, s) == 1 && s.event.buttons == 0);
    CHECK(CALL(ARK_SYS_SURFACE, s) == 0);
    caller(50, ARK_CAP_UI);
    d = (ArkDragRequest){.op = ARK_DRAG_BEGIN,
                         .source = source,
                         .kind = ARK_DRAG_TEXT,
                         .action = ARK_DRAG_COPY,
                         .length = 3};
    strcpy(d.data, "abc");
    CHECK(CALL(ARK_SYS_DRAG, d) == 0);
    caller(1, ARK_CAP_SYSTEM);
    CHECK(account(ARK_ACCOUNT_LOCK) == 0);
    CHECK(account(ARK_ACCOUNT_UNLOCK) == 0);
    d.op = ARK_DRAG_STATUS;
    CHECK(CALL(ARK_SYS_DRAG, d) == 0 && !d.token);
    process_exit_notify(50);
    process_exit_notify(51);
    catalog_model = false;
    puts("checked multi-surface resize, drag identity/coordinates/acknowledgement/session clearing "
         "and coalesced pointer motion preserving key/release edges");
}
static void network_boundaries(void) {
    caller(20, ARK_CAP_NETWORK);
    ArkNetworkRequest n = {.op = ARK_NET_HTTP_GET};
    strcopy(n.url, "invalid://url", sizeof n.url);
    CHECK(CALL(ARK_SYS_NETWORK, n) == -22);
    caller(21, ARK_CAP_NETWORK);
    strcopy(n.url, "http://example.test/", sizeof n.url);
    CHECK(CALL(ARK_SYS_NETWORK, n) == 0);
    caller(22, ARK_CAP_NETWORK);
    CHECK(CALL(ARK_SYS_NETWORK, n) == -16);
    n.op = ARK_NET_HTTP_READ;
    n.buffer = PAYLOAD;
    n.capacity = 32;
    CHECK(CALL(ARK_SYS_NETWORK, n) == -1);
    n.op = ARK_NET_HTTP_CANCEL;
    CHECK(CALL(ARK_SYS_NETWORK, n) == -1);
    caller(21, ARK_CAP_NETWORK);
    n.op = ARK_NET_HTTP_READ;
    n.offset = UINT64_MAX;
    CHECK(CALL(ARK_SYS_NETWORK, n) == -22);
    n.offset = 0;
    n.capacity = NET_HTTP_BODY_CAP + 1;
    CHECK(CALL(ARK_SYS_NETWORK, n) == -22);
    n.capacity = 32;
    n.buffer = UINT64_MAX - 4;
    CHECK(CALL(ARK_SYS_NETWORK, n) == -14);
    n.buffer = PAYLOAD;
    CHECK(CALL(ARK_SYS_NETWORK, n) == 0);
    CHECK(n.count == sizeof(body) - 1);
    CHECK(!memcmp(arena + (PAYLOAD - BASE), body, n.count));
    process_exit_notify(21);
    caller(22, ARK_CAP_NETWORK);
    n.op = ARK_NET_HTTP_GET;
    CHECK(CALL(ARK_SYS_NETWORK, n) == 0);
    n.op = ARK_NET_HTTP_CANCEL;
    CHECK(CALL(ARK_SYS_NETWORK, n) == 0);
    caller(22, 0);
    n.op = ARK_NET_STATUS;
    CHECK(CALL(ARK_SYS_NETWORK, n) == -1);
    puts(
        "checked HTTP ownership, failed acquisition cleanup, cancellation and bounded body copies");
}
static void display_boundaries(void) {
    caller(30, ARK_CAP_UI);
    ArkPresent p = {
        .pixels = PAYLOAD, .width = 4, .height = 3, .stride = 4, .flags = ARK_PRESENT_FULL};
    CHECK(CALL(ARK_SYS_PRESENT, p) == -1);
    ArkEventRequest event = {0};
    CHECK(CALL(ARK_SYS_EVENT, event) == -1);
    ArkStorageInfo store = {0};
    CHECK(CALL(ARK_SYS_STORAGE, store) == -1);
    CHECK(dispatch(ARK_SYS_POWER, 0, 0) == -1);
    caller(1, ARK_CAP_SYSTEM);
    uint32_t frame[18];
    for (unsigned i = 0; i < 18; i++)
        frame[i] = 100 + i;
    memcpy(arena + (PAYLOAD - BASE), frame, sizeof frame);
    p.stride = 6;
    unsigned before = copies_from;
    CHECK(CALL(ARK_SYS_PRESENT, p) == 0);
    CHECK(copies_from - before == 4);
    for (unsigned y = 0; y < 3; y++)
        for (unsigned x = 0; x < 4; x++)
            CHECK(presented[y * 4 + x] == frame[y * 6 + x]);
    uint32_t snapshot[12];
    memcpy(snapshot, presented, sizeof snapshot);
    memcpy(arena + (PAYLOAD - BASE), snapshot, sizeof snapshot);
    p.stride = 4;
    before = copies_from;
    CHECK(CALL(ARK_SYS_PRESENT, p) == 0);
    CHECK(copies_from - before == 2);
    CHECK(!memcmp(presented, snapshot, sizeof snapshot));
    frame[7] = 999;
    memcpy(arena + (PAYLOAD - BASE), frame, sizeof frame);
    p.stride = 6;
    p.flags = 0;
    p.damage = (ArkRect){1, 1, 1, 1};
    CHECK(CALL(ARK_SYS_PRESENT, p) == 0);
    CHECK(presented[5] == 999);
    CHECK(last_damage.x == 1 && last_damage.y == 1 && last_damage.w == 1 && last_damage.h == 1);
    for (unsigned i = 0; i < 12; i++)
        if (i != 5)
            CHECK(presented[i] == snapshot[i]);
    /* Even tiny damage retains the full-source mapping validation contract. */
    p.pixels = BASE + ARENA_SIZE - 4;
    unsigned old_present = presents;
    CHECK(CALL(ARK_SYS_PRESENT, p) == -14);
    CHECK(presents == old_present);
    p.pixels = PAYLOAD;
    p.damage = (ArkRect){0, 0, INT32_MAX, 1};
    CHECK(CALL(ARK_SYS_PRESENT, p) == -22);
    p.damage = (ArkRect){INT32_MAX, 0, 1, 1};
    CHECK(CALL(ARK_SYS_PRESENT, p) == -22);
    p.damage = (ArkRect){0, -1, 1, 1};
    CHECK(CALL(ARK_SYS_PRESENT, p) == -22);
    p.flags = ARK_PRESENT_FULL;
    p.pixels = UINT64_MAX - 4;
    CHECK(CALL(ARK_SYS_PRESENT, p) == -14);
    p.pixels = PAYLOAD;
    p.stride = UINT32_MAX;
    CHECK(CALL(ARK_SYS_PRESENT, p) == -22);
    p.stride = 4;
    p.flags |= ARK_PRESENT_ANIMATION;
    CHECK(CALL(ARK_SYS_PRESENT, p) == 0);
    CHECK(animation);
    p.flags = ARK_PRESENT_ANIMATION;
    p.damage = (ArkRect){1, 1, 1, 1};
    CHECK(CALL(ARK_SYS_PRESENT, p) == 0);
    CHECK(animation);
    CHECK(last_damage.x == 1 && last_damage.y == 1 && last_damage.w == 1 && last_damage.h == 1);
    p.damage.w = INT32_MAX;
    old_present = presents;
    CHECK(CALL(ARK_SYS_PRESENT, p) == -22);
    CHECK(presents == old_present);
    boot.width = 1;
    boot.height = 2161;
    services_init(&boot);
    p = (ArkPresent){
        .pixels = PAYLOAD, .width = 1, .height = 2161, .stride = 1, .flags = ARK_PRESENT_FULL};
    unsigned old = presents;
    CHECK(CALL(ARK_SYS_PRESENT, p) == -22);
    CHECK(presents == old);
    boot.height = 0;
    services_init(&boot);
    p.height = 0;
    CHECK(CALL(ARK_SYS_PRESENT, p) == -22);
    boot.width = 4;
    boot.height = 3;
    services_init(&boot);
    ArkGpuRequest g = {.op = UINT32_MAX};
    old = cursor_moves;
    CHECK(CALL(ARK_SYS_GPU, g) == -22);
    CHECK(cursor_moves == old);
    g.op = ARK_GPU_CURSOR_SCALE;
    g.scale = UINT32_MAX;
    CHECK(CALL(ARK_SYS_GPU, g) == -22);
    g.op = ARK_GPU_CURSOR_SHAPE;
    g.reserved = ARK_CURSOR_COUNT;
    CHECK(CALL(ARK_SYS_GPU, g) == -22);
    g.reserved = ARK_CURSOR_GRAB;
    CHECK(CALL(ARK_SYS_GPU, g) == 0);
    caller(30, ARK_CAP_UI);
    CHECK(CALL(ARK_SYS_GPU, g) == -1);
    ArkGlassRequest glass = {.op = ARK_GLASS_QUERY};
    CHECK(CALL(ARK_SYS_COMPOSITOR, glass) == -1);
    caller(1, ARK_CAP_SYSTEM);
    CHECK(CALL(ARK_SYS_COMPOSITOR, glass) == -19);
    CHECK(dispatch(ARK_SYS_COMPOSITOR, UINT64_MAX - 4, sizeof glass) == -14);
    CHECK(dispatch(ARK_SYS_COMPOSITOR, BASE, sizeof glass - 1) == -14);
    puts("checked compositor stride/damage arithmetic, fixed-buffer dimensions and privileged "
         "device gates");
}
static void session_gates(void) {
    caller(1, ARK_CAP_SYSTEM);
    ArkSpawn spawn = {0};
    strcopy(spawn.program, "clock", sizeof spawn.program);
    spawn.flags = UINT32_MAX;
    CHECK(CALL(ARK_SYS_SPAWN, spawn) == -22);
    spawn.flags = ARK_SPAWN_NEW;
    CHECK(CALL(ARK_SYS_SPAWN, spawn) == 0);
    CHECK(spawn.pid == 42);
    ArkPermissionRequest grant = {.op = ARK_PERMISSION_SET, .app = 0, .grants = ARK_CAP_SYSTEM};
    CHECK(CALL(ARK_SYS_PERMISSION, grant) == -1);
    grant.grants = 0;
    CHECK(CALL(ARK_SYS_PERMISSION, grant) == 0);
    CHECK(updated_caps == 0);
    CHECK(CALL(ARK_SYS_SPAWN, spawn) == -1);
    grant.grants = ARK_CAP_UI;
    CHECK(CALL(ARK_SYS_PERMISSION, grant) == 0);
    CHECK(CALL(ARK_SYS_SPAWN, spawn) == 0);
    caller(2, ARK_CAP_FILES);
    CHECK(CALL(ARK_SYS_PERMISSION, grant) == -1);
    caller(2, ARK_CAP_PROCESS);
    memset(spawn.program, 'x', sizeof spawn.program);
    CHECK(CALL(ARK_SYS_SPAWN, spawn) == -22);
    strcopy(spawn.program, "unknown", sizeof spawn.program);
    CHECK(CALL(ARK_SYS_SPAWN, spawn) == -2);
    caller(1, ARK_CAP_SYSTEM);
    unsigned old = revocations, cancelled = http_cancels;
    CHECK(account(ARK_ACCOUNT_LOCK) == 0);
    CHECK(revocations == old + 1);
    CHECK(http_cancels == cancelled + 1);
    CHECK(uid == ACCOUNTS_UID_NONE);
    for (unsigned sys = 0; sys < 2; sys++) {
        caller(sys ? 1 : 2, sys ? ARK_CAP_SYSTEM
                                : ARK_CAP_FILES | ARK_CAP_UI | ARK_CAP_NETWORK | ARK_CAP_PROCESS);
        ArkFileRequest f = file_request(ARK_FILE_LIST, "/");
        CHECK(CALL(ARK_SYS_FILE, f) == -1);
        ArkSurfaceRequest s = {.op = ARK_SURFACE_CREATE, .width = 2, .height = 2};
        CHECK(CALL(ARK_SYS_SURFACE, s) == -1);
        ArkNetworkRequest n = {.op = ARK_NET_HTTP_GET};
        CHECK(CALL(ARK_SYS_NETWORK, n) == -1);
        CHECK(CALL(ARK_SYS_SPAWN, spawn) == -1);
        n.op = ARK_NET_STATUS;
        CHECK(CALL(ARK_SYS_NETWORK, n) == (sys ? 0 : -1));
    }
    caller(2, ARK_CAP_FILES);
    ArkAccountRequest a = {.op = ARK_ACCOUNT_STATUS};
    CHECK(CALL(ARK_SYS_ACCOUNT, a) == 0);
    CHECK(a.uid == 1000 && a.status == ACCOUNT_LOCKED);
    CHECK(account(ARK_ACCOUNT_UNLOCK) == -1);
    caller(1, ARK_CAP_SYSTEM);
    CHECK(account(ARK_ACCOUNT_UNLOCK) == 0);
    CHECK(uid == 1000);
    CHECK(account(ARK_ACCOUNT_LOGOUT) == 0);
    a = (ArkAccountRequest){.op = ARK_ACCOUNT_STATUS};
    CHECK(CALL(ARK_SYS_ACCOUNT, a) == 0);
    CHECK(a.uid == ACCOUNTS_UID_NONE);
    puts("checked locked-session gates, session revocation hooks and non-escalating native app "
         "spawn");
}
static void catalog_permissions(void) {
    catalog_model = true;
    caller(50, ARK_CAP_UI);
    ArkPermissionRequest q = {.op = ARK_PERMISSION_REQUEST, .app = 0, .grants = ARK_CAP_FILES};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -11);
    CHECK(q.app == 3 && !strcmp(q.name, "notes"));
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_REQUEST, .grants = ARK_CAP_SYSTEM};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -1);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_REQUEST, .grants = ARK_CAP_UI};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -1);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_REQUEST, .grants = ARK_CAP_NETWORK};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -1);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_PENDING};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -1);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_SET, .app = 3, .grants = 6};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -1);
    caller(1, ARK_CAP_SYSTEM);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_PENDING};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == 0 && q.app == 3 && q.grants == 2);
    q.op = ARK_PERMISSION_RESOLVE;
    q.grants = 8;
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -22);
    q.grants = 0;
    CHECK(CALL(ARK_SYS_PERMISSION, q) == 0);
    caller(50, 4);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_REQUEST, .grants = 2};
    for (unsigned i = 0; i < 40; i++)
        CHECK(CALL(ARK_SYS_PERMISSION, q) == -1);
    caller(1, 1);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_PENDING};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -2);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_SET, .app = 3, .grants = 6};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == 0 && model_caps[0] == 6);
    caller(50, model_caps[0]);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_REQUEST, .grants = 2};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == 0);
    ArkFileRequest file = file_request(ARK_FILE_STAT, "service.txt");
    CHECK(CALL(ARK_SYS_FILE, file) >= 0);
    caller(1, 1);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_SET, .app = 3, .grants = 4};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == 0);
    caller(50, model_caps[0]);
    file = file_request(ARK_FILE_STAT, "service.txt");
    CHECK(CALL(ARK_SYS_FILE, file) == -1);
    uid = 1001;
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_REQUEST, .grants = 2};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -1);
    caller(51, 4);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_REQUEST, .app = 0, .grants = 8};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -11 && q.app == 4);
    caller(1, 1);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_PENDING};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == 0 && q.app == 4 && q.grants == 8);
    q.op = ARK_PERMISSION_RESOLVE;
    CHECK(CALL(ARK_SYS_PERMISSION, q) == 0 && model_caps[1] == 12);
    caller(51, model_caps[1]);
    ArkNetworkRequest net = {.op = ARK_NET_HTTP_GET};
    strcopy(net.url, "http://example.test/private", sizeof net.url);
    CHECK(CALL(ARK_SYS_NETWORK, net) == 0);
    caller(1, 1);
    unsigned old = http_cancels;
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_SET, .app = 4, .grants = 4};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == 0 && http_cancels == old + 1);
    caller(51, model_caps[1]);
    net.op = ARK_NET_HTTP_STATE;
    CHECK(CALL(ARK_SYS_NETWORK, net) == -1);
    caller(52, 36);
    ArkActivityRequest act = {
        .op = ARK_ACTIVITY_SET, .app = 0, .active = 1, .kind = 1, .deadline = 600};
    strcopy(act.title, "spoofed", sizeof act.title);
    CHECK(CALL(ARK_SYS_ACTIVITY, act) == 0);
    caller(1, 1);
    act = (ArkActivityRequest){.op = ARK_ACTIVITY_GET, .app = UINT32_MAX};
    CHECK(CALL(ARK_SYS_ACTIVITY, act) == 0 && act.app == 14 && act.active &&
          !strcmp(act.title, "计时器"));
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_SET, .app = 7, .grants = 4};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == 0);
    act = (ArkActivityRequest){.op = ARK_ACTIVITY_GET, .app = UINT32_MAX};
    CHECK(CALL(ARK_SYS_ACTIVITY, act) == 0 && !act.active);
    caller(52, model_caps[2]);
    act = (ArkActivityRequest){.op = ARK_ACTIVITY_SET, .active = 1, .kind = 1, .deadline = 600};
    CHECK(CALL(ARK_SYS_ACTIVITY, act) == -1);
    act.op = ARK_ACTIVITY_GET;
    CHECK(CALL(ARK_SYS_ACTIVITY, act) == -1);
    caller(50, 4);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_REQUEST, .grants = 2};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -11);
    permissions_process_exit(50);
    caller(1, 1);
    q = (ArkPermissionRequest){.op = ARK_PERMISSION_PENDING};
    CHECK(CALL(ARK_SYS_PERMISSION, q) == -2);
    int f = vfs_find("/.system/grants-1000");
    CHECK(f >= 0);
    CHECK(vfs_write(f, "ARKP2:0400040404040424\n"));
    CHECK(permissions_caps(1, 1000) == 0 && permissions_caps(7, 1000) == 36 &&
          permissions_caps(8, 1000) == 4);
    CHECK(vfs_write(f, "ARKP1:404\n"));
    CHECK(permissions_caps(0, 1000) == 4 && permissions_caps(1, 1000) == 0 &&
          permissions_caps(2, 1000) == 4 && permissions_caps(4, 1000) == 4);
    CHECK(vfs_write(f, "ARKP2:ffffffffffffffff\n"));
    for (unsigned i = 0; i < 8; i++)
        CHECK(permissions_caps(i, 1000) == 0);
    CHECK(vfs_write(f, "ARKP2:0406060404040424\n"));
    CHECK(permissions_caps(7, 1000) == 36);
    catalog_model = false;
    puts("checked catalog identity, trusted consent, anti-spam denial, wrong UID, live "
         "FILES/NETWORK/ACTIVITY revocation, legacy migration and corrupt-record fail-closed");
}
static void device_boundaries(void) {
    device_init();
    memset(device_disks, 0, sizeof device_disks);
    device_disks[0].present = true;
    device_disks[0].sectors = 64;
    device_disks[1].present = true;
    device_disks[1].sectors = 32;
    device_reads = device_wakes = 0;
    now_ms = 1007;
    /* Unit 0 is the ArkFS system volume, unit 1 a removable disk, index 2 a
     * PCI network function: the three shapes the syscall must distinguish. */
    ArkDeviceInfo info = {0};
    info.class_id = ARK_DEV_CLASS_BLOCK;
    info.bus = ARK_BUS_ISA;
    info.flags = ARK_DEV_PRESENT | ARK_DEV_READABLE | ARK_DEV_SYSTEM_VOLUME;
    info.state = ARK_DEV_STATE_OK;
    info.blocks = 64;
    strcopy(info.name, "System volume", sizeof info.name);
    strcopy(info.driver, "ahci", sizeof info.driver);
    CHECK(device_register(&info) == 0);
    info.unit = 1;
    info.flags =
        ARK_DEV_PRESENT | ARK_DEV_READABLE | ARK_DEV_WRITABLE | ARK_DEV_REMOVABLE;
    info.blocks = 32;
    strcopy(info.name, "Removable disk", sizeof info.name);
    CHECK(device_register(&info) == 1);
    memset(&info, 0, sizeof info);
    info.class_id = ARK_DEV_CLASS_NETWORK;
    info.bus = ARK_BUS_PCI;
    info.bdf = 0x300;
    info.flags = ARK_DEV_PRESENT;
    info.state = ARK_DEV_STATE_OK;
    strcopy(info.name, "e1000", sizeof info.name);
    strcopy(info.driver, "e1000", sizeof info.driver);
    CHECK(device_register(&info) == 2);
    CHECK(device_count() == 3);

    /* SYS_DRIVER shares the same addressing rules: a bad pointer, a wrong
     * length or an unmapped caller is refused before the loader is reached. */
    ArkDriverRequest r = {.op = ARK_DRV_LIST};
    CHECK(dispatch(ARK_SYS_DRIVER, 0, sizeof r) == -14);
    CHECK(dispatch(ARK_SYS_DRIVER, REQUEST, sizeof r - 1) == -14);
    permissions[1] = 1;
    CHECK(dispatch(ARK_SYS_DRIVER, REQUEST, sizeof r) == -14);
    permissions[1] = 3;
    driver_result = -22;
    memcpy(arena + (REQUEST - BASE), &r, sizeof r);
    CHECK(dispatch(ARK_SYS_DRIVER, REQUEST, sizeof r) == driver_result);
    CHECK(last_driver_request.op == ARK_DRV_LIST);
    /* the reply is copied back even when the loader reports end-of-list */
    driver_result = -2;
    CHECK(dispatch(ARK_SYS_DRIVER, REQUEST, sizeof r) == -2);
    CHECK(last_driver_request.op == ARK_DRV_LIST);

    /* Request addressing is validated before any device logic runs. */
    caller(1, ARK_CAP_SYSTEM);
    ArkDeviceRequest d = {.op = ARK_DEV_ENUMERATE};
    CHECK(dispatch(ARK_SYS_DEVICE, 0, sizeof d) == -14);
    CHECK(dispatch(ARK_SYS_DEVICE, REQUEST, sizeof d - 1) == -14);
    memcpy(arena + (REQUEST - BASE), &d, sizeof d);
    permissions[1] = 1;
    CHECK(dispatch(ARK_SYS_DEVICE, REQUEST, sizeof d) == -14);
    permissions[1] = 3;

    /* Inventory is public metadata in an active session: enumerate is dense,
     * query is by stable index, and no capability is required for either. */
    caller(2, 0);
    CHECK(CALL(ARK_SYS_DEVICE, d) == 0);
    CHECK(d.count == 3 && d.info.class_id == ARK_DEV_CLASS_BLOCK &&
          (d.info.flags & ARK_DEV_SYSTEM_VOLUME));
    d.index = 2;
    CHECK(CALL(ARK_SYS_DEVICE, d) == 0 && d.info.class_id == ARK_DEV_CLASS_NETWORK);
    d.index = 3;
    CHECK(CALL(ARK_SYS_DEVICE, d) == -2);
    d = (ArkDeviceRequest){.op = ARK_DEV_QUERY, .index = 1};
    CHECK(CALL(ARK_SYS_DEVICE, d) == 0 && d.info.unit == 1 &&
          (d.info.flags & ARK_DEV_REMOVABLE));
    d.index = 9;
    CHECK(CALL(ARK_SYS_DEVICE, d) == -22);
    d = (ArkDeviceRequest){.op = ARK_DEV_STATS, .index = 2};
    CHECK(CALL(ARK_SYS_DEVICE, d) == 0);
    d = (ArkDeviceRequest){.op = 9};
    CHECK(CALL(ARK_SYS_DEVICE, d) == -22);

    /* READ needs the DEVICE capability and a bounded, verified destination. */
    d = (ArkDeviceRequest){.op = ARK_DEV_READ,
                           .index = 1,
                           .buffer = PAYLOAD,
                           .capacity = 512,
                           .sectors = 1};
    caller(2, ARK_CAP_FILES);
    CHECK(CALL(ARK_SYS_DEVICE, d) == -1);
    caller(2, ARK_CAP_DEVICE);
    d.index = 2;
    CHECK(CALL(ARK_SYS_DEVICE, d) == -22);           /* not a block device */
    d.index = 0;
    CHECK(CALL(ARK_SYS_DEVICE, d) == -5);            /* system volume refused */
    CHECK(device_reads == 0);
    d.index = 1;
    d.sectors = 0;
    CHECK(CALL(ARK_SYS_DEVICE, d) == -22);
    d.sectors = ARK_DEV_READ_SECTORS + 1;
    d.capacity = d.sectors * 512u;
    CHECK(CALL(ARK_SYS_DEVICE, d) == -22);           /* burst cap exceeded */
    d.sectors = 2;
    d.capacity = 512;                                /* capacity must match */
    CHECK(CALL(ARK_SYS_DEVICE, d) == -22);
    d.capacity = 2 * 512u;
    d.buffer = BASE - 512;                           /* below the user window */
    CHECK(CALL(ARK_SYS_DEVICE, d) == -22);
    d.buffer = PAYLOAD;
    d.offset = 31;                                   /* 31 + 2 > 32 sectors */
    CHECK(CALL(ARK_SYS_DEVICE, d) == -5);
    CHECK(device_reads == 0);
    d.offset = 0;
    permissions[(PAYLOAD - BASE) / PAGE] = 1;        /* read-only destination */
    CHECK(CALL(ARK_SYS_DEVICE, d) == -14);
    CHECK(device_reads == 0);
    permissions[(PAYLOAD - BASE) / PAGE] = 3;
    CHECK(CALL(ARK_SYS_DEVICE, d) == 0);
    CHECK(d.count == 2 && device_reads == 1);
    CHECK(arena[PAYLOAD - BASE] == 0x5a && arena[PAYLOAD - BASE + 1023] == 0x5a);

    /* The per-process read budget is enforced inside one 100ms window and
     * covers every attempt that reaches the driver, not only completed reads:
     * the two -5 rejections above already spent tokens. */
    d.sectors = 1;
    d.capacity = 512;
    now_ms += ARK_DEVICE_READ_WINDOW_MS;             /* fresh window */
    for (unsigned i = 0; i < ARK_DEVICE_READ_BURST; i++)
        CHECK(CALL(ARK_SYS_DEVICE, d) == 0);
    CHECK(CALL(ARK_SYS_DEVICE, d) == -16);           /* window spent */
    now_ms += ARK_DEVICE_READ_WINDOW_MS;             /* next window frees it */
    CHECK(CALL(ARK_SYS_DEVICE, d) == 0);

    /* Enumerating arms the event bridge; an inventory change wakes the
     * observer, and reading the inventory again clears the pending edge. */
    CHECK(!device_pending(2));
    unsigned wakes = device_wakes;
    CHECK(device_notify(0));
    CHECK(device_pending(2));
    CHECK(device_wakes > wakes);
    d = (ArkDeviceRequest){.op = ARK_DEV_QUERY, .index = 0};
    CHECK(CALL(ARK_SYS_DEVICE, d) == 0);
    CHECK(!device_pending(2));

    /* CONTROL: REFRESH needs the DEVICE capability, FLUSH is system-only. */
    d = (ArkDeviceRequest){.op = ARK_DEV_CONTROL, .index = 1, .control = ARK_DEVCTL_FLUSH};
    caller(2, ARK_CAP_DEVICE);
    CHECK(CALL(ARK_SYS_DEVICE, d) == -1);            /* FLUSH is system-only */
    caller(1, ARK_CAP_SYSTEM);
    CHECK(CALL(ARK_SYS_DEVICE, d) == 0);             /* writable external disk */
    d.index = 0;
    CHECK(CALL(ARK_SYS_DEVICE, d) == -5);            /* system volume is not writable */
    d.index = 2;
    CHECK(CALL(ARK_SYS_DEVICE, d) == -5);            /* network node cannot flush */
    d = (ArkDeviceRequest){.op = ARK_DEV_CONTROL, .index = 1, .control = ARK_DEVCTL_REFRESH};
    caller(2, 0);
    CHECK(CALL(ARK_SYS_DEVICE, d) == -1);            /* no DEVICE capability */
    caller(2, ARK_CAP_DEVICE);
    CHECK(CALL(ARK_SYS_DEVICE, d) == 0 && (d.flags & ARK_DEV_REMOVABLE));
    d.control = 9;
    CHECK(CALL(ARK_SYS_DEVICE, d) == -22);
    caller(1, ARK_CAP_SYSTEM);
}
int main(void) {
    memset(permissions, 3, sizeof permissions);
    vfs_init();
    CHECK(accounts_init());
    services_init(&boot);
    requests_and_accounts();
    file_boundaries();
    surface_boundaries();
    drag_and_input();
    network_boundaries();
    display_boundaries();
    device_boundaries();
    catalog_permissions();
    session_gates();
    printf("Service boundary: %u checks, %u failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
