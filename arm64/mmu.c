/* AArch64 4 KiB stage-1 translation and EL0 protection probes.
 * This is a native platform foundation, not the x86 desktop port. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define AF (1ull << 10)
#define SH (3ull << 8)
#define PXN (1ull << 53)
#define UXN (1ull << 54)
#define RO (2ull << 6)
#define USER (1ull << 6)
static uint64_t l1[512] __attribute__((aligned(4096)));
static uint64_t ram_l2[512] __attribute__((aligned(4096)));
static uint64_t kernel_l3[512] __attribute__((aligned(4096)));
static uint64_t user_l3[512] __attribute__((aligned(4096)));
static uint8_t user_stack[4096] __attribute__((aligned(4096)));
extern char __text_start[], __text_end[], __rodata_end[], arm_vectors[], arm_user_code[];
extern void arm_el0_enter(unsigned mode);
extern void arm_log(const char *);
static unsigned expected, passed;
static bool failed;
void arm_mmu_init(void) {
    l1[0] = AF | 4 | PXN | UXN | 1; /* device-nGnRE, EL1 only */
    l1[1] = (uintptr_t)ram_l2 | 3;
    for (unsigned i = 0; i < 256; i++)
        ram_l2[i] = (0x40000000ull + i * 0x200000ull) | AF | SH | PXN | UXN | 1;
    ram_l2[1] = (uintptr_t)kernel_l3 | 3;
    for (unsigned i = 0; i < 512; i++) {
        uintptr_t p = 0x40200000u + i * 4096u;
        uint64_t attributes = AF | SH | UXN;
        if (p >= (uintptr_t)__text_start && p < (uintptr_t)__text_end)
            attributes |= RO;
        else {
            attributes |= PXN;
            if (p < (uintptr_t)__rodata_end)
                attributes |= RO;
        }
        kernel_l3[i] = p | attributes | 3;
    }
    /* EL0's only mappings: immutable code and a private writable NX stack. */
    ram_l2[128] = (uintptr_t)user_l3 | 3;
    user_l3[0] = (uintptr_t)arm_user_code | AF | SH | USER | RO | PXN | 3;
    user_l3[1] = (uintptr_t)user_stack | AF | SH | USER | PXN | UXN | 3;
    __asm__ volatile("dsb sy" ::: "memory");
}
void arm_mmu_enable(void) {
    uint64_t mair = 0x04ff,
             tcr = 32ull | (1ull << 8) | (1ull << 10) | (3ull << 12) | (1ull << 23) | (2ull << 32),
             control;
    __asm__ volatile("msr mair_el1,%0;msr tcr_el1,%1;msr ttbr0_el1,%2;msr vbar_el1,%3;isb;tlbi "
                     "vmalle1;dsb sy;isb" ::"r"(mair),
                     "r"(tcr), "r"((uintptr_t)l1), "r"((uintptr_t)arm_vectors)
                     : "memory");
    __asm__ volatile("mrs %0,sctlr_el1" : "=r"(control));
    control |= 1 | (1 << 2) | (1 << 12) | (1 << 19); /* M, C, I, WXN */
    __asm__ volatile("msr sctlr_el1,%0;isb" ::"r"(control) : "memory");
}
typedef struct {
    uint64_t x[31], elr, spsr, sp, esr, far, vector, pad;
} Frame;
unsigned arm_el0_exception(Frame *f) {
    unsigned ec = (unsigned)(f->esr >> 26), dfsc = (unsigned)f->esr & 63;
    if (ec == 0x15 && expected == 0) {
        if (f->x[8] == 1) {
            f->x[0] = 0x41524b;
            return 0;
        }
        if (f->x[8] == 42 && f->x[0] == 0x41524b) {
            passed++;
            arm_log("[arm64] EL0 SVC round trip PASS\n");
            return 1;
        }
    }
    if (expected == 1 && ec == 0x24 && f->far == 0x40200000 && dfsc == 15) {
        passed++;
        arm_log("[arm64] EL0 kernel read denied PASS\n");
        return 1;
    }
    if (expected == 2 && ec == 0x24 && f->far == 0x50000000 && dfsc == 15) {
        passed++;
        arm_log("[arm64] EL0 executable page write denied PASS\n");
        return 1;
    }
    if (expected == 3 && ec == 0x20 && f->far == 0x50001000 && dfsc == 15) {
        passed++;
        arm_log("[arm64] EL0 writable stack execute denied PASS\n");
        return 1;
    }
    failed = true;
    arm_log("[arm64] EL0 protection FAIL\n");
    return 1;
}
void arm_el0_test(void) {
    passed = 0;
    failed = false;
    for (expected = 0; expected < 4; expected++)
        arm_el0_enter(expected);
    arm_log(!failed && passed == 4 ? "[arm64] MMU + EL0 + SVC + W^X protection PASS\n"
                                   : "[arm64] Protection self-test FAILED\n");
}
__attribute__((noreturn)) void arm_exception_fatal(void) {
    arm_log("[arm64] FATAL unexpected privileged exception\n");
    for (;;)
        __asm__ volatile("wfe");
}
