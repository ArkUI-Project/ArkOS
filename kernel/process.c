/* Original ArkOS process boundary: ring 3, separate CR3 roots, 4 KiB user
 * pages, W^X/NX and a bounded int80 ABI. This is not a Linux ABI. */
#include "process.h"
#include "elf.h"
#define ARK_KERNEL
#include "ark_api.h"

#define PAGE_SIZE 4096ull
#define PAGE_MASK 0x000ffffffffff000ull
#define PTE_PRESENT 1ull
#define PTE_WRITE 2ull
#define PTE_USER 4ull
#define PTE_LARGE 128ull
#define PTE_NX (1ull << 63)
#define POOL_BYTES (896u * 1024u * 1024u)
#define POOL_PAGES (POOL_BYTES / 4096u)
#define KSTACK_BYTES (64u * 1024u)
#define USER_STACK_BYTES (128u * 1024u)
#define USER_STACK_TOP (PROCESS_USER_LIMIT - 8192ull)
#define COPY_LIMIT (64u * 1024u * 1024u)
#define INBOX_CAP 16u
#define QUANTUM_TICKS 2u
#define ERR_PERM 1
#define ERR_NOENT 2
#define ERR_BAD_ELF 8
#define ERR_AGAIN 11
#define ERR_NOMEM 12
#define ERR_FAULT 14
#define ERR_INVAL 22
#define ERR_NOSYS 38

/* Allocate physical pages on demand instead of putting 256 MiB in the ELF
 * BSS (which firmware and entry would both zero at every boot). */
#define POOL_BASE (128ull * 1024 * 1024)
static uint8_t *const page_pool = (uint8_t *)(uintptr_t)POOL_BASE;
extern uint8_t __kernel_end[];
static uint8_t initial_fx[512] __attribute__((aligned(16)));
static uint8_t page_owner[POOL_PAGES];
static unsigned free_hint, pool_pages, free_pages, kernel_pages;
static bool vm_reclaim_one(void), vm_resolve(unsigned, uint64_t, bool);
static void vm_release_owner(unsigned);
static void vm_performance(ArkPerformanceInfo *);
extern void process_performance_devices(ArkPerformanceInfo *) __attribute__((weak));
static uint64_t kernel_cr3;
static bool initialized;
uint64_t process_kernel_cr3(void) {
    return kernel_cr3;
}
static uint32_t next_pid = 1;
#define CPU_LIMIT 8u
__attribute__((weak)) unsigned platform_current_cpu(void) {
    return 0;
}
extern void smp_start_users(void) __attribute__((weak));
extern void smp_local_timer(void) __attribute__((weak));
extern void smp_wake_cpu(unsigned) __attribute__((weak));
static uint32_t kernel_lock;
static bool runtime;
static struct {
    int task;
    unsigned depth;
    bool ready;
    uint64_t deadline;
    uint8_t idle_stack[32768] __attribute__((aligned(16)));
    InterruptFrame idle;
} cpu_state[CPU_LIMIT];
static uint64_t cpu_tick_last[CPU_LIMIT];
static uint64_t cpu_account_at[CPU_LIMIT], cpu_busy_ms[CPU_LIMIT], cpu_total_ms[CPU_LIMIT];
#define current (cpu_state[platform_current_cpu()].task)
#define quantum_end (cpu_state[platform_current_cpu()].deadline)
extern volatile unsigned ark_kernel_panicked __attribute__((weak));
static void panic_stop(void) {
    if (&ark_kernel_panicked && __atomic_load_n(&ark_kernel_panicked, __ATOMIC_ACQUIRE))
        for (;;)
            __asm__ volatile("cli;hlt");
}
void process_kernel_enter(void) {
    panic_stop();
    if (!__atomic_load_n(&runtime, __ATOMIC_ACQUIRE))
        return;
    unsigned cpu = platform_current_cpu();
    if (cpu_state[cpu].depth++) {
        return;
    }
    while (__atomic_exchange_n(&kernel_lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&kernel_lock, __ATOMIC_RELAXED)) {
            panic_stop();
            __asm__ volatile("pause");
        }
}
void process_kernel_leave(void) {
    unsigned cpu = platform_current_cpu();
    if (!cpu_state[cpu].depth)
        return;
    if (!--cpu_state[cpu].depth)
        __atomic_store_n(&kernel_lock, 0, __ATOMIC_RELEASE);
}
static bool slot_active(unsigned slot) {
    for (unsigned c = 0; c < CPU_LIMIT; c++)
        if (cpu_state[c].task == (int)slot)
            return true;
    return false;
}

static struct {
    ProcessInfo info;
    uint64_t cr3, wake_at;
    bool message_wait;
    unsigned owner;
    uint8_t fx[512] __attribute__((aligned(16)));
    InterruptFrame *frame;
    ArkMessage inbox[INBOX_CAP];
    unsigned inbox_first, inbox_count;
    uint64_t read_bytes, write_bytes, messages_sent, messages_received;
    uint8_t stack[KSTACK_BYTES] __attribute__((aligned(16)));
} tasks[PROCESS_MAX];
static void account_cpu(void) {
    unsigned cpu = platform_current_cpu();
    uint64_t now = platform_millis();
    if (cpu_account_at[cpu]) {
        uint64_t elapsed = now - cpu_account_at[cpu];
        cpu_total_ms[cpu] += elapsed;
        if (current >= 0)
            cpu_busy_ms[cpu] += elapsed;
    }
    cpu_account_at[cpu] = now;
}
static int64_t performance_call(uint64_t addr, uint64_t bytes) {
    if (!process_has_cap(ARK_CAP_SYSTEM))
        return -ERR_PERM;
    if ((bytes != sizeof(ArkPerformanceInfo) &&
         bytes != offsetof(ArkPerformanceInfo, reserved_bytes)) ||
        !process_user_range(addr, bytes, true))
        return -ERR_FAULT;
    account_cpu();
    ArkPerformanceInfo info = {.millis = platform_millis(),
                               .pool_bytes = (uint64_t)pool_pages * 4096};
    info.free_bytes = (uint64_t)free_pages * 4096;
    info.kernel_bytes = (uint64_t)kernel_pages * 4096;
    info.user_bytes = info.pool_bytes - info.free_bytes - info.kernel_bytes;
    for (unsigned i = 0; i < CPU_LIMIT; i++)
        if (cpu_state[i].ready) {
            info.online_cpus++;
            info.cpu_busy_ms[i] = cpu_busy_ms[i];
            info.cpu_total_ms[i] = cpu_total_ms[i];
        }
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].info.state != PROCESS_FREE && tasks[i].info.state != PROCESS_DEAD)
            info.tasks++;
    vm_performance(&info);
    if (process_performance_devices)
        process_performance_devices(&info);
    return process_copy_to_user(addr, &info, bytes) ? 0 : -ERR_FAULT;
}

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp[3], reserved1, ist[7], reserved2;
    uint16_t reserved3, iomap;
} TSS64;
_Static_assert(sizeof(TSS64) == 104, "TSS hardware layout");
_Static_assert(offsetof(TSS64, rsp) == 4, "TSS RSP0 offset");
_Static_assert(offsetof(TSS64, ist) == 36, "TSS IST1 offset");
_Static_assert(sizeof(InterruptFrame) == 176, "interrupt assembly ABI");
static TSS64 per_tss[CPU_LIMIT];
static uint64_t per_gdt[CPU_LIMIT][7] __attribute__((aligned(16)));
static uint8_t per_fault_stack[CPU_LIMIT][32768] __attribute__((aligned(16)));
#define tss (per_tss[platform_current_cpu()])
#define gdt (per_gdt[platform_current_cpu()])
#define double_fault_stack (per_fault_stack[platform_current_cpu()])
extern __attribute__((noreturn)) void process_enter_frame(InterruptFrame *);

static uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq;popq %0;cli" : "=r"(f)::"memory");
    return f;
}
static void irq_restore(uint64_t f) {
    if (f & 512)
        __asm__ volatile("sti" ::: "memory");
}
static void load_cr3(uint64_t p) {
    __asm__ volatile("mov %0,%%cr3" ::"r"(p) : "memory");
}
static void log_num(uint64_t n) {
    char b[24];
    uint_to_str(n, b);
    serial_write(b);
}
static void log_hex(uint64_t n) {
    char b[19];
    const char *hex = "0123456789abcdef";
    b[0] = '0';
    b[1] = 'x';
    for (unsigned i = 0; i < 16; i++)
        b[2 + i] = hex[(n >> (60 - i * 4)) & 15];
    b[18] = 0;
    serial_write(b);
}
static void zero_page(void *p) {
    size_t n = 512;
    __asm__ volatile("cld;rep stosq" : "+D"(p), "+c"(n) : "a"(0ull) : "memory");
}
static uint64_t alloc_page(unsigned slot) {
    if (!free_pages && !vm_reclaim_one())
        return 0;
    for (unsigned checked = 0; checked < pool_pages; checked++) {
        unsigned i = (free_hint + checked) % pool_pages;
        if (!page_owner[i]) {
            page_owner[i] = (uint8_t)(slot + 1);
            free_pages--;
            free_hint = (i + 1) % pool_pages;
            void *p = page_pool + (size_t)i * 4096;
            zero_page(p);
            return (uint64_t)(uintptr_t)p;
        }
    }
    return 0;
}
static void release_pages(unsigned slot) {
    vm_release_owner(slot);
    for (unsigned i = 0; i < pool_pages; i++)
        if (page_owner[i] == slot + 1) {
            page_owner[i] = 0;
            free_pages++;
        }
    tasks[slot].cr3 = 0;
    tasks[slot].info.mapped_bytes = 0;
}
static uint64_t *table_at(uint64_t entry) {
    return (uint64_t *)(uintptr_t)(entry & PAGE_MASK);
}
static uint64_t *user_leaf(unsigned slot, uint64_t va, bool create) {
    if (va < PROCESS_USER_BASE || va >= PROCESS_USER_LIMIT || !tasks[slot].cr3)
        return 0;
    uint64_t *table = table_at(tasks[slot].cr3);
    for (unsigned shift = 39; shift > 12; shift -= 9) {
        uint64_t *e = &table[(va >> shift) & 511];
        if (!(*e & PTE_PRESENT)) {
            if (!create)
                return 0;
            uint64_t page = alloc_page(slot);
            if (!page)
                return 0;
            *e = page | 7;
        }
        if ((*e & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) || (*e & PTE_LARGE))
            return 0;
        table = table_at(*e);
    }
    return &table[(va >> 12) & 511];
}
static bool map_page(unsigned slot, uint64_t va, uint64_t flags) {
    uint64_t *leaf = user_leaf(slot, va, true);
    if (!leaf || (*leaf & PTE_PRESENT))
        return false;
    uint64_t page = alloc_page(slot);
    if (!page)
        return false;
    *leaf = page | PTE_PRESENT | PTE_USER | flags;
    tasks[slot].info.mapped_bytes += PAGE_SIZE;
    return true;
}
void *process_kernel_alloc(size_t bytes) {
    if (!initialized || !bytes || bytes > 64u * 1024 * 1024)
        return 0;
    unsigned count = (unsigned)((bytes + 4095) / 4096), run = 0;
    for (unsigned i = 0; i < pool_pages; i++) {
        if (page_owner[i])
            run = 0;
        else
            run++;
        if (run == count) {
            unsigned start = i + 1 - count;
            for (unsigned j = start; j <= i; j++)
                page_owner[j] = 255;
            free_pages -= count;
            kernel_pages += count;
            void *p = page_pool + (size_t)start * 4096;
            memset(p, 0, (size_t)count * 4096);
            return p;
        }
    }
    return 0;
}
void process_kernel_free(void *pointer, size_t bytes) {
    /* Supervisor buffers only: these pages never enter a user page table. */
    uintptr_t at = (uintptr_t)pointer;
    if (!bytes || at < POOL_BASE || at % 4096)
        return;
    size_t first = (at - POOL_BASE) / 4096, count = (bytes + 4095) / 4096;
    if (first >= pool_pages || count > pool_pages - first)
        return;
    for (size_t i = first; i < first + count; i++)
        if (page_owner[i] != 255)
            return;
    memset(pointer, 0, count * 4096);
    for (size_t i = first; i < first + count; i++)
        page_owner[i] = 0;
    free_pages += (unsigned)count;
    kernel_pages -= (unsigned)count;
    if (first < free_hint)
        free_hint = (unsigned)first;
}
#include "process_vm.inc"
static int64_t memory_call(uint64_t bytes) {
    if (!bytes || bytes > 64u * 1024 * 1024)
        return -ERR_INVAL;
    unsigned owner = tasks[current].owner;
    int64_t at = vm_reserve(owner, bytes);
    if (at < 0)
        return at;
    ArkVMRequest r = {.op = ARK_VM_COMMIT,
                      .flags = ARK_VM_READ | ARK_VM_WRITE,
                      .address = (uint64_t)at,
                      .bytes = (bytes + 4095) & ~4095ull};
    int64_t result = vm_operation(owner, &r);
    if (result < 0) {
        r.op = ARK_VM_RELEASE;
        (void)vm_operation(owner, &r);
        return result;
    }
    return at;
}
static bool new_space(unsigned slot) {
    uint64_t root = alloc_page(slot), middle = alloc_page(slot), directory = alloc_page(slot);
    if (!root || !middle || !directory)
        return false;
    memcpy((void *)(uintptr_t)root, (void *)(uintptr_t)kernel_cr3, 4096);
    uint64_t *pml4 = table_at(root), *pdpt = table_at(middle);
    memcpy(pdpt, table_at(pml4[0]), 4096);
    /* Only this one 1 GiB virtual window may contain user mappings. All existing
     * kernel and PCI mappings remain supervisor-only at their lower levels. */
    pml4[0] = middle | 7;
    pdpt[1] = directory | 7;
    tasks[slot].cr3 = root;
    return true;
}
static bool user_translate(unsigned slot, uint64_t va, bool write, uint64_t *physical) {
    if (va < PROCESS_USER_BASE || va >= PROCESS_USER_LIMIT || !tasks[slot].cr3)
        return false;
    unsigned owner = tasks[slot].owner;
    uint64_t *leaf = user_leaf(owner, va, false);
    if (leaf && (*leaf & VM_ANON) && !(*leaf & PTE_PRESENT) && !vm_resolve(owner, va, write))
        return false;
    uint64_t *table = table_at(tasks[slot].cr3);
    for (unsigned shift = 39;; shift -= 9) {
        uint64_t e = table[(va >> shift) & 511];
        if ((e & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
            (write && !(e & PTE_WRITE)))
            return false;
        if (shift == 12) {
            *physical = (e & PAGE_MASK) | (va & 4095);
            return true;
        }
        if (e & PTE_LARGE)
            return false;
        table = table_at(e);
    }
}
static bool range_for(unsigned slot, uint64_t address, size_t bytes, bool write) {
    if (bytes > COPY_LIMIT || address < PROCESS_USER_BASE || address >= PROCESS_USER_LIMIT ||
        bytes > PROCESS_USER_LIMIT - address)
        return false;
    if (!bytes)
        return true;
    uint64_t last = address + bytes - 1, p;
    for (uint64_t page = address & ~4095ull;; page += 4096) {
        if (!user_translate(slot, page, write, &p))
            return false;
        if (page >= (last & ~4095ull))
            return true;
    }
}
bool process_user_range(uint64_t address, size_t bytes, bool write) {
    return current >= 0 && range_for((unsigned)current, address, bytes, write);
}
extern void smp_copy(void *, const void *, size_t) __attribute__((weak));
bool process_copy_from_user(void *dst, uint64_t src, size_t bytes) {
    if (!process_user_range(src, bytes, false))
        return false;
    uint8_t *out = dst;
    /* Large display transfers often occupy contiguous physical pages. Discover
     * runs only after validating EVERY user PTE; APs see physical aliases only. */
    if (smp_copy && bytes >= 512 * 1024) {
        while (bytes) {
            uint64_t first;
            user_translate((unsigned)current, src, false, &first);
            size_t run = 4096 - (src & 4095);
            if (run > bytes)
                run = bytes;
            while (run < bytes) {
                uint64_t next;
                if (!user_translate((unsigned)current, src + run, false, &next) ||
                    next != first + run)
                    break;
                size_t step = bytes - run > 4096 ? 4096 : bytes - run;
                run += step;
            }
            smp_copy(out, (const void *)(uintptr_t)first, run);
            out += run;
            src += run;
            bytes -= run;
        }
        return true;
    }
    while (bytes) {
        uint64_t p;
        user_translate((unsigned)current, src, false, &p);
        size_t n = 4096 - (src & 4095);
        if (n > bytes)
            n = bytes;
        memcpy(out, (const void *)(uintptr_t)p, n);
        out += n;
        src += n;
        bytes -= n;
    }
    return true;
}
const void *process_read_alias(uint64_t address, size_t bytes) {
    return process_user_range(address, bytes, false) ? (const void *)(uintptr_t)address : 0;
}
bool process_copy_to_user(uint64_t dst, const void *src, size_t bytes) {
    if (!process_user_range(dst, bytes, true))
        return false;
    const uint8_t *in = src;
    while (bytes) {
        uint64_t p;
        user_translate((unsigned)current, dst, true, &p);
        size_t n = 4096 - (dst & 4095);
        if (n > bytes)
            n = bytes;
        memcpy((void *)(uintptr_t)p, in, n);
        in += n;
        dst += n;
        bytes -= n;
    }
    return true;
}
uint32_t process_current_pid(void) {
    return current < 0 ? 0 : tasks[tasks[current].owner].info.pid;
}
void process_record_io(bool write, uint64_t bytes) {
    if (current >= 0) {
        unsigned owner = tasks[current].owner;
        if (write)
            tasks[owner].write_bytes += bytes;
        else
            tasks[owner].read_bytes += bytes;
    }
}
uint32_t process_current_uid(void) {
    return current < 0 ? UINT32_MAX : tasks[current].info.uid;
}
uint64_t process_current_caps(void) {
    return current < 0 ? 0 : tasks[current].info.capabilities;
}
bool process_has_cap(uint64_t mask) {
    uint64_t c = process_current_caps();
    return (c & PROCESS_CAP_SYSTEM) || (c & mask) == mask;
}
bool process_set_current_uid(uint32_t uid) {
    if (current < 0 || !process_has_cap(PROCESS_CAP_SYSTEM))
        return false;
    tasks[current].info.uid = uid;
    return true;
}
bool process_get_info(uint32_t pid, ProcessInfo *out) {
    if (!out || !pid)
        return false;
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].info.state && tasks[i].info.pid == pid) {
            *out = tasks[i].info;
            return true;
        }
    return false;
}
size_t process_list(ProcessInfo *out, size_t capacity) {
    size_t n = 0;
    if (!out)
        return 0;
    for (unsigned i = 0; i < PROCESS_MAX && n < capacity; i++)
        if (tasks[i].info.state)
            out[n++] = tasks[i].info;
    return n;
}
__attribute__((weak)) int64_t process_syscall_dispatch(uint64_t n, uint64_t a, uint64_t b,
                                                       uint64_t c, uint64_t d, uint64_t e,
                                                       uint64_t f) {
    (void)n;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    return -ERR_NOSYS;
}
__attribute__((weak)) void process_exit_notify(uint32_t pid) {
    (void)pid;
}
__attribute__((weak)) void process_service_poll(void) {
}

