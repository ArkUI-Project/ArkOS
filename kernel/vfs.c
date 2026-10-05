/* ArkOS native filesystem namespace. Original code; MIT license. */
#include "ark.h"
#include "storage.h"
#include "extfs.h"

#ifdef ARK_STORAGE_HOST_TEST
#define extfs_init() ((void)0)
#define extfs_path(p) false
#define extfs_path_writable(p) false
#define extfs_find(p) (-1)
#define extfs_entry(i) ((VFile *)0)
#define extfs_read(i) false
#define extfs_list(p) true
#define extfs_create(p, d) (-1)
#define extfs_write(i, t) false
#define extfs_remove(p) false
#define extfs_rename(a, b) false
#define extfs_sync() true
#define extfs_error() ""
#endif

VFile vfs_files[VFS_MAX_FILES];
extern void storage_mark_dirty(void);

typedef struct {
    bool used, is_dir;
    char name[128];
    size_t size;
} VfsLite;
static VfsLite v2_list[VFS_V2_LIST_MAX];
static unsigned v2_list_count;
static VFile v2_facade;

/* UTF-8 is kept intact in filenames; malformed and control characters fail. */
static bool valid_utf8(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint32_t c = (unsigned char)s[i++];
        unsigned more;
        uint32_t min;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f)
                return false;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            more = 1;
            min = 0x80;
            c &= 31;
        } else if (c >= 0xe0 && c <= 0xef) {
            more = 2;
            min = 0x800;
            c &= 15;
        } else if (c >= 0xf0 && c <= 0xf4) {
            more = 3;
            min = 0x10000;
            c &= 7;
        } else
            return false;
        if (more > n - i)
            return false;
        while (more--) {
            unsigned char b = (unsigned char)s[i++];
            if ((b & 0xc0) != 0x80)
                return false;
            c = (c << 6) | (b & 63);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || (c >= 0x80 && c <= 0x9f))
            return false;
    }
    return true;
}

/* Shared privately with storage.c when validating on-disk names. */
bool vfs_path_canonical_cap(char *out, size_t cap, const char *path) {
    if (!path || !path[0] || !out || cap < 2)
        return false;
    size_t name_max = cap > 128 ? 255 : cap - 1;
    size_t path_max = cap > 128 ? VFS_PATH_MAX : cap - 1;
    if (path_max >= cap)
        path_max = cap - 1;
    strcopy(out, path[0] == '/' ? "/" : "/home/ark", cap);
    size_t length = strlen(out);
    const char *p = path;
    while (*p) {
        while (*p == '/')
            ++p;
        const char *start = p;
        size_t n = 0;
        while (*p && *p != '/') {
            if (++n > name_max)
                return false;
            ++p;
        }
        if (!n)
            break;
        if (!valid_utf8(start, n))
            return false;
        if (n == 1 && start[0] == '.')
            continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            while (length > 1 && out[length - 1] != '/')
                --length;
            if (length > 1)
                --length;
            out[length] = 0;
            continue;
        }
        size_t separator = length > 1 ? 1 : 0;
        if (length + separator + n > path_max)
            return false;
        if (separator)
            out[length++] = '/';
        memcpy(out + length, start, n);
        length += n;
        out[length] = 0;
    }
    return true;
}
bool vfs_path_canonical(char out[128], const char *path) {
    return vfs_path_canonical_cap(out, 128, path);
}

static int find_canonical(const char *name);

static bool canon_native(char *out, size_t cap, const char *path) {
    if (storage_is_v2())
        return vfs_path_canonical_cap(out, cap, path);
    if (cap < 128)
        return false;
    return vfs_path_canonical(out, path);
}

static int find_v2_list(const char *name) {
    for (unsigned i = 0; i < v2_list_count; ++i)
        if (v2_list[i].used && !strcmp(v2_list[i].name, name))
            return (int)(VFS_MAX_FILES + i);
    return -1;
}

