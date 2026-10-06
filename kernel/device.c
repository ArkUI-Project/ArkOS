/* ArkOS kernel device model. One bounded node per real device, registered by
 * the owning driver at discovery time. Nothing here talks to hardware; the
 * model only records what a driver proved and exposes it to SYS_DEVICE. */
#include "device.h"
#include "block.h"
#include "process.h"

static ArkDeviceInfo nodes[ARK_DEVICE_MAX];
static bool used[ARK_DEVICE_MAX];
static unsigned node_count;
static char error_text[96] = "";
static void (*polls[ARK_DEVICE_POLL_MAX])(void);
static unsigned poll_count;
static uint32_t current_generation = 1;
static struct {
    uint32_t pid, seen;
    bool noted;
} watchers[PROCESS_MAX];
static struct {
    uint32_t pid, count;
    uint64_t window;
} readers[PROCESS_MAX];

static bool terminated(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!s[i])
            return true;
    return false;
}
static void set_error(const char *text) {
    strcopy(error_text, text, sizeof error_text);
}
const char *device_error(void) {
    return error_text;
}
void device_init(void) {
    memset(nodes, 0, sizeof nodes);
    memset(used, 0, sizeof used);
    memset(watchers, 0, sizeof watchers);
    memset(polls, 0, sizeof polls);
    node_count = poll_count = 0;
    current_generation = 1;
    error_text[0] = 0;
}
unsigned device_count(void) {
    return node_count;
}
const ArkDeviceInfo *device_info(uint32_t index) {
    return index < ARK_DEVICE_MAX && used[index] ? &nodes[index] : 0;
}
static bool shape_ok(const ArkDeviceInfo *info) {
    if (!info || info->class_id < ARK_DEV_CLASS_PLATFORM || info->class_id > ARK_DEV_CLASS_PCI ||
        info->bus > ARK_BUS_VIRTUAL || info->state > ARK_DEV_STATE_ABSENT ||
        (info->flags & ~(ARK_DEV_PRESENT | ARK_DEV_READABLE | ARK_DEV_WRITABLE |
                         ARK_DEV_SYSTEM_VOLUME | ARK_DEV_REMOVABLE | ARK_DEV_MODULE)))
        return false;
    return terminated(info->name, sizeof info->name) && terminated(info->driver, sizeof info->driver) &&
           terminated(info->detail, sizeof info->detail);
}
int device_register(const ArkDeviceInfo *info) {
    if (!shape_ok(info)) {
        set_error("invalid device descriptor");
        return -22;
    }
    /* A driver re-probing the same hardware updates its node instead of
     * consuming another slot, so inventory stays one entry per real device. */
    for (unsigned i = 0; i < ARK_DEVICE_MAX; i++)
        if (used[i] && nodes[i].class_id == info->class_id && nodes[i].bus == info->bus &&
            nodes[i].bdf == info->bdf && nodes[i].vendor == info->vendor &&
            nodes[i].device == info->device && nodes[i].device_class == info->device_class &&
            nodes[i].unit == info->unit) {
            uint32_t generation = nodes[i].generation, rx = nodes[i].rx_bytes,
                     tx = nodes[i].tx_bytes, ops = nodes[i].ops, errors = nodes[i].errors;
            nodes[i] = *info;
            nodes[i].index = i;
            nodes[i].generation = generation;
            nodes[i].rx_bytes = rx;
            nodes[i].tx_bytes = tx;
            nodes[i].ops = ops;
            nodes[i].errors = errors;
            return (int)i;
        }
    for (unsigned i = 0; i < ARK_DEVICE_MAX; i++)
        if (!used[i]) {
            nodes[i] = *info;
            nodes[i].index = i;
            nodes[i].generation = 0;
            used[i] = true;
            node_count++;
            set_error("");
            return (int)i;
        }
    set_error("device table full");
    return -12;
}
bool device_set_state(uint32_t index, uint32_t flags, uint32_t state) {
    if (index >= ARK_DEVICE_MAX || !used[index] ||
        (flags & ~(ARK_DEV_PRESENT | ARK_DEV_READABLE | ARK_DEV_WRITABLE | ARK_DEV_SYSTEM_VOLUME |
                   ARK_DEV_REMOVABLE | ARK_DEV_MODULE)) ||
        state > ARK_DEV_STATE_ABSENT)
        return false;
    bool changed = nodes[index].flags != flags || nodes[index].state != state;
    nodes[index].flags = flags;
    nodes[index].state = state;
    if (changed)
        return device_notify(index);
    return true;
}
bool device_add_counters(uint32_t index, uint64_t rx, uint64_t tx, uint64_t ops, uint64_t errors) {
    if (index >= ARK_DEVICE_MAX || !used[index])
        return false;
    nodes[index].rx_bytes += rx;
    nodes[index].tx_bytes += tx;
    nodes[index].ops += ops;
    nodes[index].errors += errors;
    return true;
}
bool device_register_poll(void (*poll)(void)) {
    if (!poll || poll_count >= ARK_DEVICE_POLL_MAX) {
        set_error("device poll slots exhausted");
        return false;
    }
    polls[poll_count++] = poll;
    return true;
}
void device_service_poll(void) {
    for (unsigned i = 0; i < poll_count; i++)
        if (polls[i])
            polls[i]();
}
uint32_t device_generation(void) {
    return current_generation;
}
bool device_notify(uint32_t index) {
    if (index >= ARK_DEVICE_MAX || !used[index])
        return false;
    nodes[index].generation++;
    current_generation++;
    /* A wake means "some inventory changed": the woken process re-reads
     * ArkDeviceInfo and compares the per-node generation it remembers. */
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (watchers[i].noted && watchers[i].seen != current_generation)
            process_wake(watchers[i].pid);
    return true;
}
void device_note(uint32_t pid) {
    if (!pid)
        return;
    uint32_t slot = PROCESS_MAX;
    for (uint32_t i = 0; i < PROCESS_MAX; i++)
        if (watchers[i].pid == pid) {
            watchers[i].seen = current_generation;
            watchers[i].noted = true;
            return;
        } else if (!watchers[i].noted && slot == PROCESS_MAX)
            slot = i;
    if (slot < PROCESS_MAX) {
        watchers[slot].pid = pid;
        watchers[slot].seen = current_generation;
        watchers[slot].noted = true;
    }
}
bool device_pending(uint32_t pid) {
    for (uint32_t i = 0; i < PROCESS_MAX; i++)
        if (watchers[i].noted && watchers[i].pid == pid)
            return watchers[i].seen != current_generation;
    return false;
}
void device_forget_pid(uint32_t pid) {
    for (uint32_t i = 0; i < PROCESS_MAX; i++)
        if (watchers[i].pid == pid)
            memset(&watchers[i], 0, sizeof watchers[i]);
}
void device_session_reset(void) {
    memset(watchers, 0, sizeof watchers);
    memset(readers, 0, sizeof readers);
}
static uint32_t reader_slot(uint32_t pid) {
    uint32_t free_slot = PROCESS_MAX;
    for (uint32_t i = 0; i < PROCESS_MAX; i++)
        if (readers[i].pid == pid)
            return i;
        else if (!readers[i].pid && free_slot == PROCESS_MAX)
            free_slot = i;
    return free_slot;
}
bool device_read_allowed(uint32_t pid, uint64_t now_ms) {
    uint32_t slot = reader_slot(pid);
    if (slot >= PROCESS_MAX)
        return false;
    if (!readers[slot].pid || now_ms - readers[slot].window >= ARK_DEVICE_READ_WINDOW_MS) {
        readers[slot].pid = pid;
        readers[slot].window = now_ms;
        readers[slot].count = 0;
    }
    if (readers[slot].count >= ARK_DEVICE_READ_BURST) {
        set_error("device read budget exhausted; retry later");
        return false;
    }
    readers[slot].count++;
    return true;
}
static bool readable_block(uint32_t index) {
    const ArkDeviceInfo *n = device_info(index);
    return n && n->class_id == ARK_DEV_CLASS_BLOCK && n->unit < 2 &&
           (n->flags & (ARK_DEV_PRESENT | ARK_DEV_READABLE)) ==
               (ARK_DEV_PRESENT | ARK_DEV_READABLE) &&
           !(n->flags & ARK_DEV_SYSTEM_VOLUME);
}
bool device_block_read(uint32_t index, uint64_t lba, uint32_t sectors, void *buffer) {
    if (!readable_block(index) || !sectors || !buffer) {
        set_error("device is not a readable block volume");
        return false;
    }
    const ArkDeviceInfo *n = device_info(index);
    if (n->blocks && (lba >= n->blocks || sectors > n->blocks - lba)) {
        set_error("block request out of range");
        return false;
    }
    if (!block_read(block_device(n->unit), lba, sectors, buffer)) {
        set_error(block_error());
        return false;
    }
    set_error("");
    return true;
}
bool device_block_write(uint32_t index, uint64_t lba, uint32_t sectors, const void *buffer) {
    const ArkDeviceInfo *n = device_info(index);
    if (!n || n->class_id != ARK_DEV_CLASS_BLOCK || n->unit >= 2 ||
        (n->flags & (ARK_DEV_PRESENT | ARK_DEV_WRITABLE)) != (ARK_DEV_PRESENT | ARK_DEV_WRITABLE) ||
        (n->flags & ARK_DEV_SYSTEM_VOLUME)) {
        set_error("device is not a writable block volume");
        return false;
    }
    if (!sectors || !buffer) {
        set_error("block request out of range");
        return false;
    }
    if (n->blocks && (lba >= n->blocks || sectors > n->blocks - lba)) {
        set_error("block request out of range");
        return false;
    }
    if (!block_write(block_device(n->unit), lba, sectors, buffer)) {
        set_error(block_error());
        return false;
    }
    set_error("");
    return true;
}
bool device_block_flush(uint32_t index) {
    const ArkDeviceInfo *n = device_info(index);
    if (!n || n->class_id != ARK_DEV_CLASS_BLOCK || n->unit >= 2 ||
        (n->flags & (ARK_DEV_PRESENT | ARK_DEV_WRITABLE)) != (ARK_DEV_PRESENT | ARK_DEV_WRITABLE)) {
        set_error("device is not a writable block volume");
        return false;
    }
    if (!block_flush(block_device(n->unit))) {
        set_error(block_error());
        return false;
    }
    set_error("");
    return true;
}