static void process_cpu_init(void) {
    uint32_t a, d;
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 = (cr0 | (1ull << 16) | 2 | 32) & ~12ull;
    __asm__ volatile("mov %0,%%cr0" ::"r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    cr4 = (cr4 | (1ull << 9) | (1ull << 10)) & ~((1ull << 16) | (1ull << 18));
    __asm__ volatile("mov %0,%%cr4" ::"r"(cr4) : "memory");
    a = 0;
    d = 0;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(0xc0000080));
    a |= 1u << 11;
    __asm__ volatile("wrmsr" ::"a"(a), "d"(d), "c"(0xc0000080) : "memory");
    memset(&tss, 0, sizeof tss);
    tss.ist[0] = (uint64_t)(uintptr_t)(double_fault_stack + sizeof double_fault_stack);
    tss.iomap = sizeof tss;
    gdt[0] = 0;
    gdt[1] = 0x00af9a000000ffffull;
    gdt[2] = 0x00cf92000000ffffull;
    gdt[3] = 0x00affa000000ffffull;
    gdt[4] = 0x00cff2000000ffffull;
    uint64_t base = (uint64_t)(uintptr_t)&tss, limit = sizeof tss - 1;
    gdt[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ull << 40) |
             (((limit >> 16) & 15) << 48) | (((base >> 24) & 255) << 56);
    gdt[6] = base >> 32;
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } gdtr = {sizeof(gdt) - 1, (uint64_t)(uintptr_t)gdt};
    __asm__ volatile("lgdt %0" ::"m"(gdtr) : "memory");
    uint16_t selector = 0x28;
    __asm__ volatile("ltr %0" ::"r"(selector) : "memory");
    cpu_state[platform_current_cpu()].ready = true;
}
bool process_init(void) {
    uint64_t irq = irq_save();
    if (initialized) {
        irq_restore(irq);
        return true;
    }
    for (unsigned i = 0; i < CPU_LIMIT; i++)
        cpu_state[i].task = -1;
    uint32_t a, b, c, d;
    a = 0x80000000;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (a < 0x80000001) {
        serial_write("[process] NX-capable CPU required\n");
        irq_restore(irq);
        return false;
    }
    a = 0x80000001;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (!(d & (1u << 20))) {
        serial_write("[process] NX-capable CPU required\n");
        irq_restore(irq);
        return false;
    }
    uint64_t pool = (uint64_t)(uintptr_t)page_pool;
    pool_pages = POOL_PAGES;
    while (pool_pages >= 256u * 1024 * 1024 / 4096 &&
           !platform_ram_range(pool, (uint64_t)pool_pages * 4096))
        pool_pages -= 32u * 1024 * 1024 / 4096;
    if (pool + (uint64_t)pool_pages * 4096 > PROCESS_USER_BASE ||
        (uint64_t)(uintptr_t)__kernel_end > pool || pool_pages < 256u * 1024 * 1024 / 4096) {
        serial_write("[process] Insufficient contiguous RAM; use QEMU -m 512M or larger\n");
        irq_restore(irq);
        return false;
    }
    __asm__ volatile("mov %%cr3,%0" : "=r"(kernel_cr3));
    kernel_cr3 &= PAGE_MASK;
    free_pages = pool_pages;
    /* Kernel code remains general-register-only. Each task owns a complete
     * x87/MMX/SSE context; AVX is not enabled until XSAVE support exists. */
    a = 1;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if ((d & ((1u << 24) | (1u << 25) | (1u << 26))) != ((1u << 24) | (1u << 25) | (1u << 26))) {
        irq_restore(irq);
        return false;
    }
    process_cpu_init();
    memset(initial_fx, 0, sizeof initial_fx);
    *(uint16_t *)initial_fx = 0x37f;
    *(uint32_t *)(void *)(initial_fx + 24) = 0x1f80;
    platform_enable_process_gates();
    initialized = true;
    serial_write("[process] Ring3 enabled: per-process CR3, 4KiB W^X/NX pages, WP, TSS, int80; "
                 "isolated x87/SSE2 state\n");
    irq_restore(irq);
    return true;
}

