#ifndef ARK_DEVICE_H
#define ARK_DEVICE_H
#include "ark.h"
#include "ark_api.h"
/* Kernel device model. Every driver registers one bounded node per real device
 * so userspace inventory, statistics and controlled reads have a single source
 * of truth. Registration order is boot discovery order; indexes are stable for
 * the lifetime of the boot and are the only identifier userspace may use. */
#define ARK_DEVICE_MAX 64u
#define ARK_DEVICE_POLL_MAX 8u
#define ARK_DEVICE_READ_BURST 16u
#define ARK_DEVICE_READ_WINDOW_MS 100u
/* ARK_BUS_* lives in ark_api.h: ArkDeviceInfo.bus is public ABI and a .arco
 * driver fills the same descriptor through its host table. */
void device_init(void);
/* Returns the stable node index, or a negative error. Registration never
 * allocates and never blocks: a full table fails the individual driver. */
int device_register(const ArkDeviceInfo *info);
bool device_set_state(uint32_t index, uint32_t flags, uint32_t state);
bool device_add_counters(uint32_t index, uint64_t rx, uint64_t tx, uint64_t ops, uint64_t errors);
const ArkDeviceInfo *device_info(uint32_t index);
unsigned device_count(void);
const char *device_error(void);
/* Bounded driver polling, run from process_service_poll at the same cadence as
 * the network poller. Slots are a fixed table; drivers must not block here. */
bool device_register_poll(void (*poll)(void));
void device_service_poll(void);
/* Advances the global generation and wakes processes that observed an older one
 * through the existing event path. Comparison is inequality, never ordering. */
bool device_notify(uint32_t index);
uint32_t device_generation(void);
/* Event/wait bridge: a process is only eligible after it has observed the
 * current generation at least once, so unrelated device traffic never wakes an
 * application that never asked for device inventory. */
void device_note(uint32_t pid);
bool device_pending(uint32_t pid);
void device_forget_pid(uint32_t pid);
void device_session_reset(void);
/* Per-process block read budget so a granted application cannot turn the device
 * capability into an unpaced data path. Returns false when the window is spent. */
bool device_read_allowed(uint32_t pid, uint64_t now_ms);
/* Read-only sector access used by SYS_DEVICE. Rejects absent, unreadable and
 * ArkFS system volumes; the caller still validates the user buffer bounds. */
bool device_block_read(uint32_t index, uint64_t lba, uint32_t sectors, void *buffer);
bool device_block_flush(uint32_t index);
/* Boot-time inventory built from every driver's real status. Call once after the
 * last driver has probed hardware and before the desktop is started. */
void devices_init(void);
#endif