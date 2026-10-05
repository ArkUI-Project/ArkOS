#ifndef ARK_H
#define ARK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef struct {
    uint64_t framebuffer;
    uint32_t width, height, pitch, bpp;
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
    uint64_t memory_mib;
} BootInfo;
enum { EV_KEY = 1, EV_MOUSE = 2, EV_TOUCH = 3, EV_POINTER = 4, EV_SCROLL = 8 };
enum {
    KEY_ENTER = 256,
    KEY_BACKSPACE,
    KEY_ESCAPE,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_TAB,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_DELETE,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_LAUNCHER
};
typedef struct {
    int type, key, dx, dy;
    uint8_t buttons;
} Event;
void platform_init(uint32_t magic, uint32_t mb_addr, BootInfo *info);
bool platform_next_event(Event *ev);
uint64_t platform_ticks(void);
uint64_t platform_millis(void);
void platform_idle(void);
void platform_reboot(void);
void platform_poweroff(void);
void serial_write(const char *s);
void platform_time(int *h, int *m, int *s);
bool platform_datetime(int *year, int *month, int *day, int *hour, int *minute, int *second,
                       int *weekday);
void *memset(void *p, int v, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
size_t strlen(const char *s);
int memcmp(const void *a, const void *b, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
void strcopy(char *d, const char *s, size_t cap);
void uint_to_str(uint64_t n, char *out);
#define VFS_MAX_FILES 64
#define VFS_FILE_CAP 16384
#define VFS_PATH_MAX 1023
#define VFS_V2_LIST_MAX 128
#define VFS_EXT_BASE (VFS_MAX_FILES + VFS_V2_LIST_MAX)
#ifndef ARK_FILE_SLOTS
#define ARK_FILE_SLOTS 256u
#endif
typedef struct {
    bool used;
    bool is_dir;
    char name[128];
    char data[VFS_FILE_CAP];
    size_t size;
} VFile;
extern VFile vfs_files[];
void vfs_init(void);
int vfs_find(const char *name);
int vfs_create(const char *name);
bool vfs_write(int index, const char *text);
bool vfs_store(const char *path, const void *data, uint64_t len);
bool vfs_fetch(const char *path, void *data, uint64_t cap, uint64_t *out_len);
bool vfs_remove(const char *name);
bool vfs_path_canonical_cap(char *out, size_t cap, const char *path);
VFile *vfs_entry(int index);
int vfs_entry_limit(void);
bool vfs_list(const char *directory);
bool vfs_read(int index);
#define SHELL_LINES 256
#define SHELL_COLS 256
typedef struct {
    char lines[SHELL_LINES][SHELL_COLS];
    int line_count, action;
    char cwd[128], history[32][1024];
    unsigned history_count, history_next;
} ShellContext;
ShellContext *shell_context(void);
void shell_switch(ShellContext *context);
#define shell_lines (shell_context()->lines)
#define shell_line_count (shell_context()->line_count)
#define shell_action (shell_context()->action)
void shell_init(const BootInfo *info);
void shell_set_home(const char *home);
void shell_execute(const char *command);
void shell_print(const char *text);
/* Shell actions handled by the desktop: 1 files,2 notes,3 settings,4 about. */
const char *shell_cwd(void);
const char *shell_prompt(void);
#endif
