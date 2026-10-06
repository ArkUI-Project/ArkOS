#ifndef ARK_PUBLIC_API_H
#define ARK_PUBLIC_API_H
/* ArkOS native userspace ABI v1. Not Linux/POSIX. All pointer-shaped integers
 * refer to the caller's address space and MUST be copied/validated by kernel.
 * x86_64 int80: RAX=number, RDI/RSI/RDX/R10/R8/R9=arguments; RAX=result.
 * Complex calls pass request pointer in RDI and sizeof(request) in RSI.
 * Results >=0 succeed; negative results are errors. No kernel pointers cross. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cursor.h"
#define ARK_ABI_VERSION 1u
#define ARK_CAP_SYSTEM 1u
#define ARK_CAP_FILES 2u
#define ARK_CAP_UI 4u
#define ARK_CAP_NETWORK 8u
#define ARK_CAP_PROCESS 16u
#define ARK_CAP_ACTIVITY 32u
#define ARK_CAP_DEVICE 64u
#define ARK_APP_BROWSER 6u
#define ARK_APP_CLOCK 7u
#define ARK_APP_PAINT 8u
#define ARK_APP_MARKDOWN 9u
#define ARK_MAX_SURFACES 12u
#define ARK_SURFACE_MAX_W 3840u
#define ARK_SURFACE_MAX_H 2160u
#define ARK_FILE_MAX 16383u
#define ARK_FILE_SLOTS 256u

enum {
    ARK_SYS_EXIT = 0,
    ARK_SYS_YIELD = 1,
    ARK_SYS_SPAWN = 2,
    ARK_SYS_GETPID = 3,
    ARK_SYS_MSG_SEND = 4,
    ARK_SYS_MSG_RECV = 5,
    ARK_SYS_INFO = 16,
    ARK_SYS_EVENT = 17,
    ARK_SYS_PRESENT = 18,
    ARK_SYS_GPU = 19,
    ARK_SYS_FILE = 20,
    ARK_SYS_STORAGE = 21,
    ARK_SYS_POWER = 22,
    ARK_SYS_LOG = 23,
    ARK_SYS_SURFACE = 24,
    ARK_SYS_NETWORK = 25,
    ARK_SYS_ACCOUNT = 26,
    ARK_SYS_TICKS = 27,
    ARK_SYS_THREAD = 28,
    ARK_SYS_TASKS = 29,
    ARK_SYS_INSTALL = 30,
    ARK_SYS_BLOB = 31,
    ARK_SYS_PERMISSION = 32,
    ARK_SYS_ACTIVITY = 33,
    ARK_SYS_LAUNCH = 34,
    ARK_SYS_MILLIS = 35,
    ARK_SYS_PACKAGE = 36,
    ARK_SYS_DATETIME = 37,
    ARK_SYS_MEMORY = 38,
    ARK_SYS_REGISTRY = 39,
    ARK_SYS_DRAG = 40,
    ARK_SYS_PERFORMANCE = 41,
    ARK_SYS_VM = 42,
    ARK_SYS_COMPOSITOR = 43,
    ARK_SYS_DEVICE = 44,
    ARK_SYS_DRIVER = 45
};
enum {
    ARK_VM_RESERVE = 0,
    ARK_VM_COMMIT = 1,
    ARK_VM_DECOMMIT = 2,
    ARK_VM_RELEASE = 3,
    ARK_VM_PROTECT = 4,
    ARK_VM_QUERY = 5,
    ARK_VM_TRIM = 6
};
enum { ARK_VM_READ = 1u, ARK_VM_WRITE = 2u };
typedef struct {
    uint32_t op, flags;
    uint64_t address, bytes, reserved_bytes, committed_bytes, resident_bytes, swapped_bytes, faults,
        pages_in, pages_out, pool_bytes, free_bytes, swap_capacity_bytes;
} ArkVMRequest;
enum {
    ARK_EV_KEY = 1,
    ARK_EV_MOUSE = 2,
    ARK_EV_TOUCH = 3,
    ARK_EV_POINTER = 4,
    ARK_EV_CLOSE = 5,
    ARK_EV_RESIZE = 6,
    ARK_EV_FOCUS = 7,
    ARK_EV_SCROLL = 8,
    ARK_EV_OPEN = 9,
    ARK_EV_TEXT = 10,
    ARK_EV_THEME = 11,
    ARK_EV_NEW = 12,
    ARK_EV_DROP = 13,
    ARK_EV_DRAG_END = 14
};
typedef struct {
    int32_t type, key, x, y;
    uint32_t buttons, reserved;
} ArkEvent;
typedef struct {
    int32_t x, y, w, h;
} ArkRect;
typedef struct {
    uint32_t peer, type, length, reserved;
    uint8_t data[128];
} ArkMessage;
typedef struct {
    char program[64], argument[128];
    uint32_t flags, pid;
    char error[128];
} ArkSpawn;
enum { ARK_SPAWN_NEW = 1u };
enum {
    ARK_REG_GET = 0,
    ARK_REG_SET = 1,
    ARK_REG_DELETE = 2,
    ARK_REG_LIST = 3,
    ARK_REG_REVISION = 4
};
enum { ARK_REG_INTEGER = 1, ARK_REG_STRING = 2, ARK_REG_BINARY = 3 };
typedef struct {
    uint32_t op, type, length, index, count, reserved;
    int64_t integer;
    uint64_t generation;
    char key[128];
    uint8_t value[512];
    char error[128];
} ArkRegistryRequest;

typedef struct {
    uint32_t abi, width, height, bpp;
    uint64_t memory_mib, ticks;
    int32_t hour, minute, second;
    uint32_t pid, uid, caps;
    char gpu_name[64], input_name[64], home[128];
    uint32_t gpu_accelerated, hardware_cursor, gpu_capabilities, input_contacts;
    uint64_t frames, fill_commands, copy_commands, update_commands;
    uint64_t filled_pixels, copied_pixels, uploaded_pixels, cursor_updates;
} ArkSystemInfo;
/* EVENT: flags0 reads ordinary platform queue; flags1 reads virtio queue.
 * System-only. Return1event/0empty. Third-party input uses SURFACE/NEXT_EVENT. */
