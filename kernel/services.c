/* ArkOS native capability boundary. No userspace pointer is dereferenced here. */
#define ARK_KERNEL
#include "ark_api.h"
#include "process.h"
#include "gpu.h"
#include "virtio_gpu.h"
extern int64_t virtio_gpu_glass(ArkGlassRequest *) __attribute__((weak));
#include "storage.h"
#include "block.h"
#include "extfs.h"
#include "virtio_input.h"
#include "accounts.h"
#include "net.h"
#include "installer.h"
#include "permissions.h"
#include "ark_catalog.h"
#include "package.h"
#define EINVAL (-22)
#define EFAULT (-14)
#define EPERM (-1)
#define ENOENT (-2)
#define EBUSY (-16)
extern uint64_t permissions_caps(unsigned, uint32_t);
extern int64_t permissions_request(ArkPermissionRequest *);
extern int64_t blob_request(ArkBlobRequest *);
extern int64_t registry_request(ArkRegistryRequest *);
static BootInfo display;
extern void process_wake(uint32_t) __attribute__((weak));
extern void process_kernel_free(void *, size_t) __attribute__((weak));
extern const void *process_read_alias(uint64_t, size_t) __attribute__((weak));
static uint32_t *present_pixels;
static char file_data[VFS_FILE_CAP];
static ArkFileInfo file_infos[ARK_FILE_SLOTS];
static uint32_t http_owner;
static uint64_t session_generation;
static ArkDragRequest drag_transfer;
static uint32_t drag_owner, drag_token;
static uint64_t drag_expires;
typedef struct {
    bool alive, text_input;
    ArkRect caret;
    uint32_t pid, w, h, generation;
    char title[64];
    uint32_t *pixels;
    size_t capacity;
    ArkEvent events[32];
    unsigned head, tail;
    ArkCursorRegion cursors[32];
    unsigned cursor_count;
} Surface;
static Surface surfaces[ARK_MAX_SURFACES];
extern bool vfs_path_canonical(char out[128], const char *path);
static struct {
    uint32_t pid, app;
    ArkLaunchInfo info;
} launches[PROCESS_MAX];
static struct {
    uint32_t pid;
    ArkActivityRequest info;
} activities[PROCESS_MAX];
static bool system_caller(void) {
    return process_has_cap(ARK_CAP_SYSTEM);
}
bool accounts_caller_is_system(void) {
    return system_caller();
}
static bool active(void) {
    return accounts_state() == ACCOUNT_ACTIVE;
}
static bool cap(uint64_t c) {
    return process_has_cap(c) && (system_caller() || active());
}
static uint32_t caller_uid(void) {
    return system_caller() ? accounts_current_uid() : process_current_uid();
}
static bool terminated(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!s[i])
            return true;
    return false;
}
static bool get_request(void *p, size_t size, uint64_t addr, uint64_t bytes) {
    return bytes == size && process_user_range(addr, size, true) &&
           process_copy_from_user(p, addr, size);
}
static int64_t reply(uint64_t addr, const void *p, size_t bytes, int64_t result) {
    return process_copy_to_user(addr, p, bytes) ? result : EFAULT;
}
static bool path_resolve(char out[128], const char *in, bool write) {
    if (!terminated(in, 128) || !in[0])
        return false;
    char absolute[256];
    if (in[0] == '/')
        strcopy(absolute, in, sizeof absolute);
    else {
        const char *home = accounts_current_home();
        size_t h = strlen(home), n = strlen(in);
        if (!h || h + n + 2 > sizeof absolute)
            return false;
        memcpy(absolute, home, h);
        absolute[h++] = '/';
        memcpy(absolute + h, in, n + 1);
    }
    return vfs_path_canonical(out, absolute) && accounts_path_allowed(caller_uid(), out, write);
}
static void file_info(int i, ArkFileInfo *out) {
    memset(out, 0, sizeof *out);
    out->index = i;
    VFile *f = vfs_entry(i);
    if (!f || !f->used || !accounts_path_allowed(caller_uid(), f->name, false))
        return;
    out->flags = ARK_FILE_USED | (f->is_dir ? ARK_FILE_DIRECTORY : 0) |
                 (!extfs_path_writable(f->name) && extfs_path(f->name) ? ARK_FILE_READ_ONLY : 0);
    out->size = f->size;
    strcopy(out->path, f->name, sizeof out->path);
}
static int64_t files(uint64_t addr, uint64_t size) {
    ArkFileRequest q;
    if (!get_request(&q, sizeof q, addr, size))
        return EFAULT;
    if (!cap(ARK_CAP_FILES) || !active())
        return EPERM;
    q.count = 0;
    q.error[0] = 0;
    int64_t result = 0;
    char path[128], other[128];
    int index = -1;
    bool write = q.op == ARK_FILE_WRITE || q.op == ARK_FILE_CREATE || q.op == ARK_FILE_MKDIR ||
                 q.op == ARK_FILE_REMOVE || q.op == ARK_FILE_RENAME;
    if (q.op != ARK_FILE_SNAPSHOT && q.op != ARK_FILE_SYNC && !path_resolve(path, q.path, write))
        return EPERM;
    if (q.op == ARK_FILE_SNAPSHOT) {
        if (!system_caller())
            return EPERM;
        if (q.capacity > ARK_FILE_SLOTS ||
            !process_user_range(q.buffer, (size_t)q.capacity * sizeof(ArkFileInfo), true))
            return EFAULT;
        unsigned n = (unsigned)vfs_entry_limit();
        if (n > q.capacity)
            n = q.capacity;
        for (unsigned i = 0; i < n; i++)
            file_info((int)i, &file_infos[i]);
        if (!process_copy_to_user(q.buffer, file_infos, n * sizeof(ArkFileInfo)))
            return EFAULT;
        q.count = n;
    } else if (q.op == ARK_FILE_LIST) {
        if (!vfs_list(path))
            result = ENOENT;
        else if (q.buffer && q.capacity) {
            if (q.capacity > ARK_FILE_SLOTS ||
                !process_user_range(q.buffer, q.capacity * sizeof(ArkFileInfo), true))
                return EFAULT;
            size_t plen = strlen(path);
            unsigned n = 0;
            for (int i = 0; i < vfs_entry_limit() && n < q.capacity; i++) {
                ArkFileInfo f;
                file_info(i, &f);
                if (!(f.flags & ARK_FILE_USED))
                    continue;
                const char *tail = f.path + plen;
                if (strncmp(f.path, path, plen) || (!strcmp(path, "/") ? false : *tail++ != '/') ||
                    !*tail)
                    continue;
                bool child = true;
                for (const char *p = tail; *p; p++)
                    if (*p == '/')
                        child = false;
                if (child)
                    file_infos[n++] = f;
            }
            if (!process_copy_to_user(q.buffer, file_infos, n * sizeof(ArkFileInfo)))
                return EFAULT;
            q.count = n;
        }
    } else if (q.op == ARK_FILE_SYNC) {
        result = vfs_sync() ? 0 : -5;
    } else if (q.op == ARK_FILE_WRITABLE) {
        result = accounts_path_allowed(caller_uid(), path, true) &&
                 (!extfs_path(path) || extfs_path_writable(path));
    } else if (q.op == ARK_FILE_CREATE || q.op == ARK_FILE_MKDIR) {
        index =
            q.op == ARK_FILE_CREATE ? vfs_create(path) : (vfs_mkdir(path) ? vfs_find(path) : -1);
        if (index < 0)
            result = -5;
        else
            file_info(index, &q.info);
    } else if (q.op == ARK_FILE_REMOVE) {
        result = vfs_remove(path) ? 0 : -5;
    } else if (q.op == ARK_FILE_RENAME || q.op == ARK_FILE_COPY) {
        if (!path_resolve(other, q.other, true))
            return EPERM;
        result = (q.op == ARK_FILE_COPY ? vfs_copy(path, other) : vfs_rename(path, other)) ? 0 : -5;
    } else if (q.op == ARK_FILE_READ_BYTES) {
        if (q.capacity > ARK_FILE_MAX)
            return EINVAL;
        if (!process_user_range(q.buffer, q.capacity, true))
            return EFAULT;
        index = vfs_find(path);
        if (index < 0)
            result = ENOENT;
        else {
            VFile *f = vfs_entry(index);
            size_t count = 0;
            if (!f || f->is_dir)
                result = EINVAL;
            else if (index >= VFS_MAX_FILES) {
                if (!extfs_read_bytes(index - VFS_MAX_FILES, q.offset, file_data, q.capacity,
                                      &count))
                    result = -5;
            } else if (q.offset < f->size) {
                count = f->size - (size_t)q.offset;
                if (count > q.capacity)
                    count = q.capacity;
                memcpy(file_data, f->data + (size_t)q.offset, count);
            }
            if (!result) {
                if (!process_copy_to_user(q.buffer, file_data, count))
                    result = EFAULT;
                else {
                    q.count = (uint32_t)count;
                    file_info(index, &q.info);
                }
            }
            memset(file_data, 0, sizeof file_data);
        }
    } else if (q.op == ARK_FILE_STAT || q.op == ARK_FILE_READ || q.op == ARK_FILE_WRITE) {
        index = vfs_find(path);
        if (index < 0)
            result = ENOENT;
        else if (q.op == ARK_FILE_STAT)
            file_info(index, &q.info);
        else if (q.capacity > ARK_FILE_MAX || q.offset > ARK_FILE_MAX)
            result = EINVAL;
        else if (q.op == ARK_FILE_READ) {
            if (!vfs_read(index))
                result = -5;
            else {
                VFile *f = vfs_entry(index);
                if (f->is_dir)
                    result = EINVAL;
                else {
                    size_t n = q.offset < f->size ? f->size - q.offset : 0;
                    if (n > q.capacity)
                        n = q.capacity;
                    if (!process_copy_to_user(q.buffer, f->data + q.offset, n))
                        return EFAULT;
                    q.count = (uint32_t)n;
                    file_info(index, &q.info);
                }
            }
        } else if (q.offset)
            result = EINVAL;
        else {
            if (!process_copy_from_user(file_data, q.buffer, q.capacity))
                return EFAULT;
            file_data[q.capacity] = 0;
            if (strlen(file_data) != q.capacity)
                result = EINVAL;
            else
                result = vfs_write(index, file_data) ? 0 : -5;
            memset(file_data, 0, sizeof file_data);
            if (!result)
                q.count = q.capacity;
        }
    } else
        result = EINVAL;
    if (result < 0)
        strcopy(q.error, vfs_error(), sizeof q.error);
    return reply(addr, &q, sizeof q, result);
}
static int64_t account_service(uint64_t addr, uint64_t size) {
    ArkAccountRequest q;
    if (!get_request(&q, sizeof q, addr, size))
        return EFAULT;
    if (q.op != ARK_ACCOUNT_STATUS && !system_caller())
        return EPERM;
    bool ok = true;
    AccountProfile p;
    memset(&p, 0, sizeof p);
    p.uid = ACCOUNTS_UID_NONE;
    if (!terminated(q.secret, sizeof q.secret) || !terminated(q.new_secret, sizeof q.new_secret) ||
        !terminated(q.user, sizeof q.user) || !terminated(q.display, sizeof q.display))
        return EINVAL;
    switch (q.op) {
    case ARK_ACCOUNT_STATUS:
        accounts_session_profile(&p);
        break;
    case ARK_ACCOUNT_LIST:
        ok = accounts_list(q.index, &p);
        break;
    case ARK_ACCOUNT_ENROLL:
        ok = accounts_enroll(q.secret);
        break;
    case ARK_ACCOUNT_LOGIN:
        ok = accounts_login(q.user, q.secret);
        break;
    case ARK_ACCOUNT_LOGOUT:
        ok = accounts_logout();
        break;
    case ARK_ACCOUNT_LOCK:
        ok = accounts_lock();
        break;
    case ARK_ACCOUNT_UNLOCK:
        ok = accounts_unlock(q.secret);
        break;
    case ARK_ACCOUNT_CHANGE_PASSWORD:
        ok = accounts_change_password(q.secret, q.new_secret);
        break;
    case ARK_ACCOUNT_CREATE_USER:
        ok = accounts_create_user(q.user, q.display, q.secret, (q.flags & ACCOUNT_ADMIN) != 0);
        break;
    case ARK_ACCOUNT_SET_DISABLED:
        ok = accounts_set_disabled(q.uid, (q.flags & ACCOUNT_DISABLED) != 0);
        break;
    default:
        ok = false;
        break;
    }
    memset(q.secret, 0, sizeof q.secret);
    memset(q.new_secret, 0, sizeof q.new_secret);
    q.status = accounts_state();
    q.count = accounts_count();
    if (q.op != ARK_ACCOUNT_LIST)
        accounts_session_profile(&p);
    q.uid = p.uid;
    q.flags = p.flags | (accounts_persistent() ? 0x100 : 0);
    strcopy(q.user, p.slug, sizeof q.user);
    strcopy(q.display, p.display_name, sizeof q.display);
    strcopy(q.home, p.home, sizeof q.home);
    strcopy(q.error, ok ? "" : accounts_error(), sizeof q.error);
    if (session_generation != accounts_session_generation()) {
        session_generation = accounts_session_generation();
        process_revoke_user_tasks();
        permissions_session_reset();
        net_http_cancel();
        http_owner = 0;
        memset(&drag_transfer, 0, sizeof drag_transfer);
        drag_owner = 0;
        process_set_current_uid(active() ? accounts_current_uid() : ACCOUNTS_UID_NONE);
        Event ev;
        while (platform_next_event(&ev)) {
        }
        while (virtio_input_next_event(&ev)) {
        }
    }
    return reply(addr, &q, sizeof q, ok ? 0 : EPERM);
}
static int64_t network_service(uint64_t addr, uint64_t size) {
    ArkNetworkRequest q;
    if (!get_request(&q, sizeof q, addr, size))
        return EFAULT;
    if (!cap(ARK_CAP_NETWORK) || (q.op != ARK_NET_STATUS && !active()))
        return EPERM;
    int64_t result = 0;
    if (q.op == ARK_NET_HTTP_GET) {
        if (!terminated(q.url, sizeof q.url))
            return EINVAL;
        if (http_owner && http_owner != process_current_pid())
            return EBUSY;
        if (net_http_get(q.url))
            http_owner = process_current_pid();
        else {
            http_owner = 0;
            result = EINVAL;
        }
    } else if (q.op != ARK_NET_STATUS && http_owner != process_current_pid())
        return EPERM;
    if (q.op == ARK_NET_HTTP_CANCEL) {
        net_http_cancel();
        http_owner = 0;
    } else if (q.op == ARK_NET_HTTP_READ) {
        size_t n = net_http_length();
        if (q.offset > n || q.capacity > NET_HTTP_BODY_CAP)
            return EINVAL;
        n -= q.offset;
        if (n > q.capacity)
            n = q.capacity;
        if (!process_copy_to_user(q.buffer, net_http_body() + q.offset, n))
            return EFAULT;
        q.count = (uint32_t)n;
    } else if (q.op > ARK_NET_HTTP_CANCEL)
        return EINVAL;
    const NetStatus *s = net_status();
    q.present = s->present;
    q.link = s->link;
    q.configured = s->configured;
    q.ipv4 = s->ipv4;
    q.mask = s->mask;
    q.gateway = s->gateway;
    q.dns = s->dns;
    memcpy(q.mac, s->mac, 6);
    q.rx_packets = s->rx_packets;
    q.tx_packets = s->tx_packets;
    q.dropped_packets = s->dropped_packets;
    q.state = net_http_state();
    q.http_status = net_http_status();
    q.total_size = (uint32_t)net_http_length();
    strcopy(q.message, q.state == NET_HTTP_ERROR ? net_http_error() : s->message, sizeof q.message);
    strcopy(q.content_type, net_http_content_type(), sizeof q.content_type);
    return reply(addr, &q, sizeof q, result);
}
static bool event_coalesces(ArkEvent e) {
    return e.type == ARK_EV_POINTER || e.type == ARK_EV_RESIZE || e.type == ARK_EV_THEME ||
           e.type == ARK_EV_SCROLL;
}
static bool surface_enqueue(Surface *s, ArkEvent e) {
    if (s->head != s->tail) {
        unsigned last = (s->head + 31) % 32;
        ArkEvent *old = &s->events[last];
        if (event_coalesces(e) && old->type == e.type &&
            (e.type != ARK_EV_POINTER || old->buttons == e.buttons)) {
            if (e.type == ARK_EV_SCROLL) {
                e.x += old->x;
                e.y += old->y;
            }
            *old = e;
            if (process_wake)
                process_wake(s->pid);
            return true;
        }
    }
    unsigned next = (s->head + 1) % 32;
    if (next == s->tail) {
        /* Reclaim redundant motion without losing button edges, text or CLOSE. */
        bool reclaimed = false;
        for (unsigned i = s->tail; i != s->head; i = (i + 1) % 32) {
            unsigned after = (i + 1) % 32;
            ArkEvent old = s->events[i];
            if (!event_coalesces(old))
                continue;
            if (old.type == ARK_EV_POINTER &&
                (after == s->head || s->events[after].type != ARK_EV_POINTER ||
                 old.buttons != s->events[after].buttons))
                continue;
            for (unsigned j = i; (j + 1) % 32 != s->head; j = (j + 1) % 32)
                s->events[j] = s->events[(j + 1) % 32];
            s->head = (s->head + 31) % 32;
            next = (s->head + 1) % 32;
            reclaimed = true;
            break;
        }
        if (!reclaimed)
            return false;
    }
    s->events[s->head] = e;
    s->head = next;
    if (process_wake)
        process_wake(s->pid);
    return true;
}
static int64_t surface_service(uint64_t addr, uint64_t size) {
    ArkSurfaceRequest q;
    if (!get_request(&q, sizeof q, addr, size))
        return EFAULT;
    if (!cap(ARK_CAP_UI) || !active())
        return EPERM;
    if ((q.op == ARK_SURFACE_QUERY || q.op == ARK_SURFACE_COPY || q.op == ARK_SURFACE_SEND_EVENT ||
         q.op == ARK_SURFACE_CURSOR_AT) &&
        !system_caller())
        return EPERM;
    if (q.op == ARK_SURFACE_CREATE) {
        if (!q.width || !q.height || q.width > ARK_SURFACE_MAX_W || q.height > ARK_SURFACE_MAX_H ||
            !terminated(q.title, sizeof q.title))
            return EINVAL;
        unsigned id = 0;
        for (id = 0; id < ARK_MAX_SURFACES && surfaces[id].alive; id++) {
        }
        if (id == ARK_MAX_SURFACES)
            return EBUSY;
        Surface *s = &surfaces[id];
        uint32_t *pixels = s->pixels;
        size_t capacity = s->capacity;
        size_t need = (size_t)q.width * q.height * 4;
        if (need > capacity) {
            pixels = process_kernel_alloc(need);
            if (!pixels)
                return -12;
            if (s->pixels && process_kernel_free)
                process_kernel_free(s->pixels, s->capacity);
            capacity = need;
        }
        memset(s, 0, sizeof *s);
        s->pixels = pixels;
        s->capacity = capacity;
        s->alive = true;
        s->pid = process_current_pid();
        s->w = q.width;
        s->h = q.height;
        s->generation = 1;
        strcopy(s->title, q.title, sizeof s->title);
        q.id = id;
    }
    if (q.id >= ARK_MAX_SURFACES)
        return EINVAL;
    Surface *s = &surfaces[q.id];
    if (!s->alive) {
        q.flags = 0;
        return reply(addr, &q, sizeof q, q.op == ARK_SURFACE_QUERY ? 0 : ENOENT);
    }
    bool own = s->pid == process_current_pid();
    int64_t result = 0;
    if (q.op == ARK_SURFACE_CURSOR_AT) {
        q.flags = ARK_CURSOR_ARROW;
        for (unsigned i = s->cursor_count; i > 0; i--) {
            ArkCursorRegion *r = &s->cursors[i - 1];
            if (q.event.x >= r->rect.x && q.event.y >= r->rect.y &&
                q.event.x - r->rect.x < r->rect.w && q.event.y - r->rect.y < r->rect.h) {
                q.flags = r->shape;
                break;
            }
        }
        return reply(addr, &q, sizeof q, 0);
    }
    if (q.op == ARK_SURFACE_PRESENT) {
        if (!own || q.width != s->w || q.height != s->h || q.stride < s->w ||
            q.stride > ARK_SURFACE_MAX_W)
            return EPERM;
        size_t n = ((size_t)(s->h - 1) * q.stride + s->w) * 4;
        if (!process_user_range(q.pixels, n, false))
            return EFAULT;
        if (q.stride == s->w) {
            if (!process_copy_from_user(s->pixels, q.pixels, n))
                return EFAULT;
        } else
            for (unsigned y = 0; y < s->h; y++)
                if (!process_copy_from_user(s->pixels + y * s->w,
                                            q.pixels + (uint64_t)y * q.stride * 4, s->w * 4))
                    return EFAULT;
        s->generation++;
    } else if (q.op == ARK_SURFACE_QUERY || q.op == ARK_SURFACE_COPY ||
               q.op == ARK_SURFACE_SEND_EVENT) {
        if (!system_caller())
            return EPERM;
        if (q.op == ARK_SURFACE_COPY) {
            if (q.capacity < s->w * s->h)
                return EINVAL;
            if (!process_copy_to_user(q.pixels, s->pixels, s->w * s->h * 4))
                return EFAULT;
        }
        if (q.op == ARK_SURFACE_SEND_EVENT) {
            if (!surface_enqueue(s, q.event))
                return EBUSY;
        }
    } else if (q.op == ARK_SURFACE_NEXT_EVENT) {
        if (!own)
            return EPERM;
        if (s->tail != s->head) {
            q.event = s->events[s->tail];
            s->tail = (s->tail + 1) % 32;
            result = 1;
        }
    } else if (q.op == ARK_SURFACE_INPUT) {
        if (!own)
            return EPERM;
        if (q.damage.x < 0 || q.damage.y < 0 || q.damage.x > (int)s->w || q.damage.y > (int)s->h ||
            q.damage.h < 0 || q.damage.h > 64 || q.flags & ~ARK_SURFACE_TEXT_INPUT)
            return EINVAL;
        s->text_input = (q.flags & ARK_SURFACE_TEXT_INPUT) != 0;
        s->caret = q.damage;
    } else if (q.op == ARK_SURFACE_CURSORS) {
        if (!own)
            return EPERM;
        if (q.capacity > 32)
            return EINVAL;
        ArkCursorRegion regions[32];
        if (q.capacity && !process_copy_from_user(regions, q.pixels, q.capacity * sizeof *regions))
            return EFAULT;
        for (unsigned i = 0; i < q.capacity; i++) {
            ArkCursorRegion *r = &regions[i];
            if (r->shape >= ARK_CURSOR_COUNT || r->rect.x < 0 || r->rect.y < 0 || r->rect.w <= 0 ||
                r->rect.h <= 0 || r->rect.x > (int)s->w || r->rect.y > (int)s->h ||
                r->rect.w > (int)s->w - r->rect.x || r->rect.h > (int)s->h - r->rect.y)
                return EINVAL;
        }
        memcpy(s->cursors, regions, q.capacity * sizeof *regions);
        s->cursor_count = q.capacity;
    } else if (q.op == ARK_SURFACE_RESIZE) {
        if (!own)
            return EPERM;
        if (!q.width || !q.height || q.width > ARK_SURFACE_MAX_W || q.height > ARK_SURFACE_MAX_H)
            return EINVAL;
        size_t need = (size_t)q.width * q.height * 4;
        if (need > s->capacity) {
            uint32_t *p = process_kernel_alloc(need);
            if (!p)
                return -12;
            if (process_kernel_free)
                process_kernel_free(s->pixels, s->capacity);
            s->pixels = p;
            s->capacity = need;
        }
        memset(s->pixels, 0, need);
        s->w = q.width;
        s->h = q.height;
        s->generation++;
    } else if (q.op == ARK_SURFACE_CLOSE) {
        if (!own && !system_caller())
            return EPERM;
        s->alive = false;
        memset(s->pixels, 0, s->capacity);
    } else if (q.op != ARK_SURFACE_CREATE)
        return EINVAL;
    q.width = s->w;
    q.height = s->h;
    q.pid = s->pid;
    q.generation = s->generation;
    q.flags = s->alive ? ARK_SURFACE_ALIVE | ARK_SURFACE_VISIBLE |
                             (s->text_input ? ARK_SURFACE_TEXT_INPUT : 0)
                       : 0;
    q.damage = s->caret;
    strcopy(q.title, s->title, sizeof q.title);
    return reply(addr, &q, sizeof q, result);
}
static void drag_finish(unsigned action) {
    if (drag_transfer.source < ARK_MAX_SURFACES) {
        Surface *s = &surfaces[drag_transfer.source];
        if (s->alive && s->pid == drag_owner) {
            (void)surface_enqueue(s, (ArkEvent){.type = ARK_EV_POINTER, .x = -1, .y = -1});
            (void)surface_enqueue(s, (ArkEvent){.type = ARK_EV_DRAG_END,
                                                .key = (int)drag_transfer.token,
                                                .buttons = action});
        }
    }
    memset(&drag_transfer, 0, sizeof drag_transfer);
    drag_owner = 0;
}
static int64_t drag_service(uint64_t addr, uint64_t bytes) {
    ArkDragRequest q;
    if (!get_request(&q, sizeof q, addr, bytes))
        return EFAULT;
    if (!cap(ARK_CAP_UI) || !active() ||
        (!system_caller() && process_current_uid() != accounts_current_uid()))
        return EPERM;
    if (drag_transfer.token && platform_ticks() > drag_expires)
        drag_finish(0);
    uint32_t pid = process_current_pid();
    bool system = system_caller();
    if (q.op == ARK_DRAG_BEGIN) {
        if (!system && (q.source >= ARK_MAX_SURFACES || !surfaces[q.source].alive ||
                        surfaces[q.source].pid != pid))
            return EPERM;
        if (system && q.source != UINT32_MAX)
            return EINVAL;
        if (drag_transfer.token)
            return EBUSY;
        if (q.kind != ARK_DRAG_TEXT && q.kind != ARK_DRAG_FILE)
            return EINVAL;
        if (q.action != ARK_DRAG_COPY || !q.length || q.length >= 512 || q.data[q.length] ||
            strlen(q.data) != q.length || !terminated(q.mime, 48))
            return EINVAL;
        if (q.kind == ARK_DRAG_FILE) {
            if (!cap(ARK_CAP_FILES) || q.length >= 128)
                return EPERM;
            char path[128];
            if (!path_resolve(path, q.data, false))
                return EPERM;
            if (vfs_find(path) < 0)
                return ENOENT;
            strcopy(q.data, path, 512);
            q.length = (unsigned)strlen(path);
        }
        q.token = ++drag_token;
        if (!q.token)
            q.token = ++drag_token;
        q.active = 1;
        q.target = UINT32_MAX;
        drag_owner = pid;
        drag_transfer = q;
        drag_expires = platform_ticks() + 6000;
        return reply(addr, &q, sizeof q, 0);
    }
    if (q.op == ARK_DRAG_STATUS) {
        if (!system && drag_owner != pid)
            return EPERM;
        q = drag_transfer;
        return reply(addr, &q, sizeof q, 0);
    }
    if (!drag_transfer.token || q.token != drag_transfer.token)
        return ENOENT;
    if (q.op == ARK_DRAG_CANCEL) {
        if (!system && drag_owner != pid)
            return EPERM;
        drag_finish(0);
        return 0;
    }
    if (q.op == ARK_DRAG_DELIVER) {
        if (!system || !drag_transfer.active)
            return EPERM;
        if (q.target >= ARK_MAX_SURFACES || !surfaces[q.target].alive)
            return ENOENT;
        ProcessInfo target;
        if (!process_get_info(surfaces[q.target].pid, &target) ||
            target.uid != accounts_current_uid())
            return EPERM;
        if (q.x < 0 || q.y < 0 || (unsigned)q.x >= surfaces[q.target].w ||
            (unsigned)q.y >= surfaces[q.target].h)
            return EINVAL;
        ArkEvent e = {
            .type = ARK_EV_DROP, .key = (int)q.token, .x = q.x, .y = q.y, .buttons = ARK_DRAG_COPY};
        if (!surface_enqueue(&surfaces[q.target], e))
            return EBUSY;
        drag_transfer.target = q.target;
        drag_transfer.active = 0;
        drag_expires = platform_ticks() + 500;
        return 0;
    }
    if (q.op == ARK_DRAG_READ || q.op == ARK_DRAG_ACCEPT) {
        if (q.target != drag_transfer.target || q.target >= ARK_MAX_SURFACES ||
            !surfaces[q.target].alive || surfaces[q.target].pid != pid)
            return EPERM;
        if (q.op == ARK_DRAG_READ) {
            q = drag_transfer;
            return reply(addr, &q, sizeof q, 0);
        }
        if (q.action != 0 && q.action != ARK_DRAG_COPY)
            return EINVAL;
        drag_finish(q.action);
        return 0;
    }
    return EINVAL;
}
static int64_t spawn_service(uint64_t addr, uint64_t size) {
    ArkSpawn q;
    if (!get_request(&q, sizeof q, addr, size))
        return EFAULT;
    if (!cap(ARK_CAP_PROCESS) || !active())
        return EPERM;
    if (!terminated(q.program, sizeof q.program) || !terminated(q.argument, sizeof q.argument) ||
        (q.flags & ~ARK_SPAWN_NEW))
        return EINVAL;
    unsigned app = 0;
    for (; app < ARK_CATALOG_COUNT; app++)
        if (!strcmp(q.program, ark_catalog[app].program) ||
            !strcmp(q.program, ark_catalog[app].title))
            break;
    if (app == ARK_CATALOG_COUNT) {
        int64_t result = package_spawn(&q);
        if (result >= 0 && q.argument[0])
            for (unsigned i = 0; i < ARK_MAX_SURFACES; i++)
                if (surfaces[i].alive && surfaces[i].pid == q.pid) {
                    Surface *t = &surfaces[i];
                    unsigned next = (t->head + 1) % 32;
                    if (next != t->tail) {
                        t->events[t->head] = (ArkEvent){.type = ARK_EV_OPEN};
                        t->head = next;
                        if (process_wake)
                            process_wake(q.pid);
                    }
                }
        return reply(addr, &q, sizeof q, result);
    }
    uint64_t caps = permissions_caps(app, accounts_current_uid());
    if (!(caps & ARK_CAP_UI))
        return EPERM;
    ProcessInfo info;
    int pid = 0;
    unsigned slot = PROCESS_MAX;
    for (unsigned i = 0; i < PROCESS_MAX; i++) {
        if (!launches[i].pid && slot == PROCESS_MAX)
            slot = i;
        if (!(q.flags & ARK_SPAWN_NEW) && launches[i].app == app && launches[i].pid &&
            process_get_info(launches[i].pid, &info) && info.uid == accounts_current_uid() &&
            info.state != PROCESS_DEAD) {
            pid = (int)launches[i].pid;
            slot = i;
            break;
        }
    }
    if (slot == PROCESS_MAX)
        return EBUSY;
    if (!pid) {
        size_t bytes = 0;
        const uint8_t *payload = package_system_image(app, &bytes);
        if (payload)
            pid = process_spawn_elf(payload, bytes, ark_catalog[app].program,
                                    accounts_current_uid(), caps);
    }
    if (pid > 0) {
        launches[slot].pid = (uint32_t)pid;
        launches[slot].app = app;
        if (q.argument[0] || !launches[slot].info.argument[0])
            strcopy(launches[slot].info.argument, q.argument, sizeof q.argument);
        if (q.argument[0])
            for (unsigned i = 0; i < ARK_MAX_SURFACES; i++)
                if (surfaces[i].alive && surfaces[i].pid == (uint32_t)pid) {
                    Surface *t = &surfaces[i];
                    unsigned next = (t->head + 1) % 32;
                    if (next != t->tail) {
                        t->events[t->head] = (ArkEvent){.type = ARK_EV_OPEN};
                        t->head = next;
                        if (process_wake)
                            process_wake((uint32_t)pid);
                    }
                }
    }
    q.pid = pid > 0 ? (uint32_t)pid : 0;
    strcopy(q.error, pid > 0 ? "" : "No process slot or invalid executable", sizeof q.error);
    return reply(addr, &q, sizeof q, pid > 0 ? 0 : -12);
}
static int64_t activity_service(uint64_t addr, uint64_t size) {
    ArkActivityRequest q;
    if (!get_request(&q, sizeof q, addr, size))
        return EFAULT;
    if (!active())
        return EPERM;
    if (q.op == ARK_ACTIVITY_GET) {
        if (!system_caller())
            return EPERM;
        uint32_t app = q.app;
        memset(&q, 0, sizeof q);
        for (unsigned i = 0; i < PROCESS_MAX; i++)
            if (activities[i].info.active && (app == UINT32_MAX || activities[i].info.app == app)) {
                q = activities[i].info;
                break;
            }
        return reply(addr, &q, sizeof q, 0);
    }
    if (q.op != ARK_ACTIVITY_SET || system_caller() || !process_has_cap(ARK_CAP_ACTIVITY) ||
        process_current_uid() != accounts_current_uid())
        return EPERM;
    ProcessInfo info;
    if (!process_get_info(process_current_pid(), &info))
        return EPERM;
    unsigned app = 0;
    for (; app < ARK_CATALOG_COUNT; app++)
        if (!strcmp(info.name, ark_catalog[app].program))
            break;
    if (q.active > 1 || q.kind != 1 || q.deadline > platform_ticks() + 8640000)
        return EINVAL;
    if (app == ARK_CATALOG_COUNT) {
        int slot = package_activity_identity(process_current_pid(), &q);
        if (slot < 0)
            return EPERM;
        app = ARK_CATALOG_COUNT + (unsigned)slot;
    } else {
        q.app = ark_catalog[app].desktop;
        strcopy(q.title, ark_catalog[app].label, sizeof q.title);
    }
    unsigned slot = PROCESS_MAX;
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (activities[i].pid == process_current_pid()) {
            slot = i;
            break;
        }
    if (slot == PROCESS_MAX)
        for (unsigned i = 0; i < PROCESS_MAX; i++)
            if (!activities[i].pid) {
                slot = i;
                break;
            }
    if (slot == PROCESS_MAX)
        return EBUSY;
    activities[slot].pid = process_current_pid();
    activities[slot].info = q;
    return 0;
}
void services_permissions_changed(const char *name, uint32_t uid, uint64_t caps) {
    ProcessInfo info;
    if (http_owner && !(caps & ARK_CAP_NETWORK) && process_get_info(http_owner, &info) &&
        info.uid == uid && !strcmp(info.name, name)) {
        net_http_cancel();
        http_owner = 0;
    }
    if (!(caps & ARK_CAP_ACTIVITY))
        for (unsigned i = 0; i < PROCESS_MAX; i++)
            if (activities[i].pid && process_get_info(activities[i].pid, &info) &&
                info.uid == uid && !strcmp(info.name, name))
                memset(&activities[i], 0, sizeof activities[i]);
}
void process_exit_notify(uint32_t pid) {
    if (drag_owner == pid) {
        memset(&drag_transfer, 0, sizeof drag_transfer);
        drag_owner = 0;
    }
    for (unsigned i = 0; i < ARK_MAX_SURFACES; i++)
        if (surfaces[i].alive && surfaces[i].pid == pid) {
            surfaces[i].alive = false;
            memset(surfaces[i].pixels, 0, surfaces[i].capacity);
        }
    if (http_owner == pid) {
        net_http_cancel();
        http_owner = 0;
    }
    permissions_process_exit(pid);
    for (unsigned i = 0; i < PROCESS_MAX; i++) {
        if (launches[i].pid == pid)
            memset(&launches[i], 0, sizeof launches[i]);
    }
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (activities[i].pid == pid)
            memset(&activities[i], 0, sizeof activities[i]);
    package_process_exit(pid);
}
void process_service_poll(void) {
    static uint64_t last;
    static bool polled;
    uint64_t now = platform_ticks();
    if (polled && now == last)
        return;
    polled = true;
    last = now;
    net_poll();
}
void services_init(const BootInfo *info) {
    display = *info;
    session_generation = accounts_session_generation();
    permissions_session_reset();
}
int64_t process_syscall_dispatch(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                 uint64_t e, uint64_t f) {
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    process_service_poll();
    if (nr == ARK_SYS_ACTIVITY)
        return activity_service(a, b);
    if (nr == ARK_SYS_DRAG)
        return drag_service(a, b);
    if (nr == ARK_SYS_REGISTRY) {
        ArkRegistryRequest q;
        if (!get_request(&q, sizeof q, a, b))
            return EFAULT;
        int64_t result = registry_request(&q);
        return reply(a, &q, sizeof q, result);
    }
    if (nr == ARK_SYS_LAUNCH) {
        if (!active() || process_current_uid() != accounts_current_uid())
            return EPERM;
        for (unsigned i = 0; i < PROCESS_MAX; i++)
            if (launches[i].pid == process_current_pid())
                return b == sizeof(ArkLaunchInfo)
                           ? reply(a, &launches[i].info, sizeof(ArkLaunchInfo), 0)
                           : EINVAL;
        ArkLaunchInfo info;
        int64_t result = package_launch_info(process_current_pid(), &info);
        return result >= 0 ? (b == sizeof info ? reply(a, &info, sizeof info, 0) : EINVAL) : result;
    }
    if (nr == ARK_SYS_PACKAGE) {
        ArkPackageRequest q;
        if (!get_request(&q, sizeof q, a, b))
            return EFAULT;
        int64_t result = package_request(&q);
        return reply(a, &q, sizeof q, result);
    }
    if (nr == ARK_SYS_PERMISSION) {
        ArkPermissionRequest q;
        if (!get_request(&q, sizeof q, a, b))
            return EFAULT;
        int64_t result = permissions_request(&q);
        return reply(a, &q, sizeof q, result);
    }
    if (nr == ARK_SYS_BLOB) {
        ArkBlobRequest q;
        if (!get_request(&q, sizeof q, a, b))
            return EFAULT;
        int64_t result = blob_request(&q);
        return reply(a, &q, sizeof q, result);
    }
    if (nr == ARK_SYS_INSTALL) {
        ArkInstallRequest q;
        if (!system_caller() || !active() || !accounts_current_admin())
            return EPERM;
        if (!get_request(&q, sizeof q, a, b))
            return EFAULT;
        int64_t result = installer_request(&q);
        return reply(a, &q, sizeof q, result);
    }
    if (nr == ARK_SYS_DATETIME) {
        ArkDateTime q = {0};
        q.valid =
            platform_datetime(&q.year, &q.month, &q.day, &q.hour, &q.minute, &q.second, &q.weekday);
        return b == sizeof q ? reply(a, &q, sizeof q, 0) : EINVAL;
    }
    if (nr == ARK_SYS_TICKS)
        return (int64_t)platform_ticks();
    if (nr == ARK_SYS_MILLIS)
        return (int64_t)platform_millis();
    if (nr == ARK_SYS_SPAWN)
        return spawn_service(a, b);
    if (nr == ARK_SYS_FILE)
        return files(a, b);
    if (nr == ARK_SYS_ACCOUNT)
        return account_service(a, b);
    if (nr == ARK_SYS_NETWORK)
        return network_service(a, b);
    if (nr == ARK_SYS_SURFACE)
        return surface_service(a, b);
    if (nr == ARK_SYS_INFO) {
        ArkSystemInfo q;
        memset(&q, 0, sizeof q);
        q.abi = ARK_ABI_VERSION;
        q.width = display.width;
        q.height = display.height;
        q.bpp = display.bpp;
        q.memory_mib = display.memory_mib;
        q.ticks = platform_ticks();
        platform_time(&q.hour, &q.minute, &q.second);
        q.pid = process_current_pid();
        q.uid = caller_uid();
        q.caps = (uint32_t)process_current_caps();
        strcopy(q.home, accounts_current_home(), sizeof q.home);
        strcopy(q.gpu_name, gpu_backend_name(), sizeof q.gpu_name);
        strcopy(q.input_name, virtio_input_name(), sizeof q.input_name);
        const GpuStats *s = gpu_stats();
        q.gpu_accelerated = s->accelerated || s->shader_accelerated;
        q.hardware_cursor = s->hardware_cursor;
        q.gpu_capabilities = s->capabilities | (s->shader_accelerated ? 0x80000000u : 0);
        q.frames = s->frames;
        q.fill_commands = s->fill_commands;
        q.copy_commands = s->copy_commands;
        q.update_commands = s->update_commands;
        q.filled_pixels = s->filled_pixels;
        q.copied_pixels = s->copied_pixels;
        q.uploaded_pixels = s->uploaded_pixels;
        q.cursor_updates = s->cursor_updates;
        q.input_contacts = virtio_input_contacts();
        return b == sizeof q ? reply(a, &q, sizeof q, 0) : EINVAL;
    }
    if (nr == ARK_SYS_LOG) {
        char msg[4097];
        if (b > 4096 || !process_copy_from_user(msg, a, (size_t)b))
            return EFAULT;
        msg[b] = 0;
        serial_write(msg);
        return 0;
    }
    if (!system_caller())
        return EPERM;
    if (nr == ARK_SYS_COMPOSITOR) {
        ArkGlassRequest q;
        if (!get_request(&q, sizeof q, a, b))
            return EFAULT;
        if (!virtio_gpu_glass)
            return -19;
        int64_t result = virtio_gpu_glass(&q);
        return reply(a, &q, sizeof q, result);
    }
    if (nr == ARK_SYS_EVENT) {
        ArkEventRequest q;
        if (!get_request(&q, sizeof q, a, b))
            return EFAULT;
        Event ev;
        bool found = q.flags ? virtio_input_next_event(&ev) : platform_next_event(&ev);
        if (found)
            q.event = (ArkEvent){ev.type, ev.key, ev.dx, ev.dy, ev.buttons, 0};
        return reply(a, &q, sizeof q, found ? 1 : 0);
    }
    if (nr == ARK_SYS_PRESENT) {
        ArkPresent q;
        if (!get_request(&q, sizeof q, a, b))
            return EFAULT;
        if (!q.width || !q.height || q.width > 3840 || q.height > 2160 ||
            q.width != display.width || q.height != display.height || q.stride < q.width ||
            q.stride > 3840)
            return EINVAL;
        size_t n = ((size_t)(q.height - 1) * q.stride + q.width) * 4;
        if (!process_user_range(q.pixels, n, false))
            return EFAULT;
        int x = 0, y = 0, w = (int)q.width, h = (int)q.height;
        if (!(q.flags & ARK_PRESENT_FULL)) {
            x = q.damage.x;
            y = q.damage.y;
            w = q.damage.w;
            h = q.damage.h;
            if (x < 0 || y < 0 || w < 0 || h < 0 || x > (int)q.width - w || y > (int)q.height - h)
                return EINVAL;
        } /* GPU consumes pixels synchronously and retains only its own shadow. */
        if (process_read_alias) {
            const uint32_t *p = process_read_alias(q.pixels, n);
            if (!p)
                return EFAULT;
            if (q.flags & ARK_PRESENT_ANIMATION) {
                GpuRect damage = {x, y, w, h};
                gpu_present_animation_damage(p, q.stride, &damage);
            } else {
                GpuRect damage = {x, y, w, h};
                gpu_present(p, q.stride, &damage);
            }
            return 0;
        }
        if (!present_pixels) {
            present_pixels = process_kernel_alloc((size_t)display.width * display.height * 4);
            if (!present_pixels)
                return -12;
        }
        if ((q.flags & ARK_PRESENT_FULL) && q.stride == q.width) {
            if (!process_copy_from_user(present_pixels, q.pixels, n))
                return EFAULT;
        } else
            for (int j = y; j < y + h; j++)
                if (!process_copy_from_user(present_pixels + j * q.width + x,
                                            q.pixels + ((uint64_t)j * q.stride + x) * 4,
                                            (size_t)w * 4))
                    return EFAULT;
        if (q.flags & ARK_PRESENT_ANIMATION) {
            GpuRect damage = {x, y, w, h};
            gpu_present_animation_damage(present_pixels, q.width, &damage);
        } else {
            GpuRect damage = {x, y, w, h};
            gpu_present(present_pixels, q.width, &damage);
        }
        return 0;
    }
    if (nr == ARK_SYS_GPU) {
        ArkGpuRequest q;
        if (!get_request(&q, sizeof q, a, b))
            return EFAULT;
        if (q.op == ARK_GPU_CURSOR_SCALE) {
            if (q.scale < 1 || q.scale > 3)
                return EINVAL;
            gpu_cursor_set_scale(q.scale);
            return 0;
        }
        if (q.op == ARK_GPU_CURSOR_SHAPE) {
            if (q.reserved >= ARK_CURSOR_COUNT)
                return EINVAL;
            gpu_cursor_set_shape(q.reserved);
            return 0;
        }
        if (q.op != ARK_GPU_CURSOR_MOVE)
            return EINVAL;
        return gpu_cursor_move(q.x, q.y, q.visible);
    }
    if (nr == ARK_SYS_STORAGE) {
        ArkStorageInfo q;
        memset(&q, 0, sizeof q);
        q.mounted = storage_mounted();
        q.capacity_bytes = storage_capacity_bytes();
        q.used_bytes = storage_used_bytes();
        strcopy(q.status, storage_status(), sizeof q.status);
        strcopy(q.error, storage_error(), sizeof q.error);
        strcopy(q.external_status, extfs_status(), sizeof q.external_status);
        for (unsigned i = 0; i < 2; i++) {
            ExtVolumeInfo v;
            if (extfs_volume_info(i, &v)) {
                q.volumes[i].mounted = v.mounted;
                q.volumes[i].read_only = v.read_only;
                q.volumes[i].capacity_bytes = v.capacity_bytes;
                strcopy(q.volumes[i].mountpoint, v.mountpoint, sizeof q.volumes[i].mountpoint);
            }
        }
        return b == sizeof q ? reply(a, &q, sizeof q, 0) : EINVAL;
    }
    if (nr == ARK_SYS_POWER) {
        if (a == 0)
            platform_reboot();
        else if (a == 1)
            platform_poweroff();
        return EINVAL;
    }
    return -38;
}

bool process_events_pending(uint32_t pid) {
    for (unsigned i = 0; i < ARK_MAX_SURFACES; i++)
        if (surfaces[i].alive && surfaces[i].pid == pid && surfaces[i].head != surfaces[i].tail)
            return true;
    return false;
}

void process_performance_devices(ArkPerformanceInfo *info) {
    BlockStats disk = {0};
    block_statistics(&disk);
    info->disk_read_bytes = disk.read_bytes;
    info->disk_write_bytes = disk.write_bytes;
    const NetStatus *network = net_status();
    info->network_rx_bytes = network->rx_bytes;
    info->network_tx_bytes = network->tx_bytes;
}