static int install_visible(const char *path, bool directory, size_t size) {
    if (strlen(path) >= 128)
        return -1;
    int existing = find_canonical(path);
    if (existing >= 0) {
        vfs_files[existing].is_dir = directory;
        if (!directory && size < VFS_FILE_CAP)
            vfs_files[existing].size = size;
        return existing;
    }
    int listed = find_v2_list(path);
    if (listed >= 0) {
        VfsLite *lite = &v2_list[listed - VFS_MAX_FILES];
        lite->is_dir = directory;
        lite->size = size;
        return listed;
    }
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (vfs_files[i].used)
            continue;
        memset(&vfs_files[i], 0, sizeof(vfs_files[i]));
        strcopy(vfs_files[i].name, path, sizeof(vfs_files[i].name));
        vfs_files[i].is_dir = directory;
        vfs_files[i].used = true;
        vfs_files[i].size = directory ? 0 : size;
        return i;
    }
    if (v2_list_count < VFS_V2_LIST_MAX) {
        VfsLite *lite = &v2_list[v2_list_count];
        memset(lite, 0, sizeof(*lite));
        lite->used = true;
        lite->is_dir = directory;
        lite->size = size;
        strcopy(lite->name, path, sizeof(lite->name));
        return (int)(VFS_MAX_FILES + v2_list_count++);
    }
    for (unsigned i = 0; i < VFS_V2_LIST_MAX; ++i) {
        if (v2_list[i].used)
            continue;
        memset(&v2_list[i], 0, sizeof(v2_list[i]));
        v2_list[i].used = true;
        v2_list[i].is_dir = directory;
        v2_list[i].size = size;
        strcopy(v2_list[i].name, path, sizeof(v2_list[i].name));
        if (i >= v2_list_count)
            v2_list_count = i + 1;
        return (int)(VFS_MAX_FILES + i);
    }
    return -1;
}

static int find_canonical(const char *name) {
    for (int i = 0; i < VFS_MAX_FILES; ++i)
        if (vfs_files[i].used && !strcmp(vfs_files[i].name, name))
            return i;
    return -1;
}

int vfs_find(const char *name) {
    char path[1024];
    if (!canon_native(path, sizeof path, name))
        return -1;
    if (strlen(path) < 128 && extfs_path(path)) {
        int slot = extfs_find(path);
        return slot < 0 ? -1 : VFS_EXT_BASE + slot;
    }
    int hit = find_canonical(path);
    if (hit >= 0)
        return hit;
    hit = find_v2_list(path);
    if (hit >= 0)
        return hit;
    if (storage_is_v2()) {
        uint64_t size = 0;
        uint32_t type = 0;
        if (!storage_v2_lookup(path, &size, &type) || (type != 1 && type != 2))
            return -1;
        return install_visible(path, type == 2, (size_t)size);
    }
    return -1;
}

static bool parent_exists(const char *path) {
    if (!strcmp(path, "/"))
        return true;
    char parent[1024];
    strcopy(parent, path, sizeof(parent));
    size_t n = strlen(parent);
    while (n > 1 && parent[n - 1] != '/')
        --n;
    if (n > 1)
        --n;
    parent[n] = 0;
    int index = find_canonical(parent);
    if (index >= 0 && vfs_files[index].is_dir)
        return true;
    if (find_v2_list(parent) >= 0)
        return true;
    if (storage_is_v2()) {
        uint64_t size = 0;
        uint32_t type = 0;
        return storage_v2_lookup(parent, &size, &type) && type == 2;
    }
    return false;
}