int process_spawn_elf(const void *image, size_t bytes, const char *name, uint32_t uid,
                      uint64_t caps) {
    if (!initialized || !process_validate_elf(image, bytes, caps))
        return -ERR_BAD_ELF;
    const ELFHeader *h = image;
    const ELFProgram *ph = (const ELFProgram *)((const uint8_t *)image + h->phoff);
    uint64_t irq = irq_save();
    int slot = -1;
    if (next_pid > INT32_MAX) {
        irq_restore(irq);
        return -ERR_AGAIN;
    }
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if ((tasks[i].info.state == PROCESS_FREE ||
             (tasks[i].info.state == PROCESS_DEAD && !tasks[i].cr3)) &&
            !slot_active(i)) {
            slot = (int)i;
            break;
        }
    if (slot < 0) {
        irq_restore(irq);
        return -ERR_AGAIN;
    }
    release_pages((unsigned)slot);
    memset(&tasks[slot].info, 0, sizeof tasks[slot].info);
    tasks[slot].inbox_first = tasks[slot].inbox_count = 0;
    tasks[slot].read_bytes = tasks[slot].write_bytes = tasks[slot].messages_sent =
        tasks[slot].messages_received = 0;
    tasks[slot].owner = (unsigned)slot;
    tasks[slot].wake_at = 0;
    memcpy(tasks[slot].fx, initial_fx, 512);
    if (!new_space((unsigned)slot))
        goto no_memory;
    for (unsigned i = 0; i < h->phnum; i++)
        if (ph[i].type == 1 && ph[i].memsz) {
            const ELFProgram *p = &ph[i];
            uint64_t flags = (p->flags & 2) ? PTE_WRITE : 0;
            if (!(p->flags & 1))
                flags |= PTE_NX;
            for (uint64_t va = p->vaddr; va < p->vaddr + p->memsz; va += 4096)
                if (!map_page((unsigned)slot, va, flags))
                    goto no_memory;
            uint64_t va = p->vaddr;
            size_t remaining = p->filesz;
            const uint8_t *src = (const uint8_t *)image + p->offset;
            while (remaining) {
                uint64_t *leaf = user_leaf((unsigned)slot, va, false);
                size_t n = remaining > 4096 ? 4096 : remaining;
                memcpy((void *)(uintptr_t)(*leaf & PAGE_MASK), src, n);
                remaining -= n;
                src += n;
                va += n;
            }
        }
    for (uint64_t va = USER_STACK_TOP - USER_STACK_BYTES; va < USER_STACK_TOP; va += 4096)
        if (!map_page((unsigned)slot, va, PTE_WRITE | PTE_NX))
            goto no_memory;
    tasks[slot].info.pid = next_pid++;
    tasks[slot].info.group_pid = tasks[slot].info.pid;
    tasks[slot].info.uid = uid;
    tasks[slot].info.capabilities = caps;
    tasks[slot].info.state = PROCESS_READY;
    strcopy(tasks[slot].info.name, name ? name : "user", sizeof tasks[slot].info.name);
    tasks[slot].frame =
        (InterruptFrame *)(void *)(tasks[slot].stack + KSTACK_BYTES - sizeof(InterruptFrame));
    memset(tasks[slot].frame, 0, sizeof(InterruptFrame));
    tasks[slot].frame->rip = h->entry;
    tasks[slot].frame->cs = 0x1b;
    tasks[slot].frame->ss = 0x23;
    tasks[slot].frame->rsp = USER_STACK_TOP;
    tasks[slot].frame->rflags = 0x202;
    serial_write("[process] Spawn pid=");
    log_num(tasks[slot].info.pid);
    serial_write(" uid=");
    log_num(uid);
    serial_write(" name=");
    serial_write(tasks[slot].info.name);
    serial_write(" bytes=");
    log_num(tasks[slot].info.mapped_bytes);
    serial_write(" CR3=");
    log_hex(tasks[slot].cr3);
    serial_write("\n");
    int pid = (int)tasks[slot].info.pid;
    irq_restore(irq);
    return pid;
no_memory:
    release_pages((unsigned)slot);
    memset(&tasks[slot].info, 0, sizeof tasks[slot].info);
    irq_restore(irq);
    return -ERR_NOMEM;
}

