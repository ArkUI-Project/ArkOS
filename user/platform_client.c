/* Userspace compatibility facade. Every driver operation crosses int80;
 * VFile objects here are private copies, never aliases of kernel memory. */
#include "ark.h"
#include "ark_api.h"
#include "gpu.h"
#include "storage.h"
#include "extfs.h"
#include "virtio_input.h"
static ArkSystemInfo system_info;
static ArkStorageInfo disk_info;
static GpuStats stats;
static char last_error[128];
VFile vfs_files[VFS_MAX_FILES];
static VFile external_files[EXTFS_MAX_ENTRIES];
static bool loaded[ARK_FILE_SLOTS];
static ArkFileInfo metadata[ARK_FILE_SLOTS];
static void error_copy(const char *s) {
    strcopy(last_error, s && *s ? s : "System service rejected the operation", sizeof(last_error));
}
static bool info_refresh(void) {
    return ark_info(&system_info) >= 0;
}
static bool storage_refresh(void) {
    if (ark_call(ARK_SYS_STORAGE, &disk_info, sizeof(disk_info)) < 0)
        return false;
    return true;
}
static bool file_call(ArkFileRequest *r) {
    int64_t n = ark_file(r);
    if (n < 0) {
        error_copy(r->error);
        return false;
    }
    last_error[0] = 0;
    return true;
}
VFile *vfs_entry(int i) {
    if (i < 0 || i >= (int)ARK_FILE_SLOTS)
        return 0;
    return i < VFS_MAX_FILES ? &vfs_files[i] : &external_files[i - VFS_MAX_FILES];
}
int vfs_entry_limit(void) {
    return ARK_FILE_SLOTS;
}
static void apply_info(const ArkFileInfo *m) {
    if (m->index < 0 || m->index >= (int)ARK_FILE_SLOTS)
        return;
    VFile *f = vfs_entry(m->index);
    if (!(m->flags & ARK_FILE_USED)) {
        if (f->used || loaded[m->index])
            memset(f, 0, sizeof(*f));
        loaded[m->index] = false;
        return;
    }
    if (!f->used || strcmp(f->name, m->path) || f->size != m->size) {
        loaded[m->index] = false;
        f->data[0] = 0;
    }
    f->used = true;
    f->is_dir = (m->flags & ARK_FILE_DIRECTORY) != 0;
    f->size = (size_t)m->size;
    strcopy(f->name, m->path, sizeof(f->name));
}
static bool snapshot(void) {
    ArkFileRequest r = {0};
    r.op = ARK_FILE_SNAPSHOT;
    r.buffer = (uintptr_t)metadata;
    r.capacity = ARK_FILE_SLOTS;
    if (!file_call(&r))
        return false;
    if (r.count > ARK_FILE_SLOTS) {
        error_copy("Invalid metadata count returned by system service");
        return false;
    }
    for (unsigned i = 0; i < r.count; i++)
        apply_info(&metadata[i]);
    return true;
}
static bool path_copy(char out[128], const char *name) {
    if (!name || !*name) {
        error_copy("Empty file path");
        return false;
    }
    const char *home = system_info.home[0] ? system_info.home : "/home/ark";
    strcopy(out, *name == '/' ? "/" : home, 128);
    size_t length = strlen(out);
    while (*name) {
        while (*name == '/')
            name++;
        const char *start = name;
        while (*name && *name != '/')
            name++;
        size_t n = (size_t)(name - start);
        if (!n)
            break;
        if (n == 1 && start[0] == '.')
            continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            while (length > 1 && out[length - 1] != '/')
                length--;
            if (length > 1)
                length--;
            out[length] = 0;
            continue;
        }
        size_t separator = length > 1 ? 1 : 0;
        if (length + separator + n >= 128) {
            error_copy("Path exceeds 127 UTF-8 bytes");
            return false;
        }
        if (separator)
            out[length++] = '/';
        memcpy(out + length, start, n);
        length += n;
        out[length] = 0;
    }
    return true;
}
void platform_init(uint32_t magic, uint32_t mb_addr, BootInfo *b) {
    (void)magic;
    (void)mb_addr;
    memset(b, 0, sizeof(*b));
    if (!info_refresh())
        return;
    b->width = system_info.width;
    b->height = system_info.height;
    b->pitch = system_info.width * 4;
    b->bpp = 32;
    b->red_pos = 16;
    b->red_size = 8;
    b->green_pos = 8;
    b->green_size = 8;
    b->blue_pos = 0;
    b->blue_size = 8;
    b->memory_mib = system_info.memory_mib;
}
uint64_t platform_ticks(void) {
    return ark_ticks();
}
uint64_t platform_millis(void) {
    return ark_millis();
}
void platform_idle(void) {
    ArkThreadRequest r = {0};
    r.op = ARK_THREAD_SLEEP;
    r.ticks = 1;
    if (ark_thread(&r) < 0)
        ark_yield();
}
void platform_time(int *h, int *m, int *s) {
    (void)info_refresh();
    *h = system_info.hour;
    *m = system_info.minute;
    *s = system_info.second;
}
void platform_reboot(void) {
    (void)ark_syscall6(ARK_SYS_POWER, 0, 0, 0, 0, 0, 0);
}
void platform_poweroff(void) {
    (void)ark_syscall6(ARK_SYS_POWER, 1, 0, 0, 0, 0, 0);
}
void serial_write(const char *s) {
    if (!s)
        return;
    size_t left = strlen(s);
    while (left) {
        size_t n = left > 4096 ? 4096 : left;
        (void)ark_syscall6(ARK_SYS_LOG, (uintptr_t)s, n, 0, 0, 0, 0);
        s += n;
        left -= n;
    }
}
static bool next_event(Event *out, unsigned queue) {
    ArkEventRequest r = {0};
    r.flags = queue;
    if (ark_call(ARK_SYS_EVENT, &r, sizeof(r)) <= 0)
        return false;
    out->type = r.event.type;
    out->key = r.event.key;
    out->dx = r.event.x;
    out->dy = r.event.y;
    out->buttons = (uint8_t)r.event.buttons;
    return true;
}
bool platform_next_event(Event *e) {
    return next_event(e, 0);
}
bool virtio_input_next_event(Event *e) {
    return next_event(e, 1);
}
bool virtio_input_init(void) {
    return info_refresh();
}
const char *virtio_input_name(void) {
    return system_info.input_name;
}
unsigned virtio_input_contacts(void) {
    return system_info.input_contacts;
}
bool virtio_input_pointer_ready(void) {
    return system_info.input_name[0] != 0;
}
void platform_set_relative_pointer_enabled(bool enabled) {
    (void)enabled;
}
bool gpu_init(BootInfo *b) {
    platform_init(0, 0, b);
    return system_info.gpu_accelerated != 0;
}
extern void desktop_presented(void) __attribute__((weak));
extern bool desktop_begin_present(const uint32_t *) __attribute__((weak));
extern void desktop_end_present(void) __attribute__((weak));
static void present(const uint32_t *p, unsigned stride, const GpuRect *d, bool animation) {
    ArkPresent r = {0};
    r.pixels = (uintptr_t)p;
    r.width = system_info.width;
    r.height = system_info.height;
    r.stride = stride;
    r.flags = (d ? 0 : ARK_PRESENT_FULL) | (animation ? ARK_PRESENT_ANIMATION : 0);
    if (d)
        r.damage = (ArkRect){d->x, d->y, d->w, d->h};
    bool overlay = desktop_begin_present && desktop_end_present && desktop_begin_present(p);
    int64_t result = ark_call(ARK_SYS_PRESENT, &r, sizeof(r));
    if (overlay)
        desktop_end_present();
    if (result >= 0 && desktop_presented)
        desktop_presented();
}
void gpu_present(const uint32_t *p, unsigned stride, const GpuRect *d) {
    present(p, stride, d, false);
}
void gpu_present_animation(const uint32_t *p, unsigned stride) {
    present(p, stride, 0, true);
}
void gpu_present_animation_damage(const uint32_t *p, unsigned stride, const GpuRect *d) {
    present(p, stride, d, true);
}
bool gpu_cursor_move(int x, int y, bool visible) {
    ArkGpuRequest r = {0};
    r.op = ARK_GPU_CURSOR_MOVE;
    r.x = x;
    r.y = y;
    r.visible = visible;
    return ark_call(ARK_SYS_GPU, &r, sizeof(r)) > 0;
}
void gpu_cursor_set_scale(unsigned n) {
    ArkGpuRequest r = {0};
    r.op = ARK_GPU_CURSOR_SCALE;
    r.scale = n;
    (void)ark_call(ARK_SYS_GPU, &r, sizeof(r));
}
void gpu_cursor_set_shape(unsigned n) {
    ArkGpuRequest r = {.op = ARK_GPU_CURSOR_SHAPE, .reserved = n};
    (void)ark_call(ARK_SYS_GPU, &r, sizeof r);
}
const char *gpu_backend_name(void) {
    return system_info.gpu_name;
}
const GpuStats *gpu_stats(void) {
    (void)info_refresh();
    stats.accelerated = system_info.gpu_accelerated != 0;
    stats.hardware_cursor = system_info.hardware_cursor != 0;
    stats.capabilities = system_info.gpu_capabilities;
    stats.frames = system_info.frames;
    stats.fill_commands = system_info.fill_commands;
    stats.copy_commands = system_info.copy_commands;
    stats.update_commands = system_info.update_commands;
    stats.filled_pixels = system_info.filled_pixels;
    stats.copied_pixels = system_info.copied_pixels;
    stats.uploaded_pixels = system_info.uploaded_pixels;
    stats.cursor_updates = system_info.cursor_updates;
    return &stats;
}
void vfs_init(void) {
    memset(vfs_files, 0, sizeof(vfs_files));
    memset(external_files, 0, sizeof(external_files));
    memset(loaded, 0, sizeof(loaded));
    (void)info_refresh();
    if (!snapshot())
        return;
    for (int i = 0; i < VFS_MAX_FILES; i++)
        if (vfs_files[i].used && !vfs_files[i].is_dir && vfs_files[i].size < VFS_FILE_CAP)
            (void)vfs_read(i);
}
int vfs_find(const char *name) {
    ArkFileRequest r = {0};
    r.op = ARK_FILE_STAT;
    if (!path_copy(r.path, name) || !file_call(&r))
        return -1;
    apply_info(&r.info);
    int index = r.info.index;
    if (index < 0 || index >= (int)ARK_FILE_SLOTS)
        return -1;
    VFile *f = vfs_entry(index);
    if (!f->is_dir && f->size < VFS_FILE_CAP && !loaded[index])
        (void)vfs_read(index);
    return index;
}
bool vfs_read(int index) {
    VFile *f = vfs_entry(index);
    if (!f || !f->used || f->is_dir) {
        error_copy("Invalid file handle");
        return false;
    }
    if (f->size >= VFS_FILE_CAP) {
        error_copy("File exceeds 16383-byte text view limit");
        return false;
    }
    ArkFileRequest r = {0};
    r.op = ARK_FILE_READ;
    strcopy(r.path, f->name, sizeof(r.path));
    r.buffer = (uintptr_t)f->data;
    r.capacity = VFS_FILE_CAP - 1;
    if (!file_call(&r))
        return false;
    if (r.count >= VFS_FILE_CAP) {
        error_copy("Invalid file read length");
        return false;
    }
    f->data[r.count] = 0;
    f->size = r.count;
    loaded[index] = true;
    return true;
}
static int create(const char *name, bool dir) {
    ArkFileRequest r = {0};
    r.op = dir ? ARK_FILE_MKDIR : ARK_FILE_CREATE;
    if (!path_copy(r.path, name) || !file_call(&r))
        return -1;
    apply_info(&r.info);
    return r.info.index;
}
int vfs_create(const char *name) {
    return create(name, false);
}
bool vfs_mkdir(const char *name) {
    return create(name, true) >= 0;
}
bool vfs_write(int index, const char *text) {
    VFile *f = vfs_entry(index);
    if (!f || !f->used || f->is_dir || !text) {
        error_copy("Invalid file handle");
        return false;
    }
    size_t n = 0;
    while (n < VFS_FILE_CAP && text[n])
        n++;
    if (n >= VFS_FILE_CAP) {
        error_copy("File exceeds 16383 bytes");
        return false;
    }
    ArkFileRequest r = {0};
    r.op = ARK_FILE_WRITE;
    strcopy(r.path, f->name, sizeof(r.path));
    r.buffer = (uintptr_t)text;
    r.capacity = (uint32_t)n;
    if (!file_call(&r))
        return false;
    memmove(f->data, text, n + 1);
    f->size = n;
    loaded[index] = true;
    return true;
}
bool vfs_list(const char *name) {
    ArkFileRequest r = {0};
    r.op = ARK_FILE_LIST;
    if (!path_copy(r.path, name) || !file_call(&r))
        return false;
    return snapshot();
}
static bool pair_op(unsigned op, const char *old, const char *next) {
    ArkFileRequest r = {0};
    r.op = op;
    if (!path_copy(r.path, old) || (next && !path_copy(r.other, next)) || !file_call(&r))
        return false;
    return snapshot();
}
bool vfs_remove(const char *p) {
    return pair_op(ARK_FILE_REMOVE, p, 0);
}
bool vfs_copy(const char *a, const char *b) {
    return pair_op(ARK_FILE_COPY, a, b);
}
bool vfs_rename(const char *a, const char *b) {
    return pair_op(ARK_FILE_RENAME, a, b);
}
bool vfs_sync(void) {
    ArkFileRequest r = {0};
    r.op = ARK_FILE_SYNC;
    return file_call(&r);
}
const char *vfs_error(void) {
    return last_error;
}
bool storage_init(void) {
    return storage_refresh() && disk_info.mounted;
}
bool storage_mounted(void) {
    (void)storage_refresh();
    return disk_info.mounted != 0;
}
const char *storage_status(void) {
    (void)storage_refresh();
    return disk_info.status;
}
uint64_t storage_capacity_bytes(void) {
    (void)storage_refresh();
    return disk_info.capacity_bytes;
}
uint64_t storage_used_bytes(void) {
    (void)storage_refresh();
    return disk_info.used_bytes;
}
bool storage_sync(void) {
    return vfs_sync();
}
const char *storage_error(void) {
    return last_error;
}
void extfs_init(void) {
    (void)storage_refresh();
}
const char *extfs_status(void) {
    (void)storage_refresh();
    return disk_info.external_status;
}
const char *extfs_error(void) {
    return last_error;
}
bool extfs_path(const char *p) {
    return p && !strncmp(p, "/mnt", 4) && (!p[4] || p[4] == '/');
}
bool extfs_path_writable(const char *p) {
    ArkFileRequest r = {0};
    r.op = ARK_FILE_WRITABLE;
    if (!path_copy(r.path, p))
        return false;
    int64_t n = ark_file(&r);
    if (n < 0) {
        error_copy(r.error);
        return false;
    }
    return n != 0;
}
bool extfs_volume_info(unsigned n, ExtVolumeInfo *out) {
    if (n >= 2 || !out || !storage_refresh())
        return false;
    ArkVolumeInfo *v = &disk_info.volumes[n];
    out->mounted = v->mounted != 0;
    out->read_only = v->read_only != 0;
    out->capacity_bytes = v->capacity_bytes;
    out->mountpoint = v->mountpoint;
    return true;
}
int extfs_find(const char *p) {
    int i = vfs_find(p);
    return i >= VFS_MAX_FILES ? i - VFS_MAX_FILES : -1;
}
VFile *extfs_entry(int i) {
    return i >= 0 && i < EXTFS_MAX_ENTRIES ? &external_files[i] : 0;
}
bool extfs_read(int i) {
    return vfs_read(i + VFS_MAX_FILES);
}
bool extfs_list(const char *p) {
    return vfs_list(p);
}
int extfs_create(const char *p, bool directory) {
    int i = create(p, directory);
    return i >= VFS_MAX_FILES ? i - VFS_MAX_FILES : -1;
}
bool extfs_write(int i, const char *s) {
    return vfs_write(i + VFS_MAX_FILES, s);
}
bool extfs_remove(const char *p) {
    return vfs_remove(p);
}
bool extfs_rename(const char *a, const char *b) {
    return vfs_rename(a, b);
}
bool extfs_sync(void) {
    return vfs_sync();
}
