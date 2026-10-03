#ifndef ARK_PROCESS_H
#define ARK_PROCESS_H
#include "ark.h"

#define PROCESS_MAX 32u
#define PROCESS_USER_BASE 0x40000000ull
#define PROCESS_USER_LIMIT 0x80000000ull
#define PROCESS_CAP_SYSTEM 1ull
#define PROCESS_CAP_FILES 2ull
#define PROCESS_CAP_UI 4ull
#define PROCESS_CAP_NETWORK 8ull
#define PROCESS_CAP_PROCESS 16ull

/* Exact stack layout produced by interrupts.S, including the long-mode
 * five-word hardware return frame (also present on same-CPL interrupts). */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error, rip, cs, rflags, rsp, ss;
} InterruptFrame;
enum {
    PROCESS_FREE = 0,
    PROCESS_READY = 1,
    PROCESS_RUNNING = 2,
    PROCESS_DEAD = 3,
    PROCESS_SLEEPING = 4,
    PROCESS_WAITING = 5
};
typedef struct {
    uint32_t pid, uid, state;
    int32_t exit_status;
    uint64_t capabilities, mapped_bytes, cpu_ticks, switches;
    uint32_t group_pid, thread;
    char name[32];
} ProcessInfo;

/* Initialize AFTER all driver/MMIO mappings are installed. Requires NX and
 * enough contiguous RAM for the kernel's 256 MiB physical-page pool. */
bool process_init(void);
void *process_kernel_alloc(size_t bytes);
void process_kernel_free(void *pointer, size_t bytes);
int process_spawn_elf(const void *image, size_t bytes, const char *name, uint32_t uid,
                      uint64_t caps);
__attribute__((noreturn)) void process_run(void);
uint32_t process_current_pid(void);
uint32_t process_current_uid(void);
uint64_t process_current_caps(void);
bool process_has_cap(uint64_t mask);
void process_record_io(bool write, uint64_t bytes);
bool process_set_current_uid(uint32_t uid);
void process_update_caps(const char *name, uint32_t uid, uint64_t caps);
bool process_get_info(uint32_t pid, ProcessInfo *out);
size_t process_list(ProcessInfo *out, size_t capacity);
/* Kernel-side session revocation: current SYSTEM process can kill a task or
 * revoke all non-SYSTEM tasks. Exit notification releases driver-owned state. */
bool process_kill(uint32_t pid, int status);
void process_wake(uint32_t pid);
unsigned process_revoke_user_tasks(void);

/* Pointers are in the CURRENT process. Every page is checked, lengths are
 * overflow checked and bounded to 64 MiB. Copies use supervisor physical
 * aliases after validating user access; no speculative raw user dereference. */
bool process_user_range(uint64_t address, size_t bytes, bool write);
bool process_copy_from_user(void *dst, uint64_t src, size_t bytes);
bool process_copy_to_user(uint64_t dst, const void *src, size_t bytes);
/* Synchronous CPU read only. Caller MUST NOT retain alias or hand it to DMA.
 * The global kernel lock pins mapping topology for the synchronous call.
 * User pixel contents may change concurrently; no address is retained or DMAed. */
const void *process_read_alias(uint64_t address, size_t bytes);

/* Platform interrupt glue. Caller has acknowledged hardware IRQs first. */
InterruptFrame *process_on_interrupt(InterruptFrame *frame);
void platform_enable_process_gates(void);
void platform_load_process_idt(void);
void process_kernel_enter(void);
void process_kernel_leave(void);
__attribute__((noreturn)) void process_ap_run(void);
unsigned platform_current_cpu(void);
bool platform_ram_range(uint64_t physical, uint64_t bytes);

/* Services override these weak hooks; both execute in kernel context. */
int64_t process_syscall_dispatch(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6);
void process_exit_notify(uint32_t pid);
/* Optional bounded driver poll. Runs only at outer user interrupt/syscall
 * boundaries; ring-0 interrupts do not reenter it. Must not block. */
void process_service_poll(void);
#endif