static void mark_dead(unsigned slot, int status) {
    if (tasks[slot].info.state == PROCESS_DEAD || tasks[slot].info.state == PROCESS_FREE)
        return;
    tasks[slot].info.state = PROCESS_DEAD;
    tasks[slot].info.exit_status = status;
    tasks[slot].inbox_count = 0;
    serial_write("[process] Exit pid=");
    log_num(tasks[slot].info.pid);
    serial_write(" status=");
    log_hex((uint64_t)(int64_t)status);
    serial_write("\n");
}
static void group_dead(unsigned owner, int status) {
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].owner == owner && tasks[i].info.state)
            mark_dead(i, status);
    process_exit_notify(tasks[owner].info.pid);
}
static void reclaim_group(unsigned owner) {
    for (unsigned c = 0; c < CPU_LIMIT; c++)
        if (cpu_state[c].task >= 0 && tasks[cpu_state[c].task].owner == owner)
            return;
    release_pages(owner);
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].owner == owner)
            tasks[i].cr3 = 0;
}
bool process_kill(uint32_t pid, int status) {
    if (!process_has_cap(PROCESS_CAP_SYSTEM))
        return false;
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].info.pid == pid && tasks[i].info.state &&
            tasks[i].info.state != PROCESS_DEAD) {
            unsigned owner = tasks[i].owner;
            if ((current >= 0 && owner == tasks[current].owner) ||
                (tasks[owner].info.capabilities & PROCESS_CAP_SYSTEM))
                return false;
            group_dead(owner, status);
            reclaim_group(owner);
            return true;
        }
    return false;
}
unsigned process_revoke_user_tasks(void) {
    if (!process_has_cap(PROCESS_CAP_SYSTEM))
        return 0;
    unsigned count = 0;
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].owner == i && tasks[i].info.state && tasks[i].info.state != PROCESS_DEAD &&
            !(tasks[i].info.capabilities & PROCESS_CAP_SYSTEM)) {
            group_dead(i, -ERR_PERM);
            reclaim_group(i);
            count++;
        }
    return count;
}
static InterruptFrame *activate(unsigned slot) {
    int before = current;
    current = (int)slot;
    tasks[slot].info.state = PROCESS_RUNNING;
    tasks[slot].info.switches++;
    tss.rsp[0] = (uint64_t)(uintptr_t)(tasks[slot].stack + KSTACK_BYTES);
    if (before < 0 || tasks[before].cr3 != tasks[slot].cr3)
        load_cr3(tasks[slot].cr3);
    __asm__ volatile("fxrstor64 %0" ::"m"(tasks[slot].fx) : "memory");
    tasks[slot].frame->rflags =
        (tasks[slot].frame->rflags & ~((3ull << 12) | (1ull << 14) | (1ull << 17))) | 0x202;
    cpu_tick_last[platform_current_cpu()] = platform_ticks();
    quantum_end = platform_ticks() + QUANTUM_TICKS;
    return tasks[slot].frame;
}
static void wake_idle_cpus(void) {
    unsigned here = platform_current_cpu();
    if (smp_wake_cpu)
        for (unsigned cpu = 0; cpu < CPU_LIMIT; cpu++)
            if (cpu != here && cpu_state[cpu].ready && cpu_state[cpu].task < 0)
                smp_wake_cpu(cpu);
}
void process_wake(uint32_t pid) {
    bool woke = false;
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].info.group_pid == pid && tasks[i].info.state == PROCESS_WAITING &&
            !tasks[i].message_wait) {
            tasks[i].info.state = PROCESS_READY;
            woke = true;
        }
    if (woke)
        wake_idle_cpus();
}
static bool wake_tasks(void) {
    uint64_t now = platform_millis();
    bool woke = false;
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if ((tasks[i].info.state == PROCESS_SLEEPING || tasks[i].info.state == PROCESS_WAITING) &&
            now >= tasks[i].wake_at) {
            tasks[i].info.state = PROCESS_READY;
            woke = true;
        }
    if (woke)
        wake_idle_cpus();
    return woke;
}
static __attribute__((noreturn)) void idle_loop(void) {
    for (;;)
        __asm__ volatile("sti;hlt" ::: "memory");
}
static void reap_dead(void) {
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].owner == i && tasks[i].info.state == PROCESS_DEAD && tasks[i].cr3)
            reclaim_group(i);
}
static InterruptFrame *schedule(InterruptFrame *frame, bool idle_if_alone) {
    account_cpu();
    int previous = current;
    if (previous >= 0) {
        tasks[previous].frame = frame;
        __asm__ volatile("fxsave64 %0" : "=m"(tasks[previous].fx)::"memory");
        if (tasks[previous].info.state == PROCESS_RUNNING)
            tasks[previous].info.state = PROCESS_READY;
    }
    wake_tasks();
    for (unsigned offset = 1; offset <= PROCESS_MAX; offset++) {
        unsigned slot = (unsigned)(previous + (int)offset) % PROCESS_MAX;
        if (tasks[slot].info.state != PROCESS_READY)
            continue;
        if ((int)slot == previous && idle_if_alone) {
            tasks[slot].info.state = PROCESS_SLEEPING;
            tasks[slot].wake_at = platform_millis() + 10;
            continue;
        }
        InterruptFrame *next = activate(slot);
        reap_dead();
        return next;
    }
    unsigned cpu = platform_current_cpu();
    current = -1;
    load_cr3(kernel_cr3);
    reap_dead();
    InterruptFrame *idle = &cpu_state[cpu].idle;
    memset(idle, 0, sizeof *idle);
    idle->rip = (uintptr_t)idle_loop;
    idle->cs = 8;
    idle->ss = 16;
    idle->rflags = 0x202;
    idle->rsp = (uintptr_t)(cpu_state[cpu].idle_stack + sizeof cpu_state[cpu].idle_stack) - 8;
    return idle;
}
static bool executable(unsigned owner, uint64_t entry) {
    uint64_t *p = user_leaf(owner, entry, false);
    return p && (*p & PTE_PRESENT) && !(*p & PTE_NX);
}
extern bool process_events_pending(uint32_t) __attribute__((weak));
static int64_t thread_call(uint64_t ptr, uint64_t size, InterruptFrame *frame, bool *reschedule) {
    ArkThreadRequest r;
    if (size != sizeof r)
        return -ERR_INVAL;
    if (!process_copy_from_user(&r, ptr, sizeof r) || !process_user_range(ptr, sizeof r, true))
        return -ERR_FAULT;
    unsigned owner = tasks[current].owner;
    if (r.op == ARK_THREAD_CREATE) {
        if (!executable(owner, r.entry) || !executable(owner, r.trampoline))
            return -ERR_FAULT;
        int slot = -1;
        for (unsigned i = 0; i < PROCESS_MAX; i++)
            if (!slot_active(i) && i != owner &&
                (tasks[i].info.state == PROCESS_FREE ||
                 (tasks[i].info.state == PROCESS_DEAD && !tasks[i].cr3))) {
                slot = (int)i;
                break;
            }
        if (slot < 0 || next_pid > INT32_MAX)
            return -ERR_AGAIN;
        uint64_t top = USER_STACK_TOP - ((uint64_t)slot + 1) * 256 * 1024;
        uint64_t extra = 0;
        for (uint64_t va = top - USER_STACK_BYTES; va < top; va += 4096) {
            uint64_t *leaf = user_leaf(owner, va, false);
            if (!leaf || !(*leaf & PTE_PRESENT))
                extra += 4096;
        }
        uint64_t limit = (tasks[owner].info.capabilities & PROCESS_CAP_SYSTEM)
                             ? 512ull * 1024 * 1024
                             : 64ull * 1024 * 1024;
        if (tasks[owner].info.mapped_bytes + vm_usage[owner].reserved - vm_usage[owner].resident >
                limit ||
            extra > limit - (tasks[owner].info.mapped_bytes + vm_usage[owner].reserved -
                             vm_usage[owner].resident))
            return -ERR_NOMEM;
        for (uint64_t va = top - USER_STACK_BYTES; va < top; va += 4096) {
            uint64_t *leaf = user_leaf(owner, va, false);
            if (leaf && (*leaf & PTE_PRESENT))
                zero_page((void *)(uintptr_t)(*leaf & PAGE_MASK));
            else if (!map_page(owner, va, PTE_WRITE | PTE_NX))
                return -ERR_NOMEM;
        }
        /* Thread stacks share the process address space, with an unmapped guard.
         * Their pages stay owned by the process until group exit. */
        memset(&tasks[slot].info, 0, sizeof tasks[slot].info);
        tasks[slot].owner = owner;
        tasks[slot].cr3 = tasks[owner].cr3;
        tasks[slot].wake_at = 0;
        memcpy(tasks[slot].fx, initial_fx, 512);
        tasks[slot].inbox_count = tasks[slot].inbox_first = 0;
        tasks[slot].info = tasks[owner].info;
        tasks[slot].info.pid = next_pid++;
        tasks[slot].info.thread = 1;
        tasks[slot].info.state = PROCESS_READY;
        tasks[slot].info.cpu_ticks = tasks[slot].info.switches = 0;
        tasks[slot].info.mapped_bytes = USER_STACK_BYTES;
        InterruptFrame *f =
            (InterruptFrame *)(void *)(tasks[slot].stack + KSTACK_BYTES - sizeof(InterruptFrame));
        memset(f, 0, sizeof *f);
        tasks[slot].frame = f;
        f->rip = r.entry;
        f->rdi = r.argument;
        f->cs = 0x1b;
        f->ss = 0x23;
        f->rflags = 0x202;
        f->rsp = top - 8;
        uint64_t p;
        user_translate(owner, top - 8, true, &p);
        *(uint64_t *)(uintptr_t)p = r.trampoline;
        r.tid = tasks[slot].info.pid;
    } else if (r.op == ARK_THREAD_ID)
        r.tid = tasks[current].info.pid;
    else if (r.op == ARK_THREAD_SLEEP || r.op == ARK_THREAD_WAIT || r.op == ARK_THREAD_WAIT_MS ||
             r.op == ARK_THREAD_WAIT_MSG_MS) {
        bool message_only = r.op == ARK_THREAD_WAIT_MSG_MS,
             milliseconds = r.op == ARK_THREAD_WAIT_MS || message_only,
             waiting = r.op != ARK_THREAD_SLEEP;
        if (r.ticks > (milliseconds ? 86400000u : 8640000u))
            return -ERR_INVAL;
        if (waiting &&
            (tasks[current].inbox_count || (!message_only && process_events_pending &&
                                            process_events_pending(process_current_pid()))))
            return 0;
        tasks[current].message_wait = message_only;
        tasks[current].wake_at =
            milliseconds ? platform_millis() + r.ticks : (platform_ticks() + r.ticks) * 10;
        tasks[current].info.state = waiting ? PROCESS_WAITING : PROCESS_SLEEPING;
        *reschedule = true;
    } else if (r.op == ARK_THREAD_EXIT) {
        if ((unsigned)current == owner)
            return -ERR_PERM;
        mark_dead((unsigned)current, r.status);
        *reschedule = true;
    } else if (r.op == ARK_THREAD_JOIN) {
        bool found = false;
        for (unsigned i = 0; i < PROCESS_MAX; i++)
            if (tasks[i].owner == owner && tasks[i].info.pid == r.tid && tasks[i].info.state) {
                if ((int)i == current || i == owner)
                    return -ERR_INVAL;
                if (tasks[i].info.state != PROCESS_DEAD || slot_active(i))
                    return -ERR_AGAIN;
                r.status = tasks[i].info.exit_status;
                tasks[i].info.state = PROCESS_FREE;
                found = true;
                break;
            }
        if (!found)
            return -ERR_NOENT;
    } else
        return -ERR_INVAL;
    (void)frame;
    return process_copy_to_user(ptr, &r, sizeof r) ? 0 : -ERR_FAULT;
}
__attribute__((weak)) unsigned platform_online_cpus(void) {
    return 1;
}
static int64_t task_call(uint64_t ptr, uint64_t size) {
    if (!process_has_cap(PROCESS_CAP_SYSTEM))
        return -ERR_PERM;
    ArkTaskRequest r;
    if (size != sizeof r)
        return -ERR_INVAL;
    if (!process_copy_from_user(&r, ptr, sizeof r) || !process_user_range(ptr, sizeof r, true))
        return -ERR_FAULT;
    if (r.op == ARK_TASK_KILL)
        return process_kill(r.tid, -9) ? 0 : -ERR_PERM;
    if ((r.op != ARK_TASK_LIST && r.op != ARK_TASK_METRICS) || r.capacity > PROCESS_MAX)
        return -ERR_INVAL;
    size_t stride = r.op == ARK_TASK_METRICS ? sizeof(ArkTaskMetrics) : sizeof(ArkTaskInfo);
    if (r.capacity && !process_user_range(r.buffer, r.capacity * stride, true))
        return -ERR_FAULT;
    r.count = 0;
    r.ticks = platform_ticks();
    r.online_cpus = platform_online_cpus();
    r.reserved = 0;
    for (unsigned i = 0; i < PROCESS_MAX && r.count < r.capacity; i++)
        if (tasks[i].info.state && tasks[i].info.state != PROCESS_DEAD) {
            ProcessInfo *p = &tasks[i].info;
            ArkTaskInfo t = {0};
            t.tid = p->pid;
            t.pid = p->group_pid;
            t.uid = p->uid;
            t.state = p->state;
            t.exit_status = p->exit_status;
            t.thread = p->thread;
            t.capabilities = p->capabilities;
            t.mapped_bytes = p->mapped_bytes;
            t.cpu_ticks = p->cpu_ticks;
            t.switches = p->switches;
            strcopy(t.name, p->name, sizeof t.name);
            unsigned owner = tasks[i].owner;
            if (r.op == ARK_TASK_METRICS)
                t.mapped_bytes = tasks[owner].info.mapped_bytes;
            ArkTaskMetrics detail = {.task = t,
                                     .reserved_bytes = vm_usage[owner].reserved,
                                     .committed_bytes = vm_usage[owner].committed,
                                     .swapped_bytes = vm_usage[owner].swapped,
                                     .faults = vm_usage[owner].faults,
                                     .read_bytes = tasks[owner].read_bytes,
                                     .write_bytes = tasks[owner].write_bytes,
                                     .messages_sent = tasks[owner].messages_sent,
                                     .messages_received = tasks[owner].messages_received};
            if (!process_copy_to_user(r.buffer + r.count * stride,
                                      r.op == ARK_TASK_METRICS ? (void *)&detail : (void *)&t,
                                      stride))
                return -ERR_FAULT;
            r.count++;
        }
    return process_copy_to_user(ptr, &r, sizeof r) ? (int64_t)r.count : -ERR_FAULT;
}
static int64_t message_send(uint64_t ptr, uint64_t bytes) {
    ArkMessage m;
    if (bytes != sizeof m)
        return -ERR_INVAL;
    if (!process_copy_from_user(&m, ptr, sizeof m))
        return -ERR_FAULT;
    if (m.length > sizeof m.data)
        return -ERR_INVAL;
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].info.pid == m.peer &&
            (tasks[i].info.state == PROCESS_READY || tasks[i].info.state == PROCESS_RUNNING ||
             tasks[i].info.state == PROCESS_SLEEPING || tasks[i].info.state == PROCESS_WAITING)) {
            if (tasks[i].info.uid != process_current_uid() &&
                !(tasks[i].info.capabilities & PROCESS_CAP_SYSTEM) &&
                !process_has_cap(PROCESS_CAP_SYSTEM))
                return -ERR_PERM;
            if (tasks[i].inbox_count == INBOX_CAP)
                return -ERR_AGAIN;
            m.peer = process_current_pid();
            m.reserved = 0;
            for (unsigned j = m.length; j < sizeof m.data; j++)
                m.data[j] = 0;
            tasks[i].inbox[(tasks[i].inbox_first + tasks[i].inbox_count) % INBOX_CAP] = m;
            tasks[i].inbox_count++;
            tasks[tasks[current].owner].messages_sent++;
            /* Messages address a particular TID. Wake that recipient; unrelated
             * event-waiting threads in its process have no new work. */
            if (tasks[i].info.state == PROCESS_WAITING) {
                tasks[i].info.state = PROCESS_READY;
                wake_idle_cpus();
            }
            return 0;
        }
    return -ERR_NOENT;
}
static int64_t message_recv(uint64_t ptr, uint64_t bytes) {
    if (bytes != sizeof(ArkMessage))
        return -ERR_INVAL;
    if (!tasks[current].inbox_count)
        return -ERR_AGAIN;
    if (!process_copy_to_user(ptr, &tasks[current].inbox[tasks[current].inbox_first],
                              sizeof(ArkMessage)))
        return -ERR_FAULT;
    tasks[current].inbox_first = (tasks[current].inbox_first + 1) % INBOX_CAP;
    tasks[current].inbox_count--;
    tasks[tasks[current].owner].messages_received++;
    return 0;
}
InterruptFrame *process_on_interrupt(InterruptFrame *frame) {
    account_cpu();
    if (!initialized || !cpu_state[platform_current_cpu()].ready)
        return frame;
    if (current < 0) {
        if (runtime && (frame->vector == 32 || frame->vector == 48 || frame->vector == 240)) {
            wake_tasks();
            process_service_poll();
            return schedule(frame, false);
        }
        return frame;
    }
    if ((frame->cs & 3) != 3)
        return frame;
    if (tasks[current].info.state == PROCESS_DEAD)
        return schedule(frame, false);
    frame->rflags = (frame->rflags & ~((3ull << 12) | (1ull << 14) | (1ull << 17))) | 0x202;
    tasks[current].frame = frame;
    if (frame->vector < 32) {
        uint64_t cr2 = 0;
        if (frame->vector == 14)
            __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        if (frame->vector == 14 && !(frame->error & 17)) {
            unsigned owner = tasks[current].owner;
            fault_owner = (int)owner;
            bool resolved = vm_resolve(owner, cr2, (frame->error & 2) != 0);
            fault_owner = -1;
            if (resolved)
                return frame;
        }
        serial_write("[process] User fault pid=");
        log_num(process_current_pid());
        serial_write(" vector=");
        log_num(frame->vector);
        serial_write(" error=");
        log_hex(frame->error);
        serial_write(" rip=");
        log_hex(frame->rip);
        if (frame->vector == 14) {
            serial_write(" address=");
            log_hex(cr2);
        }
        serial_write("; kernel survives\n");
        group_dead(tasks[current].owner, -(int)(128 + frame->vector));
        return schedule(frame, false);
    }
    if (frame->vector == 32 || frame->vector == 48) {
        unsigned cpu = platform_current_cpu();
        uint64_t tick = platform_ticks();
        tasks[current].info.cpu_ticks += tick - cpu_tick_last[cpu];
        cpu_tick_last[cpu] = tick;
        bool woke = wake_tasks();
        process_service_poll();
        return woke || platform_ticks() >= quantum_end ? schedule(frame, false) : frame;
    }
    if (frame->vector != 128)
        return frame;
    process_service_poll();
    uint64_t n = frame->rax;
    if (n == ARK_SYS_PERFORMANCE) {
        frame->rax = (uint64_t)performance_call(frame->rdi, frame->rsi);
        return frame;
    }
    if (n == ARK_SYS_EXIT) {
        group_dead(tasks[current].owner, (int)frame->rdi);
        return schedule(frame, false);
    }
    if (n == ARK_SYS_YIELD) {
        frame->rax = 0;
        return schedule(frame, frame->rdi == 0);
    }
    if (n == ARK_SYS_THREAD) {
        bool reschedule = false;
        frame->rax = (uint64_t)thread_call(frame->rdi, frame->rsi, frame, &reschedule);
        if (reschedule)
            return schedule(frame, false);
    } else if (n == ARK_SYS_TASKS)
        frame->rax = (uint64_t)task_call(frame->rdi, frame->rsi);
    else if (n == ARK_SYS_MEMORY)
        frame->rax = (uint64_t)memory_call(frame->rdi);
    else if (n == ARK_SYS_VM)
        frame->rax = (uint64_t)vm_call(frame->rdi, frame->rsi);
    else if (n == ARK_SYS_GETPID)
        frame->rax = process_current_pid();
    else if (n == ARK_SYS_MSG_SEND)
        frame->rax = (uint64_t)message_send(frame->rdi, frame->rsi);
    else if (n == ARK_SYS_MSG_RECV)
        frame->rax = (uint64_t)message_recv(frame->rdi, frame->rsi);
    else
        frame->rax = (uint64_t)process_syscall_dispatch(n, frame->rdi, frame->rsi, frame->rdx,
                                                        frame->r10, frame->r8, frame->r9);
    return frame;
}
__attribute__((noreturn)) void process_ap_run(void) {
    __asm__ volatile("cli" ::: "memory");
    process_kernel_enter();
    process_cpu_init();
    platform_load_process_idt();
    if (smp_local_timer)
        smp_local_timer();
    serial_write("[smp] User scheduler CPU ");
    log_num(platform_current_cpu());
    serial_write(" ready\n");
    process_enter_frame(schedule(0, false));
}
__attribute__((noreturn)) void process_run(void) {
    __asm__ volatile("cli" ::: "memory");
    __atomic_store_n(&runtime, true, __ATOMIC_RELEASE);
    process_kernel_enter();
    if (smp_start_users)
        smp_start_users();
    serial_write("[process] Entering Ring3 SMP scheduler\n");
    process_enter_frame(schedule(0, false));
}

void process_update_caps(const char *name, uint32_t uid, uint64_t caps) {
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].info.state != PROCESS_FREE && tasks[i].info.uid == uid &&
            !strcmp(tasks[tasks[i].owner].info.name, name) &&
            !(tasks[i].info.capabilities & PROCESS_CAP_SYSTEM)) {
            tasks[i].info.capabilities = caps;
            if (!(caps & PROCESS_CAP_UI) && tasks[i].info.state != PROCESS_DEAD)
                group_dead(tasks[i].owner, -1);
        }
}
