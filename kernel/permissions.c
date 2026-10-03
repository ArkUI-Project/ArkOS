/* Per-account catalog grants, migration and a bounded trusted consent queue. */
#define ARK_KERNEL
#include "permissions.h"
#include "ark_catalog.h"
#include "ark.h"
#include "accounts.h"
#include "process.h"
#include "storage.h"
#include "package.h"
extern void process_wake(uint32_t) __attribute__((weak));
static struct {
    uint32_t pid, uid, bits;
} pending[ARK_CATALOG_COUNT];
static uint32_t denied[ARK_CATALOG_COUNT], denied_pid[ARK_CATALOG_COUNT];
static void path(char out[128], uint32_t uid) {
    char n[24];
    uint_to_str(uid, n);
    strcopy(out, "/.system/grants-", 128);
    size_t at = strlen(out);
    strcopy(out + at, n, 128 - at);
}
static int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static void read_grants(uint32_t uid, unsigned g[ARK_CATALOG_COUNT]) {
    for (unsigned i = 0; i < ARK_CATALOG_COUNT; i++)
        g[i] = ark_catalog[i].defaults;
    char p[128];
    path(p, uid);
    int f = vfs_find(p);
    if (f < 0)
        return;
    const char *s = vfs_files[f].data;
    size_t n = strlen(s);
    if (n == 10 && !strncmp(s, "ARKP1:", 6) && s[9] == '\n') {
        for (unsigned i = 0; i < 3; i++) {
            int v = hex(s[6 + i]);
            if (v < 0 || ((unsigned)v & ~ark_catalog[i].maximum))
                goto corrupt;
            g[i] = (unsigned)v;
        }
        return;
    }
    /* ARKP2 append-only catalog: preserve all recorded grants as apps are added.
     * Known append-only formats preserve earlier application grants. */
    if ((n < 7 + 8 * 2 || n > 7 + ARK_CATALOG_COUNT * 2 || (n - 7) % 2) ||
        strncmp(s, "ARKP2:", 6) || s[n - 1] != '\n')
        goto corrupt;
    for (unsigned i = 0; i < (n - 7) / 2; i++) {
        int a = hex(s[6 + 2 * i]), b = hex(s[7 + 2 * i]);
        if (a < 0 || b < 0)
            goto corrupt;
        unsigned v = (unsigned)(a * 16 + b);
        if (v & ~ark_catalog[i].maximum)
            goto corrupt;
        g[i] = v;
    }
    return;
corrupt:
    for (unsigned i = 0; i < ARK_CATALOG_COUNT; i++)
        g[i] = 0;
}
uint64_t permissions_caps(unsigned app, uint32_t uid) {
    if (app >= ARK_CATALOG_COUNT)
        return 0;
    unsigned g[ARK_CATALOG_COUNT];
    read_grants(uid, g);
    return g[app];
}
void permissions_process_exit(uint32_t pid) {
    for (unsigned i = 0; i < ARK_CATALOG_COUNT; i++) {
        if (pending[i].pid == pid)
            memset(&pending[i], 0, sizeof pending[i]);
        if (denied_pid[i] == pid) {
            denied_pid[i] = denied[i] = 0;
        }
    }
}
void permissions_session_reset(void) {
    memset(pending, 0, sizeof pending);
    memset(denied, 0, sizeof denied);
    memset(denied_pid, 0, sizeof denied_pid);
}
static int current_app(void) {
    ProcessInfo p;
    if (!process_get_info(process_current_pid(), &p))
        return -1;
    for (unsigned i = 0; i < ARK_CATALOG_COUNT; i++)
        if (!strcmp(p.name, ark_catalog[i].program))
            return (int)i;
    return -1;
}
int64_t permissions_request(ArkPermissionRequest *q) {
    if (accounts_state() != ACCOUNT_ACTIVE)
        return -1;
    uint32_t uid = accounts_current_uid();
    bool system = process_has_cap(ARK_CAP_SYSTEM);
    if (q->app >= ARK_PACKAGE_PERMISSION_FIRST && q->op != ARK_PERMISSION_REQUEST)
        return package_permissions(q);
    if (q->op == ARK_PERMISSION_REQUEST) {
        int app = current_app();
        if (app < 0 && !system)
            return package_permissions(q);
        if (system || app < 0 || process_current_uid() != uid || !q->grants ||
            (q->grants & ~ark_catalog[app].maximum) || (q->grants & ARK_CAP_UI))
            return -1;
        unsigned g = (unsigned)permissions_caps((unsigned)app, uid), missing = q->grants & ~g;
        q->app = (unsigned)app;
        q->maximum = ark_catalog[app].maximum;
        strcopy(q->name, ark_catalog[app].program, sizeof q->name);
        if (!missing) {
            q->grants = g;
            return 0;
        }
        uint32_t pid = process_current_pid();
        if (denied_pid[app] == pid && (denied[app] & missing))
            return -1;
        if (pending[app].pid && pending[app].pid != pid)
            return -16;
        pending[app].pid = pid;
        pending[app].uid = uid;
        pending[app].bits |= missing;
        return -11;
    }
    if (!system)
        return -1;
    if (q->op == ARK_PERMISSION_PENDING) {
        for (unsigned i = 0; i < ARK_CATALOG_COUNT; i++)
            if (pending[i].pid && pending[i].uid == uid) {
                q->app = i;
                q->grants = pending[i].bits;
                q->maximum = ark_catalog[i].maximum;
                strcopy(q->name, ark_catalog[i].program, sizeof q->name);
                return 0;
            }
        return -2;
    }
    if (q->app >= ARK_CATALOG_COUNT)
        return -22;
    unsigned g[ARK_CATALOG_COUNT];
    read_grants(uid, g);
    unsigned app = q->app;
    if (q->op == ARK_PERMISSION_RESOLVE) {
        if (!pending[app].pid || pending[app].uid != uid)
            return -2;
        if (q->grants && q->grants != pending[app].bits)
            return -22;
        if (!q->grants) {
            if (process_wake)
                process_wake(pending[app].pid);
            denied_pid[app] = pending[app].pid;
            denied[app] |= pending[app].bits;
            memset(&pending[app], 0, sizeof pending[app]);
            return 0;
        }
        q->grants |= g[app];
        q->op = ARK_PERMISSION_SET;
    }
    if (q->op == ARK_PERMISSION_SET) {
        if (q->grants & ~ark_catalog[app].maximum)
            return -1;
        g[app] = q->grants;
        char p[128], s[8 + ARK_CATALOG_COUNT * 2] = "ARKP2:";
        const char *h = "0123456789abcdef";
        path(p, uid);
        for (unsigned i = 0; i < ARK_CATALOG_COUNT; i++) {
            s[6 + 2 * i] = h[g[i] >> 4];
            s[7 + 2 * i] = h[g[i] & 15];
        }
        s[6 + ARK_CATALOG_COUNT * 2] = '\n';
        s[7 + ARK_CATALOG_COUNT * 2] = 0;
        int f = vfs_find(p);
        if (f < 0)
            f = vfs_create(p);
        if (f < 0 || !vfs_write(f, s))
            return -5;
        uint32_t wake_pid = pending[app].pid;
        memset(&pending[app], 0, sizeof pending[app]);
        denied_pid[app] = denied[app] = 0;
        process_update_caps(ark_catalog[app].program, uid, g[app]);
        services_permissions_changed(ark_catalog[app].program, uid, g[app]);
        if (wake_pid && process_wake)
            process_wake(wake_pid);
        if (storage_mounted() && !storage_sync())
            return -5;
    } else if (q->op != ARK_PERMISSION_GET)
        return -22;
    q->maximum = ark_catalog[app].maximum;
    q->grants = g[app];
    strcopy(q->name, ark_catalog[app].program, sizeof q->name);
    return 0;
}