static int create_entry(const char *name, bool directory) {
    char path[1024];
    if (!canon_native(path, sizeof path, name))
        return -1;
    if (strlen(path) < 128 && extfs_path(path)) {
        int slot = extfs_create(path, directory);
        return slot < 0 ? -1 : VFS_EXT_BASE + slot;
    }
    if (!directory && !strcmp(path, "/"))
        return -1;
    int existing = find_canonical(path);
    if (existing >= 0)
        return vfs_files[existing].is_dir == directory ? existing : -1;
    existing = find_v2_list(path);
    if (existing >= 0)
        return v2_list[existing - VFS_MAX_FILES].is_dir == directory ? existing : -1;
    if (!parent_exists(path))
        return -1;
    if (storage_is_v2()) {
        uint64_t size = 0;
        uint32_t type = 0;
        if (storage_v2_lookup(path, &size, &type)) {
            if ((type == 2) != directory)
                return -1;
            return install_visible(path, directory, (size_t)size);
        }
        if (directory) {
            if (!storage_v2_mkdir(path))
                return -1;
        } else if (!storage_v2_write(path, "", 0))
            return -1;
        int visible = install_visible(path, directory, 0);
        if (visible >= 0)
            return visible;
        if (strlen(path) >= 128)
            return -1;
        VfsLite *lite = &v2_list[VFS_V2_LIST_MAX - 1];
        memset(lite, 0, sizeof(*lite));
        lite->used = true;
        lite->is_dir = directory;
        strcopy(lite->name, path, sizeof(lite->name));
        if (v2_list_count < VFS_V2_LIST_MAX)
            v2_list_count = VFS_V2_LIST_MAX;
        return VFS_MAX_FILES + VFS_V2_LIST_MAX - 1;
    }
    int slot = -1;
    for (int i = 0; i < VFS_MAX_FILES; ++i)
        if (!vfs_files[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return -1;
    memset(&vfs_files[slot], 0, sizeof(vfs_files[slot]));
    strcopy(vfs_files[slot].name, path, sizeof(vfs_files[slot].name));
    vfs_files[slot].is_dir = directory;
    vfs_files[slot].used = true;
    storage_mark_dirty();
    return slot;
}

int vfs_create(const char *name) {
    return create_entry(name, false);
}
bool vfs_mkdir(const char *path) {
    if (create_entry(path, true) >= 0)
        return true;
    if (!storage_is_v2())
        return false;
    char canon[1024];
    uint64_t size = 0;
    uint32_t type = 0;
    if (!canon_native(canon, sizeof canon, path)) {
        (void)storage_v2_mkdir(path);
        return false;
    }
    return storage_v2_lookup(canon, &size, &type) && type == 2;
}

bool vfs_write(int index, const char *text) {
    if (index >= VFS_EXT_BASE)
        return extfs_write(index - VFS_EXT_BASE, text);
    if (!text)
        return false;
    if (index >= VFS_MAX_FILES && index < VFS_EXT_BASE) {
        VfsLite *lite = &v2_list[index - VFS_MAX_FILES];
        if (!lite->used || lite->is_dir || !storage_is_v2())
            return false;
        size_t length = 0;
        while (text[length])
            ++length;
        if (!storage_v2_write(lite->name, text, length))
            return false;
        lite->size = length;
        return true;
    }
    if (index < 0 || index >= VFS_MAX_FILES || !vfs_files[index].used || vfs_files[index].is_dir)
        return false;
    if (storage_is_v2()) {
        size_t length = 0;
        while (text[length])
            ++length;
        if (!storage_v2_write(vfs_files[index].name, text, length))
            return false;
        if (length < VFS_FILE_CAP) {
            memmove(vfs_files[index].data, text, length);
            vfs_files[index].data[length] = '\0';
            vfs_files[index].size = length;
        }
        return true;
    }
    size_t length = 0;
    while (length < VFS_FILE_CAP && text[length])
        ++length;
    if (length >= VFS_FILE_CAP)
        return false;
    /* Supports saving from a view into a file's existing contents. */
    memmove(vfs_files[index].data, text, length);
    vfs_files[index].data[length] = '\0';
    vfs_files[index].size = length;
    storage_mark_dirty();
    return true;
}

static bool below(const char *path, const char *directory) {
    size_t n = strlen(directory);
    return !strncmp(path, directory, n) && path[n] == '/';
}

bool vfs_remove(const char *name) {
    char path[1024];
    if (!canon_native(path, sizeof path, name) || !strcmp(path, "/"))
        return false;
    if (strlen(path) < 128 && extfs_path(path))
        return extfs_remove(path);
    int index = find_canonical(path);
    int listed = find_v2_list(path);
    if (index < 0 && listed < 0 && !storage_is_v2())
        return false;
    if (index >= 0 && vfs_files[index].is_dir) {
        for (int i = 0; i < VFS_MAX_FILES; ++i)
            if (vfs_files[i].used && below(vfs_files[i].name, path))
                return false;
    }
    if (storage_is_v2() && !storage_v2_remove(path))
        return false;
    if (index >= 0) {
        memset(&vfs_files[index], 0, sizeof(vfs_files[index]));
        if (!storage_is_v2())
            storage_mark_dirty();
    }
    if (listed >= 0)
        memset(&v2_list[listed - VFS_MAX_FILES], 0, sizeof(v2_list[0]));
    return true;
}

bool vfs_copy(const char *oldpath, const char *newpath) {
    char old[1024], dest[1024];
    if (!canon_native(old, sizeof old, oldpath) || !canon_native(dest, sizeof dest, newpath) ||
        !strcmp(old, dest))
        return false;
    int source = vfs_find(old);
    if (source < 0 || !vfs_read(source))
        return false;
    VFile *file = vfs_entry(source);
    int target = vfs_create(dest);
    return target >= 0 && vfs_write(target, file->data);
}

bool vfs_rename(const char *oldpath, const char *newpath) {
    char old[1024], dest[1024];
    if (!canon_native(old, sizeof old, oldpath) || !canon_native(dest, sizeof dest, newpath) ||
        !strcmp(old, "/") || !strcmp(dest, "/"))
        return false;
    if ((strlen(old) < 128 && extfs_path(old)) || (strlen(dest) < 128 && extfs_path(dest))) {
        if (strlen(old) < 128 && strlen(dest) < 128 && extfs_path(old) && extfs_path(dest))
            return extfs_rename(old, dest);
        if (strlen(old) < 128 && extfs_path(old) && !extfs_path_writable(old))
            return false;
        /* Cross-volume moves use a durable copy before deleting the source. */
        if (vfs_find(dest) >= 0 || !vfs_copy(old, dest) || !vfs_sync())
            return false;
        return vfs_remove(old);
    }
    int source = find_canonical(old);
    if (source < 0) {
        if (!(storage_is_v2() && storage_v2_rename(old, dest)))
            return false;
        int listed = find_v2_list(old);
        if (listed >= 0 && strlen(dest) < 128) {
            strcopy(v2_list[listed - VFS_MAX_FILES].name, dest,
                    sizeof(v2_list[0].name));
        } else if (listed >= 0)
            memset(&v2_list[listed - VFS_MAX_FILES], 0, sizeof(v2_list[0]));
        return true;
    }
    if (!strcmp(old, dest))
        return true;
    if (find_canonical(dest) >= 0 || !parent_exists(dest) || below(dest, old))
        return false;
    if (storage_is_v2() && !storage_v2_rename(old, dest))
        return false;
    size_t from = strlen(old), to = strlen(dest);
    /* Validate every child first, so a long descendant cannot cause half a move. */
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (!vfs_files[i].used || (i != source && !below(vfs_files[i].name, old)))
            continue;
        if (to + strlen(vfs_files[i].name) - from >= sizeof(vfs_files[i].name))
            return false;
    }
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (!vfs_files[i].used || (i != source && !below(vfs_files[i].name, old)))
            continue;
        size_t rest = strlen(vfs_files[i].name) - from;
        memmove(vfs_files[i].name + to, vfs_files[i].name + from, rest + 1);
        memcpy(vfs_files[i].name, dest, to);
    }
    if (!storage_is_v2())
        storage_mark_dirty();
    return true;
}