typedef struct {
    uint32_t flags, reserved;
    ArkEvent event;
} ArkEventRequest;
typedef struct {
    int32_t year, month, day, hour, minute, second, weekday;
    uint32_t valid;
} ArkDateTime;
/* PRESENT is SYSTEM-only; kernel validates buffer layout and copies clipped
 * damage from caller pixels into a kernel-owned presentation buffer. */
typedef struct {
    uint64_t pixels;
    uint32_t width, height, stride, flags;
    ArkRect damage;
} ArkPresent;
/* ANIMATION streams the selected FULL/damage region and invalidates the GPU
 * retained shadow; it retains the same SYSTEM gate and source validation. */
enum { ARK_PRESENT_FULL = 1u, ARK_PRESENT_ANIMATION = 2u };
enum { ARK_GPU_CURSOR_MOVE = 0, ARK_GPU_CURSOR_SCALE = 1, ARK_GPU_CURSOR_SHAPE = 2 };
typedef struct {
    uint32_t op, visible, scale, reserved;
    int32_t x, y;
} ArkGpuRequest;
/* SYSTEM-only, fixed GPU programs. Pixels stay in the caller's address space;
 * PCI DMA uses kernel-owned staging pages. Render changes only the clipped
 * glass rectangle after all GPU passes and the readback fence have completed. */
enum {
    ARK_GLASS_QUERY = 0,
    ARK_GLASS_RENDER = 1,
    ARK_GLASS_DISPERSION = 1u,
    ARK_GLASS_SHADOW = 2u
};
typedef struct {
    uint32_t op, flags;
    uint64_t pixels;
    uint32_t width, height, stride, radius;
    ArkRect rect;
    uint32_t color, tint, edge, reserved;
    ArkRect shadow;
    uint64_t jobs, passes, uploaded_bytes, readback_bytes, fences, milliseconds;
    uint64_t validate_ms, stage_ms, upload_ms, submit_ms, readback_ms, copy_ms;
} ArkGlassRequest;

