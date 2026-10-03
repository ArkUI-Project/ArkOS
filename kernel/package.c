/* Native per-account ArkPkg admission and transactions. Original ArkOS code. */
#define ARK_KERNEL
#include "package.h"
#include "blob.h"
#include "storage.h"
#include "ark.h"
#include "accounts.h"
#include "extfs.h"
#include "elf.h"
#include "sha256.h"
#include "permissions.h"
#include "alloc.h"
extern bool vfs_path_canonical(char out[128], const char *path);

static uint8_t image[ARK_BLOB_MAX] __attribute__((aligned(16)));
typedef struct Package {
    struct Package *next;
    ArkPackageInfo info;
    ArkLaunchInfo launch;
} Package;
static Package *installed;
static struct {
    uint32_t pid, slot, uid;
    ArkLaunchInfo info;
} package_launches[PROCESS_MAX];
static void clear_packages(void) {
    while (installed) {
        Package *p = installed;
        installed = p->next;
        ark_free(p);
    }
}
static Package *by_slot(unsigned slot) {
    for (Package *p = installed; p; p = p->next)
        if (p->info.slot == slot)
            return p;
    return 0;
}
static Package *find(const char *id) {
    for (Package *p = installed; p; p = p->next)
        if (!strcmp(id, p->info.id))
            return p;
    return 0;
}
static uint32_t loaded_uid;
static uint32_t rd(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void wr(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
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
static bool terminated(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!s[i])
            return true;
    return false;
}
static bool id_ok(const char *s) {
    unsigned n = 0;
    while (n < 28 && s[n]) {
        unsigned c = (uint8_t)s[n++];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
            return false;
    }
    return n > 0 && n < 28 && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9')) &&
           s[n - 1] != '.';
}
static bool text_ok(const char *s, size_t n) {
    if (!terminated(s, n))
        return false;
    for (size_t i = 0; s[i]; i++)
        if ((uint8_t)s[i] < 32 || s[i] == 127)
            return false;
    return true;
}
static int64_t error(ArkPackageRequest *q, int code, const char *message) {
    strcopy(q->error, message, sizeof q->error);
    return code;
}
static bool header(const uint8_t *p, uint32_t bytes, ArkPackageInfo *info) {
    if (bytes < ARK_PACKAGE_HEADER || bytes > ARK_PACKAGE_MAX || memcmp(p, "ARKPKG1\0", 8) ||
        p[8] != 1 || p[9] || p[10] != 62 || p[11] || rd(p + 12) != ARK_PACKAGE_HEADER ||
        rd(p + 16) != bytes || rd(p + 20) != ARK_ABI_VERSION || rd(p + 24) & ~ARK_PACKAGE_CAPS ||
        !(rd(p + 24) & ARK_CAP_UI) || rd(p + 40) != bytes - ARK_PACKAGE_HEADER || rd(p + 44) ||
        rd(p + 252) != crc(p, 252) || !id_ok((const char *)p + 48) || !p[80] ||
        !text_ok((const char *)p + 80, 64) || !text_ok((const char *)p + 144, 64))
        return false;
    for (unsigned i = 240; i < 252; i++)
        if (p[i])
            return false;
    memset(info, 0, sizeof *info);
    info->slot = UINT32_MAX;
    info->bytes = bytes;
    info->major = rd(p + 28);
    info->minor = rd(p + 32);
    info->patch = rd(p + 36);
    info->maximum = rd(p + 24);
    strcopy(info->id, (const char *)p + 48, sizeof info->id);
    strcopy(info->title, (const char *)p + 80, sizeof info->title);
    strcopy(info->summary, (const char *)p + 144, sizeof info->summary);
    ark_sha256(p, ARK_PACKAGE_HEADER, info->manifest_sha256);
    return true;
}
static bool payload_valid(ArkPackageInfo *info) {
    const uint8_t *p = image + 32;
    uint8_t hash[32];
    ark_sha256(p + ARK_PACKAGE_HEADER, info->bytes - ARK_PACKAGE_HEADER, hash);
    return !memcmp(hash, p + 208, 32) &&
           process_validate_elf(p + ARK_PACKAGE_HEADER, info->bytes - ARK_PACKAGE_HEADER,
                                info->maximum);
}
static void object_name(char out[64], const char *id) {
    strcopy(out, "@pkg.", 64);
    strcopy(out + 5, id, 59);
}
static void program_name(char out[32], const char *id) {
    strcopy(out, "pkg.", 32);
    strcopy(out + 4, id, 28);
}
static bool load(void) {
    uint32_t uid = accounts_current_uid();
    if (loaded_uid == uid)
        return true;
    clear_packages();
    loaded_uid = 0;
    Package **tail = &installed;
    for (unsigned i = 0;; i++) {
        ArkBlobRequest q = {.op = ARK_BLOB_LIST, .index = i};
        int64_t result = blob_kernel_request(&q, uid);
        if (result == -2)
            break;
        if (result < 0)
            return false;
        if (strncmp(q.name, "@pkg.", 5))
            continue;
        if (q.size < 32 + ARK_PACKAGE_HEADER || q.size > ARK_BLOB_MAX)
            return false;
        q.op = ARK_BLOB_READ;
        q.buffer = (uintptr_t)image;
        q.capacity = 32 + ARK_PACKAGE_HEADER;
        if (blob_kernel_request(&q, uid) < 0 || memcmp(image, "ARKINST1", 8) ||
            rd(image + 16) != q.size - 32)
            return false;
        unsigned slot = rd(image + 8), grants = rd(image + 12);
        ArkPackageInfo info;
        if (slot > INT32_MAX - ARK_PACKAGE_PERMISSION_FIRST || by_slot(slot) ||
            !header(image + 32, q.size - 32, &info) || grants & ~info.maximum ||
            strcmp(q.name + 5, info.id) || find(info.id))
            return false;
        Package *p = ark_alloc(sizeof *p);
        if (!p)
            return false;
        info.slot = slot;
        info.grants = grants;
        info.installed = 1;
        p->info = info;
        *tail = p;
        tail = &p->next;
    }
    loaded_uid = uid;
    return true;
}
static bool running(Package *p) {
    ProcessInfo info;
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (package_launches[i].pid && package_launches[i].slot == p->info.slot &&
            package_launches[i].uid == loaded_uid) {
            if (process_get_info(package_launches[i].pid, &info) && info.state != PROCESS_DEAD &&
                info.uid == loaded_uid) {
                p->info.pid = package_launches[i].pid;
                return true;
            }
            memset(&package_launches[i], 0, sizeof package_launches[i]);
        }
    p->info.pid = 0;
    return false;
}
static bool read_installed(Package *p) {
    ArkBlobRequest q = {
        .op = ARK_BLOB_READ, .buffer = (uintptr_t)image, .capacity = p->info.bytes + 32};
    object_name(q.name, p->info.id);
    ArkPackageInfo info;
    return blob_kernel_request(&q, loaded_uid) >= 0 && q.size == q.capacity &&
           header(image + 32, q.size - 32, &info) && !strcmp(info.id, p->info.id) &&
           payload_valid(&info);
}
static bool persist(Package *p, ArkPackageInfo *info) {
    memset(image, 0, 32);
    memcpy(image, "ARKINST1", 8);
    wr(image + 8, info->slot);
    wr(image + 12, info->grants);
    wr(image + 16, info->bytes);
    ArkBlobRequest q = {
        .op = ARK_BLOB_WRITE, .buffer = (uintptr_t)image, .capacity = info->bytes + 32};
    object_name(q.name, info->id);
    if (blob_kernel_request(&q, loaded_uid) < 0)
        return false;
    info->installed = 1;
    p->info = *info;
    return true;
}
static bool newer(const ArkPackageInfo *a, const ArkPackageInfo *b) {
    if (a->major != b->major)
        return a->major > b->major;
    if (a->minor != b->minor)
        return a->minor > b->minor;
    return a->patch > b->patch;
}
static int64_t source(ArkPackageRequest *q) {
    if (!terminated(q->path, sizeof q->path) || !q->path[0])
        return error(q, -22, "Choose an .arkpkg file or blob:NAME");
    uint32_t bytes;
    if (!strncmp(q->path, "blob:", 5)) {
        if (!q->path[5] || strlen(q->path + 5) >= 64 || !strncmp(q->path + 5, "@pkg.", 5))
            return error(q, -1, "无权读取此安装文件");
        ArkBlobRequest b = {
            .op = ARK_BLOB_READ, .buffer = (uintptr_t)(image + 32), .capacity = ARK_PACKAGE_HEADER};
        strcopy(b.name, q->path + 5, sizeof b.name);
        if (blob_kernel_request(&b, loaded_uid) < 0)
            return error(q, -5, "无法读取安装文件");
        bytes = b.size;
        if (bytes < ARK_PACKAGE_HEADER || bytes > ARK_PACKAGE_MAX)
            return error(q, -22, "安装文件过大");
        b.capacity = bytes;
        if (blob_kernel_request(&b, loaded_uid) < 0)
            return error(q, -5, "安装文件损坏，请重新获取");
    } else {
        char p[128];
        if (!vfs_path_canonical(p, q->path) || !accounts_path_allowed(loaded_uid, p, false))
            return error(q, -1, "无权读取此位置的安装文件");
        int f = vfs_find(p);
        VFile *entry = vfs_entry(f);
        if (!entry || !entry->used || entry->is_dir || entry->size < ARK_PACKAGE_HEADER ||
            entry->size > ARK_PACKAGE_MAX)
            return error(q, -22, "找不到安装文件，或文件大小不受支持");
        bytes = (uint32_t)entry->size;
        if (extfs_path(p)) {
            size_t count = 0;
            if (!extfs_read_bytes(f - VFS_MAX_FILES, 0, image + 32, bytes, &count) ||
                count != bytes)
                return error(q, -5, "无法读取安装文件");
        } else
            memcpy(image + 32, entry->data, bytes);
    }
    if (!header(image + 32, bytes, &q->info) || !payload_valid(&q->info))
        return error(q, -22, "安装文件损坏或不适用于当前系统");
    return 0;
}
int64_t package_request(ArkPackageRequest *q) {
    q->error[0] = 0;
    if (!process_has_cap(ARK_CAP_SYSTEM) || accounts_state() != ACCOUNT_ACTIVE)
        return error(q, -1, "请登录后管理应用");
    unsigned system_count = package_system_count();
    if (q->op == ARK_PACKAGE_LIST && q->index < system_count)
        return package_system_info(q->index, &q->info);
    if (q->op == ARK_PACKAGE_FIND && package_system_find(q->id, &q->info))
        return 0;
    if (!storage_mounted() && (q->op == ARK_PACKAGE_LIST || q->op == ARK_PACKAGE_FIND))
        return -2;
    if (!load())
        return error(q, -5, "无法读取应用，请检查系统磁盘");
    if (q->op == ARK_PACKAGE_LIST) {
        unsigned n = q->index - system_count;
        Package *p = installed;
        while (p && n--)
            p = p->next;
        if (!p)
            return -2;
        running(p);
        q->info = p->info;
        return 0;
    }
    if (q->op == ARK_PACKAGE_INSPECT || q->op == ARK_PACKAGE_INSTALL ||
        q->op == ARK_PACKAGE_UPGRADE) {
        int64_t result = source(q);
        if (result < 0)
            return result;
        unsigned expected = 0;
        for (unsigned i = 0; i < 32; i++)
            expected |= q->expected_manifest_sha256[i];
        if (expected && memcmp(q->expected_manifest_sha256, q->info.manifest_sha256, 32))
            return error(q, -11, "安装文件发生变化，请重新读取");
        ArkPackageInfo system;
        if (package_system_find(q->info.id, &system))
            return error(q, -1, "这是系统应用，请通过系统启动镜像更新");
        Package *old = find(q->info.id);
        if (q->op == ARK_PACKAGE_INSPECT) {
            if (old) {
                q->info.slot = old->info.slot;
                q->info.installed = 1;
                q->info.grants = old->info.grants;
            }
            return 0;
        }
        if (q->op == ARK_PACKAGE_INSTALL && old)
            return error(q, -17, "应用已经安装，请选择升级");
        if (q->op == ARK_PACKAGE_UPGRADE && (!old || !newer(&q->info, &old->info)))
            return error(q, -22, "请选择同一应用的更新版本");
        if (old && running(old))
            return error(q, -16, "请先关闭应用，再升级");
        unsigned slot = 0;
        if (old)
            slot = old->info.slot;
        else
            while (by_slot(slot))
                slot++;
        Package *p = old ? old : ark_alloc(sizeof *p);
        if (!p)
            return error(q, -12, "内存不足，请稍后再试");
        q->info.slot = slot;
        q->info.grants = old ? old->info.grants & q->info.maximum : ARK_CAP_UI;
        if (!persist(p, &q->info)) {
            if (!old)
                ark_free(p);
            return error(q, -5, "无法保存安装，请检查磁盘可用空间");
        }
        if (!old) {
            Package **tail = &installed;
            while (*tail)
                tail = &(*tail)->next;
            *tail = p;
        }
        serial_write("[package] ");
        serial_write(q->op == ARK_PACKAGE_INSTALL ? "Installed " : "Upgraded ");
        serial_write(q->info.id);
        serial_write("\n");
        return 0;
    }
    if (!terminated(q->id, sizeof q->id))
        return -22;
    if (package_system_find(q->id, &q->info))
        return error(q, -1, "此应用随系统提供，请通过系统镜像管理");
    Package *p = find(q->id);
    if (!p)
        return error(q, -2, "没有找到已安装的应用");
    q->info = p->info;
    if (q->op == ARK_PACKAGE_FIND)
        return 0;
    if (q->op == ARK_PACKAGE_REMOVE) {
        if (running(p))
            return error(q, -16, "请先关闭应用，再卸载");
        ArkBlobRequest b = {.op = ARK_BLOB_REMOVE};
        object_name(b.name, q->id);
        if (blob_kernel_request(&b, loaded_uid) < 0)
            return error(q, -5, "无法保存卸载，请检查系统磁盘");
        Package **at = &installed;
        while (*at != p)
            at = &(*at)->next;
        *at = p->next;
        ark_free(p);
        q->info.installed = 0;
        serial_write("[package] Removed ");
        serial_write(q->id);
        serial_write("\n");
        return 0;
    }
    if (q->op == ARK_PACKAGE_GRANTS) {
        if (q->grants & ~q->info.maximum)
            return error(q, -1, "应用不支持所选权限");
        if (!read_installed(p))
            return error(q, -5, "应用文件损坏，请重新安装");
        q->info.grants = q->grants;
        if (!persist(p, &q->info))
            return error(q, -5, "无法保存权限，请检查磁盘");
        char name[32];
        program_name(name, p->info.id);
        process_update_caps(name, loaded_uid, q->grants);
        services_permissions_changed(name, loaded_uid, q->grants);
        return 0;
    }
    return -22;
}
int64_t package_spawn(ArkSpawn *q) {
    if (accounts_state() != ACCOUNT_ACTIVE || !process_has_cap(ARK_CAP_PROCESS) || !load())
        return -1;
    const char *id = !strncmp(q->program, "pkg.", 4) ? q->program + 4 : q->program;
    Package *p = find(id);
    if (!p)
        return -2;
    ArkPackageInfo *info = &p->info;
    if (!(info->grants & ARK_CAP_UI)) {
        strcopy(q->error, "应用的运行权限已关闭", sizeof q->error);
        return -1;
    }
    bool existing = running(p);
    unsigned launch = PROCESS_MAX;
    if (!(q->flags & ARK_SPAWN_NEW) && existing) {
        for (unsigned i = 0; i < PROCESS_MAX; i++)
            if (package_launches[i].pid == info->pid) {
                launch = i;
                break;
            }
    } else
        for (unsigned i = 0; i < PROCESS_MAX; i++)
            if (!package_launches[i].pid) {
                launch = i;
                break;
            }
    if (launch == PROCESS_MAX)
        return -16;
    if ((q->flags & ARK_SPAWN_NEW) || !existing) {
        if (!read_installed(p)) {
            strcopy(q->error, "应用文件损坏，请重新安装", sizeof q->error);
            return -5;
        }
        char name[32];
        program_name(name, info->id);
        int pid =
            process_spawn_elf(image + 32 + ARK_PACKAGE_HEADER, info->bytes - ARK_PACKAGE_HEADER,
                              name, loaded_uid, info->grants);
        if (pid <= 0) {
            strcopy(q->error, "无法启动应用，请关闭一些窗口或增加内存", sizeof q->error);
            return -12;
        }
        info->pid = (uint32_t)pid;
    }
    q->pid = info->pid;
    package_launches[launch].pid = q->pid;
    package_launches[launch].slot = info->slot;
    package_launches[launch].uid = loaded_uid;
    strcopy(package_launches[launch].info.argument, q->argument, 128);
    return 0;
}
int64_t package_launch_info(uint32_t pid, ArkLaunchInfo *out) {
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (package_launches[i].pid == pid) {
            *out = package_launches[i].info;
            return 0;
        }
    return -2;
}
void package_process_exit(uint32_t pid) {
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (package_launches[i].pid == pid)
            memset(&package_launches[i], 0, sizeof package_launches[i]);
    for (Package *p = installed; p; p = p->next)
        if (p->info.pid == pid) {
            p->info.pid = 0;
            memset(&p->launch, 0, sizeof p->launch);
        }
}
int64_t package_permissions(ArkPermissionRequest *q) {
    if (accounts_state() != ACCOUNT_ACTIVE || !load())
        return -1;
    Package *p = by_slot(q->app - ARK_PACKAGE_PERMISSION_FIRST);
    if (q->op == ARK_PERMISSION_REQUEST) {
        if (process_has_cap(ARK_CAP_SYSTEM) || process_current_uid() != loaded_uid)
            return -1;
        p = 0;
        ProcessInfo info;
        if (!process_get_info(process_current_pid(), &info))
            return -1;
        for (Package *it = installed; it; it = it->next) {
            char name[32];
            program_name(name, it->info.id);
            if (!strcmp(name, info.name)) {
                p = it;
                break;
            }
        }
        if (!p || !q->grants || (q->grants & ~p->info.grants))
            return -1;
    } else if (!process_has_cap(ARK_CAP_SYSTEM))
        return -1;
    if (!p)
        return -2;
    if (q->op == ARK_PERMISSION_SET) {
        ArkPackageRequest r = {.op = ARK_PACKAGE_GRANTS, .grants = q->grants};
        strcopy(r.id, p->info.id, sizeof r.id);
        int64_t result = package_request(&r);
        if (result < 0)
            return result;
    } else if (q->op != ARK_PERMISSION_GET && q->op != ARK_PERMISSION_REQUEST)
        return -22;
    q->app = ARK_PACKAGE_PERMISSION_FIRST + p->info.slot;
    q->maximum = p->info.maximum;
    q->grants = p->info.grants;
    program_name(q->name, p->info.id);
    return 0;
}
int package_activity_identity(uint32_t pid, ArkActivityRequest *q) {
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (package_launches[i].pid == pid) {
            Package *p = by_slot(package_launches[i].slot);
            if (!p || package_launches[i].uid != loaded_uid)
                return -1;
            q->app = ARK_PACKAGE_DESKTOP_FIRST + p->info.slot;
            strcopy(q->title, p->info.title, sizeof q->title);
            return (int)p->info.slot;
        }
    return -1;
}
#ifdef ARK_PACKAGE_HOST_TEST
void package_test_reset(void) {
    loaded_uid = 0;
    clear_packages();
}
#endif