bool vfs_store(const char *path, const void *data, uint64_t len) {
    char canon[1024];
    if ((!data && len) || !path)
        return false;
    if (!canon_native(canon, sizeof canon, path)) {
        if (storage_is_v2())
            (void)storage_v2_write(path, data, len);
        return false;
    }
    if (!strcmp(canon, "/"))
        return false;
    if (strlen(canon) < 128 && extfs_path(canon))
        return false;
    if (!storage_is_v2()) {
        if (len >= VFS_FILE_CAP)
            return false;
        for (uint64_t i = 0; i < len; ++i)
            if (!((const unsigned char *)data)[i])
                return false;
        int index = vfs_create(canon);
        if (index < 0)
            return false;
        char tmp[VFS_FILE_CAP];
        memcpy(tmp, data, (size_t)len);
        tmp[len] = 0;
        return vfs_write(index, tmp);
    }
    if (!parent_exists(canon) || !storage_v2_write(canon, data, len))
        return false;
    int existing = install_visible(canon, false, (size_t)len);
    if (existing >= 0 && existing < VFS_MAX_FILES && len < VFS_FILE_CAP) {
        memcpy(vfs_files[existing].data, data, (size_t)len);
        vfs_files[existing].data[len] = 0;
        vfs_files[existing].size = (size_t)len;
    }
    return true;
}
bool vfs_fetch(const char *path, void *data, uint64_t cap, uint64_t *out_len) {
    char canon[1024];
    if (!canon_native(canon, sizeof canon, path))
        return false;
    if (storage_is_v2())
        return storage_v2_read(canon, data, cap, out_len);
    int index = find_canonical(canon);
    if (index < 0 || vfs_files[index].is_dir || vfs_files[index].size > cap)
        return false;
    if (data && vfs_files[index].size)
        memcpy(data, vfs_files[index].data, vfs_files[index].size);
    if (out_len)
        *out_len = vfs_files[index].size;
    return true;
}