enum {
    ARK_FILE_SNAPSHOT = 0,
    ARK_FILE_STAT = 1,
    ARK_FILE_LIST = 2,
    ARK_FILE_READ = 3,
    ARK_FILE_WRITE = 4,
    ARK_FILE_CREATE = 5,
    ARK_FILE_MKDIR = 6,
    ARK_FILE_REMOVE = 7,
    ARK_FILE_RENAME = 8,
    ARK_FILE_COPY = 9,
    ARK_FILE_SYNC = 10,
    ARK_FILE_WRITABLE = 11,
    ARK_FILE_READ_BYTES = 12
};
/* READ_BYTES: binary-safe positional read. capacity<=ARK_FILE_MAX; offset may
 * address any byte in an external file. Native text files are also readable.
 * Same FILES capability and canonical-path/UID policy as READ; no terminator. */
enum { ARK_FILE_USED = 1u, ARK_FILE_DIRECTORY = 2u, ARK_FILE_READ_ONLY = 4u };
typedef struct {
    int32_t index;
    uint32_t flags;
    uint64_t size;
    char path[128];
} ArkFileInfo;
typedef struct {
    uint32_t op, flags;
    char path[128], other[128];
    uint64_t buffer, offset;
    uint32_t capacity, count;
    ArkFileInfo info;
    char error[128];
} ArkFileRequest;
/* READ/WRITE capacity is bytes; READ returns count (NUL not included). Kernel
 * enforces <=ARK_FILE_MAX and validates canonical scope. WRITE input excludes
 * NUL; kernel adds its own terminator. STAT/CREATE/MKDIR return info.
 * SNAPSHOT: SYSTEM only, buffer[capacity] ArkFileInfo entries (capacity<=128),
 * including unused slots; count returned. LIST populates kernel directory cache
 * and may optionally return scoped immediate child metadata the same way.
 * Index is metadata only: all reads/writes resolve and authorize path again.
 * /.system and aliases MUST be denied, including to desktop file/shell clients. */
typedef struct {
    uint32_t mounted, read_only;
    uint64_t capacity_bytes;
    char mountpoint[32];
} ArkVolumeInfo;
enum { ARK_STORAGE_NEEDS_UNLOCK = 1u, ARK_STORAGE_DO_UNLOCK = 2u, ARK_STORAGE_ENCRYPTED = 4u };
typedef struct {
    uint32_t mounted, arkfs2;
    uint64_t capacity_bytes, used_bytes;
    char status[128], error[128], external_status[128];
    ArkVolumeInfo volumes[2];
    uint64_t free_bytes;
    uint32_t flags, reserved2; /* NEEDS_UNLOCK|ENCRYPTED out; DO_UNLOCK in */
    char passphrase[64];       /* in: DO_UNLOCK; kernel zeroes after use */
} ArkStorageInfo;
/* POWER: arg1=0 reboot,1 shutdown. LOG: arg1=buffer,arg2=bounded length<=4096. */

enum {
    ARK_SURFACE_CREATE = 0,
    ARK_SURFACE_PRESENT = 1,
    ARK_SURFACE_QUERY = 2,
    ARK_SURFACE_COPY = 3,
    ARK_SURFACE_SEND_EVENT = 4,
    ARK_SURFACE_NEXT_EVENT = 5,
    ARK_SURFACE_CLOSE = 6,
    ARK_SURFACE_RESIZE = 7,
    ARK_SURFACE_INPUT = 8,
    ARK_SURFACE_CURSORS = 9,
    ARK_SURFACE_CURSOR_AT = 10
};
typedef struct {
    ArkRect rect;
    uint32_t shape;
} ArkCursorRegion;
enum { ARK_SURFACE_ALIVE = 1u, ARK_SURFACE_VISIBLE = 2u, ARK_SURFACE_TEXT_INPUT = 4u };
typedef struct {
    uint32_t op, id, width, height, stride, flags;
    uint64_t pixels;
    ArkRect damage;
    uint32_t generation, pid, capacity, reserved;
    char title[64];
    ArkEvent event;
    char error[128];
} ArkSurfaceRequest;
/* CREATE creates an own surface, id assigned0..11. PRESENT copies pixels into
 * kernel-owned storage and increments generation; flags FULL selects allpixels.
 * QUERY id0..11 returns dimensions/title/owner/generation/ALIVE; COPY copies the
 * snapshot to SYSTEM caller's pixels, capacity in pixels. SEND_EVENT is SYSTEM
 * only and enqueues event to surface owner; NEXT_EVENT returns1event/0empty.
 * RESIZE is owner-only, changes the backing store after the app handles RESIZE.
 * NonSYSTEM callers may CREATE/PRESENT/NEXT_EVENT/CLOSE/RESIZE their own surfaces.
 * Actual process identity is obtained by kernel; pid in requests is not trusted. */

