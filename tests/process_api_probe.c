/* Runs only in the isolated services-test ISO. Uses the real public int80 ABI. */
#include "ark_api.h"
#include "package.h"
#ifndef API_CONTROLLER
#define API_CONTROLLER 0
#endif
static size_t length(const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}
static void log_text(const char *s) {
    (void)ark_syscall6(ARK_SYS_LOG, (uintptr_t)s, length(s), 0, 0, 0, 0);
}
static void fail(unsigned code) {
    char text[64] = "[api-probe] FAIL code=";
    size_t at = length(text);
    if (code >= 100)
        text[at++] = (char)('0' + code / 100);
    if (code >= 10)
        text[at++] = (char)('0' + code / 10 % 10);
    text[at++] = (char)('0' + code % 10);
    text[at++] = '\n';
    text[at] = 0;
    log_text(text);
    ark_exit((int)code);
}
static void check(int yes, unsigned code) {
    if (!yes)
        fail(code);
}
static void copy(char *d, const char *s) {
    while ((*d++ = *s++)) {
    }
}
static long control(unsigned op, uint64_t a) {
    return ark_syscall6(900 + op, a, 0, 0, 0, 0, 0);
}
#if API_CONTROLLER
static void await_exit(uint32_t pid, int expected, unsigned code) {
    for (unsigned i = 0; i < 3000; i++) {
        int64_t r = control(1, pid);
        if (r != INT64_MIN) {
            check(r == expected, code);
            return;
        }
        ark_yield();
    }
    fail(code);
}
int main(void) {
    /* Millisecond deadlines share the public monotonic clock; the old ticks
     * ABI remains 10 ms. Check limits without waiting for an oversized request. */
    uint64_t ms = ark_millis();
    check(ms / 10 <= ark_ticks() && ark_ticks() <= ms / 10 + 1, 32);
    ArkThreadRequest delay = {.op = ARK_THREAD_WAIT_MS, .ticks = 86400001};
    check(ark_thread(&delay) == -22, 33);
    check(ark_syscall6(ARK_SYS_THREAD, 0x1000000, sizeof delay, 0, 0, 0, 0) == -14, 34);
    delay = (ArkThreadRequest){.op = ARK_THREAD_WAIT_MS, .ticks = 7};
    check(ark_thread(&delay) == 0, 35);
    check(ark_millis() >= ms + 7, 36);
    delay = (ArkThreadRequest){.op = ARK_THREAD_WAIT_MSG_MS, .ticks = 86400001};
    check(ark_thread(&delay) == -22, 37);
    ms = ark_millis();
    check(ark_wait_message_ms(7) == 0 && ark_millis() >= ms + 7, 38);
    ArkMessage queued = {.peer = ark_pid(), .type = 0x57414954};
    check(ark_call(ARK_SYS_MSG_SEND, &queued, sizeof queued) == 0, 39);
    check(ark_wait_message_ms(1000) == 0, 40);
    check(ark_call(ARK_SYS_MSG_RECV, &queued, sizeof queued) == 0 && queued.type == 0x57414954, 41);
    log_text("[api-probe] HPET milliseconds, legacy ticks and bounded native wait PASS\n");
    ArkAccountRequest account = {0};
    account.op = ARK_ACCOUNT_ENROLL;
    copy(account.secret, "NativeBoundary!42");
    check(ark_account(&account) == 0, 1);
    check(account.uid == 1000, 2);
    ArkFileRequest file = {0};
    file.op = ARK_FILE_CREATE;
    copy(file.path, "/home/ark/boundary.txt");
    check(ark_file(&file) == 0, 3);
    static const char content[] = "boundary";
    file = (ArkFileRequest){0};
    file.op = ARK_FILE_WRITE;
    copy(file.path, "/home/ark/boundary.txt");
    file.buffer = (uintptr_t)content;
    file.capacity = sizeof(content) - 1;
    check(ark_file(&file) == 0, 4);
    ArkSurfaceRequest surface = {0};
    surface.op = ARK_SURFACE_CREATE;
    surface.width = surface.height = 8;
    copy(surface.title, "SYSTEM owned");
    check(ark_surface(&surface) == 0 && surface.id == 0, 5);
    static uint32_t pixels[64];
    for (unsigned i = 0; i < 64; i++)
        pixels[i] = 0xabcdef;
    surface.op = ARK_SURFACE_PRESENT;
    surface.pixels = (uintptr_t)pixels;
    surface.stride = 8;
    check(ark_surface(&surface) == 0, 6);
    int child = (int)control(0, 0);
    check(child > 0, 7);
    await_exit((uint32_t)child, 0, 8);
    ArkMessage message = {0};
    check(ark_call(ARK_SYS_MSG_RECV, &message, sizeof message) == 0, 9);
    check(message.peer == (uint32_t)child && message.peer != ark_pid() &&
              message.type == 0x53595354,
          10);
    /* Revocation kills a still-running third-party task while retaining SYSTEM. */
    child = (int)control(0, 6);
    check(child > 0, 11);
    for (unsigned i = 0; i < 3; i++)
        ark_yield();
    account = (ArkAccountRequest){0};
    account.op = ARK_ACCOUNT_LOCK;
    check(ark_account(&account) == 0, 12);
    await_exit((uint32_t)child, -1, 13);
    file = (ArkFileRequest){0};
    file.op = ARK_FILE_STAT;
    copy(file.path, "/home/ark/boundary.txt");
    check(ark_file(&file) == -1, 14);
    /* Test-only kernel spawner injects a stale app after lock: service gates must
     * independently refuse it even though production SPAWN is already blocked. */
    child = (int)control(0, 1);
    check(child > 0, 15);
    await_exit((uint32_t)child, 0, 16);
    account = (ArkAccountRequest){0};
    account.op = ARK_ACCOUNT_UNLOCK;
    copy(account.secret, "NativeBoundary!42");
    check(ark_account(&account) == 0, 17);
    surface = (ArkSurfaceRequest){0};
    surface.op = ARK_SURFACE_QUERY;
    surface.id = 0;
    check(ark_surface(&surface) == 0 && (surface.flags & ARK_SURFACE_ALIVE) &&
              surface.pid == ark_pid(),
          18);
    child = (int)control(0, 2);
    check(child > 0, 19);
    await_exit((uint32_t)child, 0, 20);
    child = (int)control(0, 3);
    check(child > 0, 21);
    await_exit((uint32_t)child, 0, 22);
    /* User requests cannot reach authentication storage even with SYSTEM cap. */
    file = (ArkFileRequest){0};
    file.op = ARK_FILE_READ;
    copy(file.path, "/home/ark/../../.system/accounts.db");
    check(ark_file(&file) == -1, 23);
    child = (int)control(0, 4);
    check(child > 0, 25);
    ArkPermissionRequest grant = {0};
    int64_t consent;
    do {
        grant = (ArkPermissionRequest){.op = ARK_PERMISSION_PENDING};
        consent = ark_call(ARK_SYS_PERMISSION, &grant, sizeof grant);
        if (consent < 0)
            ark_yield();
    } while (consent < 0);
    check(grant.app == 4 && grant.grants == ARK_CAP_NETWORK, 26);
    grant.op = ARK_PERMISSION_RESOLVE;
    check(ark_call(ARK_SYS_PERMISSION, &grant, sizeof grant) == 0, 27);
    message = (ArkMessage){0};
    while (ark_call(ARK_SYS_MSG_RECV, &message, sizeof message) < 0)
        ark_yield();
    check(message.peer == (uint32_t)child && message.type == 83, 28);
    grant = (ArkPermissionRequest){.op = ARK_PERMISSION_SET, .app = 4, .grants = ARK_CAP_UI};
    check(ark_call(ARK_SYS_PERMISSION, &grant, sizeof grant) == 0, 29);
    message = (ArkMessage){.peer = (uint32_t)child, .type = 84};
    check(ark_call(ARK_SYS_MSG_SEND, &message, sizeof message) == 0, 30);
    await_exit((uint32_t)child, 0, 31);
    log_text("[api-probe] Ring3 consent identity, grant and live NETWORK revocation PASS\n");
    check(control(2, 0) == 0, 24);
    log_text("[api-probe] SYSTEM survivor and native service isolation PASS\n");
    return 0;
}
#else
int main(void) {
    int stage = (int)control(3, 0);
    check(stage >= 0, 100);
    if (stage == 4) {
        ArkPermissionRequest q = {.op = ARK_PERMISSION_REQUEST, .app = 0, .grants = ARK_CAP_FILES};
        check(ark_call(ARK_SYS_PERMISSION, &q, sizeof q) == -1, 140);
        q = (ArkPermissionRequest){.op = ARK_PERMISSION_SET, .app = 4, .grants = 12};
        check(ark_call(ARK_SYS_PERMISSION, &q, sizeof q) == -1, 141);
        q = (ArkPermissionRequest){
            .op = ARK_PERMISSION_REQUEST, .app = 0, .grants = ARK_CAP_NETWORK};
        int64_t result = ark_call(ARK_SYS_PERMISSION, &q, sizeof q);
        check(result == -11 && q.app == 4, 142);
        do {
            q = (ArkPermissionRequest){.op = ARK_PERMISSION_REQUEST, .grants = ARK_CAP_NETWORK};
            result = ark_call(ARK_SYS_PERMISSION, &q, sizeof q);
            if (result == -11)
                ark_yield();
        } while (result == -11);
        check(result == 0, 143);
        ArkSystemInfo info = {0};
        check(ark_info(&info) == 0 && info.caps == 12, 144);
        ArkNetworkRequest net = {.op = ARK_NET_STATUS};
        check(ark_network(&net) == 0, 145);
        ArkMessage m = {.peer = 1, .type = 83};
        check(ark_call(ARK_SYS_MSG_SEND, &m, sizeof m) == 0, 146);
        while (ark_call(ARK_SYS_MSG_RECV, &m, sizeof m) < 0)
            ark_yield();
        check(m.peer == 1 && m.type == 84, 147);
        net = (ArkNetworkRequest){.op = ARK_NET_STATUS};
        check(ark_network(&net) == -1, 148);
        check(ark_info(&info) == 0 && info.caps == 4, 149);
        return 0;
    }
    if (stage == 6) {
        for (;;)
            ark_yield();
    }
    ArkSystemInfo info = {0};
    check(ark_info(&info) == 0 && !(info.caps & ARK_CAP_SYSTEM), 101);
    ArkPackageRequest pkg = {.op = ARK_PACKAGE_INSTALL};
    copy(pkg.path, "blob:untrusted.arkpkg");
    check(ark_package(&pkg) == -1, 150);
    check(ark_syscall6(ARK_SYS_PACKAGE, 0x1000000, sizeof pkg, 0, 0, 0, 0) == -14, 151);
    pkg = (ArkPackageRequest){.op = ARK_PACKAGE_GRANTS, .grants = ARK_CAP_SYSTEM};
    copy(pkg.id, "hello");
    check(ark_package(&pkg) == -1, 152);
    log_text("[api-probe] Ring3 package management, pointer and privilege denial PASS\n");
    ArkPresent present = {0};
    check(ark_call(ARK_SYS_PRESENT, &present, sizeof present) == -1, 102);
    ArkEventRequest event = {0};
    check(ark_call(ARK_SYS_EVENT, &event, sizeof event) == -1, 103);
    check(ark_syscall6(ARK_SYS_POWER, 2, 0, 0, 0, 0, 0) == -1, 104);
    ArkGpuRequest gpu = {0};
    check(ark_call(ARK_SYS_GPU, &gpu, sizeof gpu) == -1, 105);
    ArkAccountRequest account = {0};
    account.op = ARK_ACCOUNT_STATUS;
    check(ark_account(&account) == 0, 106);
    const unsigned mutations[] = {ARK_ACCOUNT_ENROLL,      ARK_ACCOUNT_LOGIN,
                                  ARK_ACCOUNT_LOGOUT,      ARK_ACCOUNT_LOCK,
                                  ARK_ACCOUNT_UNLOCK,      ARK_ACCOUNT_CHANGE_PASSWORD,
                                  ARK_ACCOUNT_CREATE_USER, ARK_ACCOUNT_SET_DISABLED};
    for (unsigned i = 0; i < sizeof mutations / sizeof mutations[0]; i++) {
        account = (ArkAccountRequest){0};
        account.op = mutations[i];
        account.uid = 0;
        account.flags = 0xffffffff;
        copy(account.user, "ark");
        copy(account.secret, "NativeBoundary!42");
        check(ark_account(&account) == -1, 107);
    }
    ArkFileRequest file = {0};
    file.op = ARK_FILE_STAT;
    copy(file.path, "/home/ark/boundary.txt");
    ArkSurfaceRequest surface = {0};
    surface.op = ARK_SURFACE_CREATE;
    surface.width = surface.height = 8;
    copy(surface.title, "Untrusted");
    if (stage == 1 || stage == 3) {
        check(ark_file(&file) == -1, 108);
        check(ark_surface(&surface) == -1, 109);
        ArkNetworkRequest net = {0};
        net.op = ARK_NET_STATUS;
        check(ark_network(&net) == -1, 110);
        log_text("[api-probe] Locked/missing-capability denial PASS\n");
        return 0;
    }
    if (stage == 2) {
        check(info.uid == 1001, 111);
        check(ark_file(&file) == -1, 112);
        log_text("[api-probe] Wrong-UID file denial PASS\n");
        return 0;
    }
    static const char *paths[] = {"/.system/accounts.db",
                                  "/.system/../.system/accounts.db",
                                  "/home/ark/../../.system/accounts.db",
                                  "../../../.system/accounts.db",
                                  "/home/other/secret.txt",
                                  "/home/ark2/boundary.txt"};
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        file = (ArkFileRequest){0};
        file.op = ARK_FILE_READ;
        copy(file.path, paths[i]);
        check(ark_file(&file) == -1, 113);
    }
    file = (ArkFileRequest){0};
    file.op = ARK_FILE_SNAPSHOT;
    check(ark_file(&file) == -1, 114);
    char data[32] = {0};
    file = (ArkFileRequest){0};
    file.op = ARK_FILE_READ;
    copy(file.path, "/home/ark/boundary.txt");
    file.buffer = (uintptr_t)data;
    file.capacity = 8;
    check(ark_file(&file) == 0 && file.count == 8, 115);
    check(data[0] == 'b' && data[7] == 'y', 116);
    file.buffer = 0x1000000;
    check(ark_file(&file) == -14, 117);
    check(ark_syscall6(ARK_SYS_FILE, 0x7fffdff8, sizeof file, 0, 0, 0, 0) == -14, 118);
    check(ark_syscall6(ARK_SYS_INFO, 0x1000000, sizeof info, 0, 0, 0, 0) == -14, 119);
    check(ark_syscall6(ARK_SYS_LOG, 0x1000000, 32, 0, 0, 0, 0) == -14, 120);
    surface = (ArkSurfaceRequest){0};
    surface.id = 0;
    surface.op = ARK_SURFACE_PRESENT;
    surface.width = surface.height = surface.stride = 8;
    surface.pid = ark_pid();
    check(ark_surface(&surface) == -1, 121);
    surface.op = ARK_SURFACE_CLOSE;
    check(ark_surface(&surface) == -1, 122);
    surface.op = ARK_SURFACE_QUERY;
    check(ark_surface(&surface) == -1, 123);
    surface.id = 7;
    check(ark_surface(&surface) == -1, 124);
    surface = (ArkSurfaceRequest){0};
    surface.op = ARK_SURFACE_CREATE;
    surface.width = surface.height = 8;
    copy(surface.title, "Untrusted own surface");
    check(ark_surface(&surface) == 0 && surface.pid == ark_pid() && surface.id != 0, 125);
    surface.op = ARK_SURFACE_PRESENT;
    surface.stride = 8;
    surface.pixels = 0x1000000;
    check(ark_surface(&surface) == -14, 126);
    static uint32_t pixels[64];
    surface.pixels = (uintptr_t)pixels;
    check(ark_surface(&surface) == 0, 127);
    ArkSpawn spawn = {0};
    copy(spawn.program, "clock");
    spawn.flags = ARK_CAP_SYSTEM;
    check(ark_spawn(&spawn) == -1, 128);
    ArkMessage msg = {0};
    msg.peer = 1;
    msg.type = 0x53595354;
    msg.length = 4;
    msg.data[0] = ARK_CAP_SYSTEM;
    check(ark_call(ARK_SYS_MSG_SEND, &msg, sizeof msg) == 0, 129);
    check(ark_call(ARK_SYS_PRESENT, &present, sizeof present) == -1, 130);
    log_text("[api-probe] Active third-party EPERM/EFAULT/ownership/IPC checks PASS\n");
    return 0;
}
#endif