static void seed_file(const char *name, const char *text) {
    if (vfs_find(name) < 0)
        vfs_write(vfs_create(name), text);
}

void vfs_init(void) {
    memset(vfs_files, 0, (sizeof(VFile) * VFS_MAX_FILES));
    bool mounted = storage_init();
    const char *directories[] = {
        "/", "/home", "/home/ark", "/home/ark/Documents", "/home/ark/Desktop", "/tmp", "/etc"};
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i)
        vfs_mkdir(directories[i]);
    seed_file("README.txt", "ArkOS — 原生内核操作系统\n\n"
                            "欢迎使用 ArkOS。图形桌面与内核均为独立实现，不使用 Linux 内核。\n"
                            "终端输入 help 查看命令；文件管理器支持目录、编辑与删除。\n\n"
                            "若状态显示 ArkFS，文件通过 ATA PIO 写入 arkos-data.img。\n"
                            "使用 sync 或记事本保存可立即落盘；桌面也会定期同步。\n"
                            "无磁盘或磁盘格式不匹配时进入 RAM 模式，重启会丢失更改。\n\n"
                            "ArkFS v1：64 个条目，单文件最多 16383 字节，路径最多 127 字节。\n"
                            "ArkFS v2：容量跟空闲块走，路径最多 1023 字节。支持中文路径。\n"
                            "原生终端提供类 Unix 命令，尚不能运行 Linux ELF 程序。\n");
    seed_file("hello.txt",
              "Hello from ArkOS!\n你好，ArkOS！\nNative kernel, genuine persistent disk I/O.\n");
    seed_file("notes.txt", "我的 ArkOS 笔记\n\n从这里开始记录。保存后可通过 sync 确认磁盘写入。\n");
    seed_file("/home/ark/Documents/使用说明.txt",
              "ArkOS 使用说明\n\n"
              "文件：打开文件管理器，点击目录进入，选择文本以记事本编辑。\n"
              "终端：help、pwd、ls、cd、mkdir、cat、echo、cp、mv、rm、sync。\n"
              "磁盘：ArkFS 使用两个快照与 CRC 校验，恢复最近一次完整提交。\n"
              "退出：使用系统关机或 reboot，确保保存完成后再关闭虚拟机。\n"
              "未连接数据磁盘时仅有临时内存文件，请注意存储状态。\n");
    seed_file("/etc/hostname", "arkos\n");
    const char *release = "NAME=ArkOS\nVERSION=0.13.0+mouse1\nID=arkos\nKERNEL=ArkOS-native\n";
    int release_index = vfs_find("/etc/os-release");
    if (release_index < 0)
        release_index = vfs_create("/etc/os-release");
    if (release_index >= 0 && strcmp(vfs_files[release_index].data, release))
        vfs_write(release_index, release);
    if (mounted)
        storage_sync();
    extfs_init();
}

typedef struct {
    const char *dir;
    size_t dir_len;
} ListFill;

static bool list_fill_child(const char *path, int is_dir, uint64_t size, void *user) {
    ListFill *fill = user;
    size_t n = fill->dir_len;
    if (!strcmp(fill->dir, "/")) {
        if (path[0] != '/' || !path[1])
            return true;
        for (const char *p = path + 1; *p; ++p)
            if (*p == '/')
                return true;
    } else {
        if (strncmp(path, fill->dir, n) || path[n] != '/' || !path[n + 1])
            return true;
        for (const char *p = path + n + 1; *p; ++p)
            if (*p == '/')
                return true;
    }
    if (strlen(path) >= 128)
        return true;
    if (v2_list_count >= VFS_V2_LIST_MAX)
        return true;
    VfsLite *lite = &v2_list[v2_list_count++];
    memset(lite, 0, sizeof(*lite));
    lite->used = true;
    lite->is_dir = is_dir != 0;
    lite->size = (size_t)size;
    strcopy(lite->name, path, sizeof(lite->name));
    return true;
}