enum {
    ARK_NET_STATUS = 0,
    ARK_NET_HTTP_GET = 1,
    ARK_NET_HTTP_STATE = 2,
    ARK_NET_HTTP_READ = 3,
    ARK_NET_HTTP_CANCEL = 4
};
typedef struct {
    uint32_t op, state, present, link, configured, ipv4, mask, gateway, dns, http_status;
    uint8_t mac[6];
    uint16_t reserved;
    uint64_t rx_packets, tx_packets, dropped_packets;
    uint64_t buffer, offset;
    uint32_t capacity, count, total_size, flags;
    char url[256], message[128], content_type[96];
} ArkNetworkRequest;
/* HTTP operations require NETWORK capability; the single native HTTP operation
 * must be owned by initiating PID so other apps cannot read/cancel its body. */
enum {
    ARK_ACCOUNT_STATUS = 0,
    ARK_ACCOUNT_LIST = 1,
    ARK_ACCOUNT_ENROLL = 2,
    ARK_ACCOUNT_LOGIN = 3,
    ARK_ACCOUNT_LOGOUT = 4,
    ARK_ACCOUNT_LOCK = 5,
    ARK_ACCOUNT_UNLOCK = 6,
    ARK_ACCOUNT_CHANGE_PASSWORD = 7,
    ARK_ACCOUNT_CREATE_USER = 8,
    ARK_ACCOUNT_SET_DISABLED = 9
};
typedef struct {
    uint32_t op, status, uid, flags, index, count;
    char user[24], display[64], home[64];
    char secret[96], new_secret[96], error[128];
} ArkAccountRequest;
/* ACCOUNT nonSYSTEM may query current public status only. All authentication
 * and management operations require SYSTEM and kernel session/admin checks.
 * LIST index0..7 returns one public profile; secret fields must not be logged. */

enum {
    ARK_THREAD_CREATE = 0,
    ARK_THREAD_EXIT = 1,
    ARK_THREAD_JOIN = 2,
    ARK_THREAD_SLEEP = 3,
    ARK_THREAD_ID = 4,
    ARK_THREAD_WAIT = 5,
    ARK_THREAD_WAIT_MS = 6,
    ARK_THREAD_WAIT_MSG_MS = 7
};
typedef struct {
    uint32_t op, tid;
    uint64_t entry, argument, trampoline, ticks;
    int32_t status;
    uint32_t reserved;
} ArkThreadRequest;
typedef struct {
    uint32_t tid, pid, uid, state;
    int32_t exit_status;
    uint32_t thread;
    uint64_t capabilities, mapped_bytes, cpu_ticks, switches;
    char name[32];
} ArkTaskInfo;
typedef struct {
    ArkTaskInfo task;
    uint64_t reserved_bytes, committed_bytes, swapped_bytes, faults, read_bytes, write_bytes,
        messages_sent, messages_received;
} ArkTaskMetrics;
enum { ARK_TASK_LIST = 0, ARK_TASK_KILL = 1, ARK_TASK_METRICS = 2 };
typedef struct {
    uint32_t op, tid, capacity, count;
    uint64_t buffer, ticks;
    uint32_t online_cpus, reserved;
} ArkTaskRequest;
typedef struct {
    uint64_t millis, pool_bytes, free_bytes, kernel_bytes, user_bytes;
    uint32_t online_cpus, tasks;
    uint64_t cpu_busy_ms[8], cpu_total_ms[8];
    uint64_t reserved_bytes, committed_bytes, swapped_bytes, faults, pages_in, pages_out,
        swap_capacity_bytes, disk_read_bytes, disk_write_bytes, network_rx_bytes, network_tx_bytes;
} ArkPerformanceInfo;
enum {
    ARK_DRAG_BEGIN = 0,
    ARK_DRAG_STATUS = 1,
    ARK_DRAG_DELIVER = 2,
    ARK_DRAG_READ = 3,
    ARK_DRAG_CANCEL = 4,
    ARK_DRAG_ACCEPT = 5
};
enum { ARK_DRAG_TEXT = 1, ARK_DRAG_FILE = 2, ARK_DRAG_COPY = 1 };
typedef struct {
    uint32_t op, token, source, target, kind, action, length, active;
    int32_t x, y;
    char mime[48], data[512];
} ArkDragRequest;

enum {
    ARK_PERMISSION_GET = 0,
    ARK_PERMISSION_SET = 1,
    ARK_PERMISSION_REQUEST = 2,
    ARK_PERMISSION_PENDING = 3,
    ARK_PERMISSION_RESOLVE = 4
};
typedef struct {
    uint32_t op, app, grants, maximum;
    char name[32];
} ArkPermissionRequest;
/* REQUEST uses caller catalog identity. PENDING/RESOLVE are SYSTEM-only. */
enum { ARK_ACTIVITY_SET = 0, ARK_ACTIVITY_GET = 1 };
typedef struct {
    uint32_t op, app, active, kind;
    uint64_t deadline;
    char title[32];
} ArkActivityRequest;
typedef struct {
    char argument[128];
} ArkLaunchInfo;
#define ARK_BLOB_MAX (8u * 1024u * 1024u)
enum { ARK_BLOB_LIST = 0, ARK_BLOB_READ = 1, ARK_BLOB_WRITE = 2, ARK_BLOB_REMOVE = 3 };
typedef struct {
    uint32_t op, index, capacity, count, offset, size, checksum, reserved;
    uint64_t buffer;
    char name[64], error[128];
} ArkBlobRequest;
enum {
    ARK_INSTALL_LIST = 0,
    ARK_INSTALL_BEGIN = 1,
    ARK_INSTALL_STEP = 2,
    ARK_INSTALL_STATUS = 3,
    ARK_INSTALL_CANCEL = 4
};
typedef struct {
    uint32_t op, disk, present, in_use, media, state, done, total;
    uint64_t sectors;
    char confirm[8], message[128];
    uint32_t features; /* ARKFS2_FEAT_ENCRYPT / COMPRESS on BEGIN */
    uint32_t can_encrypt; /* LIST: 1 if RDRAND usable */
    char passphrase[64]; /* BEGIN when ENCRYPT set */
} ArkInstallRequest;

/* SYS_DEVICE (44): kernel-owned hardware inventory. Enumeration, query and
 * counters are public inventory in an active session; reads and controls need
 * ARK_CAP_DEVICE. No request field grants authority: class, flags and identity
 * come from kernel device state, never from the caller. */
enum {
    ARK_DEV_ENUMERATE = 0,
    ARK_DEV_QUERY = 1,
    ARK_DEV_STATS = 2,
    ARK_DEV_READ = 3,
    ARK_DEV_CONTROL = 4,
    ARK_DEV_WRITE = 5
};
enum { ARK_DEVCTL_FLUSH = 1, ARK_DEVCTL_REFRESH = 2 };
enum {
    ARK_DEV_CLASS_PLATFORM = 1,
    ARK_DEV_CLASS_CPU = 2,
    ARK_DEV_CLASS_BLOCK = 3,
    ARK_DEV_CLASS_NETWORK = 4,
    ARK_DEV_CLASS_DISPLAY = 5,
    ARK_DEV_CLASS_INPUT = 6,
    ARK_DEV_CLASS_SERIAL = 7,
    ARK_DEV_CLASS_VOLUME = 8,
    ARK_DEV_CLASS_POWER = 9,
    ARK_DEV_CLASS_SENSOR = 10,
    ARK_DEV_CLASS_PCI = 11
};
enum { ARK_BUS_PLATFORM = 0, ARK_BUS_PCI = 1, ARK_BUS_ISA = 2, ARK_BUS_VIRTUAL = 3 };
enum { ARK_DEV_PRESENT = 1u, ARK_DEV_READABLE = 2u, ARK_DEV_WRITABLE = 4u,
       ARK_DEV_SYSTEM_VOLUME = 8u, ARK_DEV_REMOVABLE = 16u, ARK_DEV_MODULE = 32u };