/* Native ArkFS records and external records retain separate storage/lifetimes. */
VFile *vfs_entry(int index) {
    if (index < 0)
        return 0;
    if (index < VFS_MAX_FILES)
        return &vfs_files[index];
    if (index < VFS_EXT_BASE) {
        VfsLite *lite = &v2_list[index - VFS_MAX_FILES];
        memset(&v2_facade, 0, sizeof(v2_facade));
        if (!lite->used)
            return &v2_facade;
        v2_facade.used = true;
        v2_facade.is_dir = lite->is_dir;
        v2_facade.size = lite->size;
        strcopy(v2_facade.name, lite->name, sizeof(v2_facade.name));
        return &v2_facade;
    }
    return extfs_entry(index - VFS_EXT_BASE);
}
int vfs_entry_limit(void) {
#ifdef ARK_STORAGE_HOST_TEST
    return VFS_EXT_BASE;
#else
    return VFS_EXT_BASE + EXTFS_MAX_ENTRIES;
#endif
}
bool vfs_read(int index) {
    if (index >= VFS_EXT_BASE)
        return extfs_read(index - VFS_EXT_BASE);
    if (index >= VFS_MAX_FILES && index < VFS_EXT_BASE) {
        VfsLite *lite = &v2_list[index - VFS_MAX_FILES];
        return lite->used && !lite->is_dir;
    }
    return index >= 0 && vfs_files[index].used && !vfs_files[index].is_dir;
}
bool vfs_list(const char *directory) {
    char path[1024];
    if (!canon_native(path, sizeof path, directory))
        return false;
    if (strlen(path) < 128 && extfs_path(path))
        return extfs_list(path);
    if (storage_is_v2()) {
        uint64_t size = 0;
        uint32_t type = 0;
        if (!storage_v2_lookup(path, &size, &type) || type != 2)
            return false;
        memset(v2_list, 0, sizeof(v2_list));
        v2_list_count = 0;
        ListFill fill = {path, strlen(path)};
        return storage_v2_visit(list_fill_child, &fill);
    }
    int i = find_canonical(path);
    return i >= 0 && vfs_files[i].is_dir;
}
bool vfs_sync(void) {
    bool native_ok = !storage_mounted() || storage_sync();
    bool external_ok = extfs_sync();
    return native_ok && external_ok;
}
const char *vfs_error(void) {
    const char *external = extfs_error();
    if (external[0])
        return external;
    const char *disk = storage_error();
    return disk[0] ? disk : "Filesystem operation failed (path, capacity or read-only volume)";
}

#ifdef ARK_STORAGE_HOST_TEST
__attribute__((weak)) bool storage_is_v2(void) { return false; }
__attribute__((weak)) uint64_t storage_free_bytes(void) { return 0; }
__attribute__((weak)) bool storage_v2_write(const char *path, const void *data, uint64_t len) {
    (void)path; (void)data; (void)len; return false;
}
__attribute__((weak)) bool storage_v2_read(const char *path, void *data, uint64_t cap, uint64_t *out_len) {
    (void)path; (void)data; (void)cap; (void)out_len; return false;
}
__attribute__((weak)) bool storage_v2_mkdir(const char *path) { (void)path; return false; }
__attribute__((weak)) bool storage_v2_remove(const char *path) { (void)path; return false; }
__attribute__((weak)) bool storage_v2_rename(const char *from, const char *to) {
    (void)from; (void)to; return false;
}
__attribute__((weak)) bool storage_v2_lookup(const char *path, uint64_t *size, uint32_t *type) {
    (void)path; (void)size; (void)type; return false;
}
__attribute__((weak)) bool storage_v2_visit(StorageVisit visit, void *user) {
    (void)visit; (void)user; return false;
}
#endif