enum { ARK_DEV_STATE_UNKNOWN = 0, ARK_DEV_STATE_OK = 1, ARK_DEV_STATE_ERROR = 2,
       ARK_DEV_STATE_ABSENT = 3 };
#define ARK_DEV_READ_CAP 65536u
#define ARK_DEV_READ_SECTORS 128u
typedef struct {
    uint32_t index, class_id, bus, bdf, vendor, device, device_class, flags, state, unit;
    uint64_t blocks, generation, rx_bytes, tx_bytes, ops, errors, last_event_ms;
    char name[48], driver[32], detail[96];
} ArkDeviceInfo;
typedef struct {
    uint32_t op, index, flags, generation, control, capability, status;
    uint64_t buffer, offset;
    uint32_t capacity, count, sectors, reserved;
    char error[128];
    ArkDeviceInfo info;
} ArkDeviceRequest;
/* READ on a BLOCK node copies ARK_DEV_READ_CAP bounded sectors into buffer.
 * WRITE is system-only and copies bounded sectors from buffer into a
 * non-system BLOCK node. The ArkFS system volume is never reachable either
 * way, and WRITE requires the SYSTEM capability on top of an active session. */
enum {
    ARK_DRV_LIST = 0,
    ARK_DRV_QUERY = 1,
    ARK_DRV_INSTALL = 2,
    ARK_DRV_REMOVE = 3
};
enum {
    ARK_DRV_STATE_LOADED = 1,
    ARK_DRV_STATE_DISABLED = 2,
    ARK_DRV_STATE_FAILED = 3
};
typedef struct {
    uint32_t index, state, version, api, flags;
    uint64_t image_bytes, bytes;
    char name[32], detail[96];
    uint8_t sha256[32];
} ArkDriverInfo;
typedef struct {
    uint32_t op, index, flags, count;
    char path[128], name[32], error[128];
    ArkDriverInfo info;
} ArkDriverRequest;
/* LIST/QUERY report the installed set in an active session. INSTALL accepts an
 * .arco from a scoped local path or blob:NAME, verifies it against the
 * protected manifest and activates it; REMOVE disables the module and drops
 * its manifest entry. Both mutations need SYSTEM, an active admin session and
 * a well-formed ARCO1 image; code executes only inside the kernel. */

#ifndef ARK_KERNEL
#ifdef ARK_API_HOST_TEST
/* Host regression seam only: production builds always execute int80. */
int64_t ark_test_syscall6(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#endif
static inline int64_t ark_syscall6(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                   uint64_t e, uint64_t f) {
#ifdef ARK_API_HOST_TEST
    return ark_test_syscall6(n, a, b, c, d, e, f);
#else
    register uint64_t r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
    uint64_t result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "memory", "cc");
    return (int64_t)result;
#endif
}
static inline int64_t ark_call(uint64_t n, void *p, size_t bytes) {
    return ark_syscall6(n, (uintptr_t)p, bytes, 0, 0, 0, 0);
}
static inline void *ark_memory(size_t bytes) {
    int64_t p = ark_syscall6(ARK_SYS_MEMORY, bytes, 0, 0, 0, 0, 0);
    return p > 0 ? (void *)(uintptr_t)p : 0;
}
static inline int64_t ark_vm(ArkVMRequest *p) {
    return ark_call(ARK_SYS_VM, p, sizeof(*p));
}
static inline bool ark_memory_release(void *p, size_t bytes) {
    ArkVMRequest r = {
        .op = ARK_VM_RELEASE, .address = (uintptr_t)p, .bytes = (bytes + 4095) & ~4095ull};
    return ark_vm(&r) >= 0;
}
static inline uint64_t ark_ticks(void) {
    return (uint64_t)ark_syscall6(ARK_SYS_TICKS, 0, 0, 0, 0, 0, 0);
}
/* Monotonic milliseconds. Legacy TICKS and SLEEP/WAIT retain 100 Hz units. */
static inline uint64_t ark_millis(void) {
    return (uint64_t)ark_syscall6(ARK_SYS_MILLIS, 0, 0, 0, 0, 0, 0);
}
static inline int64_t ark_wait_ms(uint64_t milliseconds) {
    ArkThreadRequest r = {0};
    r.op = ARK_THREAD_WAIT_MS;
    r.ticks = milliseconds;
    return ark_call(ARK_SYS_THREAD, &r, sizeof r);
}
static inline int64_t ark_wait_message_ms(uint64_t milliseconds) {
    ArkThreadRequest r = {0};
    r.op = ARK_THREAD_WAIT_MSG_MS;
    r.ticks = milliseconds;
    return ark_call(ARK_SYS_THREAD, &r, sizeof r);
}
static inline void ark_yield(void) {
    (void)ark_syscall6(ARK_SYS_YIELD, 0, 0, 0, 0, 0, 0);
}
static inline void ark_yield_busy(void) {
    (void)ark_syscall6(ARK_SYS_YIELD, 1, 0, 0, 0, 0, 0);
}
static inline int64_t ark_thread(ArkThreadRequest *p) {
    return ark_call(ARK_SYS_THREAD, p, sizeof(*p));
}
static inline int64_t ark_tasks(ArkTaskRequest *p) {
    return ark_call(ARK_SYS_TASKS, p, sizeof(*p));
}
static inline uint32_t ark_pid(void) {
    return (uint32_t)ark_syscall6(ARK_SYS_GETPID, 0, 0, 0, 0, 0, 0);
}
static inline void ark_exit(int code) {
    (void)ark_syscall6(ARK_SYS_EXIT, (uint64_t)(int64_t)code, 0, 0, 0, 0, 0);
    for (;;)
        ark_yield();
}
static inline int64_t ark_info(ArkSystemInfo *p) {
    return ark_call(ARK_SYS_INFO, p, sizeof(*p));
}
static inline int64_t ark_datetime(ArkDateTime *p) {
    return ark_call(ARK_SYS_DATETIME, p, sizeof(*p));
}
static inline int64_t ark_file(ArkFileRequest *p) {
    return ark_call(ARK_SYS_FILE, p, sizeof(*p));
}
static inline int64_t ark_surface(ArkSurfaceRequest *p) {
    return ark_call(ARK_SYS_SURFACE, p, sizeof(*p));
}
static inline int64_t ark_network(ArkNetworkRequest *p) {
    return ark_call(ARK_SYS_NETWORK, p, sizeof(*p));
}
static inline int64_t ark_account(ArkAccountRequest *p) {
    return ark_call(ARK_SYS_ACCOUNT, p, sizeof(*p));
}
static inline int64_t ark_spawn(ArkSpawn *p) {
    return ark_call(ARK_SYS_SPAWN, p, sizeof(*p));
}
static inline int64_t ark_registry(ArkRegistryRequest *p) {
    return ark_call(ARK_SYS_REGISTRY, p, sizeof(*p));
}
static inline int64_t ark_drag(ArkDragRequest *p) {
    return ark_call(ARK_SYS_DRAG, p, sizeof(*p));
}
static inline int64_t ark_device(ArkDeviceRequest *p) {
    return ark_call(ARK_SYS_DEVICE, p, sizeof(*p));
}
static inline int64_t ark_driver(ArkDriverRequest *p) {
    return ark_call(ARK_SYS_DRIVER, p, sizeof(*p));
}
#endif
#endif
