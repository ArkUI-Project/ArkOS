/* Isolated native ring-3 probes, never linked into the desktop executable. */
#include "ark_api.h"
#ifndef PROBE_MODE
#define PROBE_MODE 0
#endif
static void report(const char *s, size_t n) {
    (void)ark_syscall6(100, (uintptr_t)s, n, 0, 0, 0, 0);
}
#define SAY(s) report(s, sizeof(s) - 1)
#if PROBE_MODE == 7
static volatile unsigned completed;
#ifdef ARK_SMP_TEST
static volatile unsigned seen_cpus;
#endif
static void thread_return(void) {
    ArkThreadRequest r = {0};
    r.op = ARK_THREAD_EXIT;
    r.status = 17;
    (void)ark_thread(&r);
    for (;;)
        ark_yield();
}
static void worker(uint64_t arg) {
    uint64_t expected = 0x1234567800000000ull + arg, actual = 0;
    __asm__ volatile("movq %0,%%xmm15" ::"r"(expected));
    for (unsigned i = 0; i < 24; i++) {
#ifdef ARK_SMP_TEST
        uint32_t a = 1, b, c, d;
        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
        __atomic_fetch_or(&seen_cpus, 1u << ((b >> 24) & 31), __ATOMIC_SEQ_CST);
#endif
        ark_yield();
        __asm__ volatile("movq %%xmm15,%0" : "=r"(actual));
        if (actual != expected) {
            SAY("[probe] SSE context FAIL\n");
            ark_exit(71);
        }
    }
    ArkThreadRequest sleep = {0};
    sleep.op = ARK_THREAD_SLEEP;
    sleep.ticks = 3;
    if (ark_thread(&sleep) < 0)
        ark_exit(72);
    __atomic_add_fetch(&completed, 1, __ATOMIC_SEQ_CST);
}
#endif
int main(void) {
#if PROBE_MODE == 1
    (void)*(volatile uint64_t *)(uintptr_t)0x1000000;
#elif PROBE_MODE == 2
    __asm__ volatile("outb %0,$0x80" ::"a"((unsigned char)0));
#elif PROBE_MODE == 3
    unsigned char *p = ark_memory(4096);
    if (!p)
        return 1;
    p[0] = 0xc3;
    ((void (*)(void))(uintptr_t)p)();
#elif PROBE_MODE == 4
    *(volatile unsigned char *)(uintptr_t)main = 0xc3;
#elif PROBE_MODE == 5
    static char text[64] = "user-copy";
    char output[64] = {0};
    if (ark_syscall6(101, (uintptr_t)text, sizeof text, (uintptr_t)output, 0, 0, 0) != 0)
        return 1;
    for (unsigned i = 0; i < sizeof text; i++)
        if (text[i] != output[i])
            return 2;
    if (ark_syscall6(101, 0x100000, 16, (uintptr_t)output, 0, 0, 0) != -14)
        return 3;
    if (ark_syscall6(101, 0xfffffffffffffff0ull, 32, (uintptr_t)output, 0, 0, 0) != -14)
        return 4;
    if (ark_syscall6(101, 0x7fffdff0ull, 32, (uintptr_t)output, 0, 0, 0) != -14)
        return 5;
    if (ark_syscall6(101, (uintptr_t)text, 16, (uintptr_t)main, 0, 0, 0) != -14)
        return 6;
    if (ark_syscall6(999, 0, 0, 0, 0, 0, 0) != -38)
        return 7;
    if (ark_memory(0) || ark_memory((size_t)-1) || ark_memory(64u * 1024 * 1024))
        return 8;
    unsigned char *p = ark_memory(8193);
    if (!p)
        return 9;
    p[0] = 17;
    p[8192] = 29;
    unsigned char *next = ark_memory(4096);
    if (!next || next == p || next[0] || p[0] != 17 || p[8192] != 29)
        return 10;
    SAY("[probe] Pointer validation and bounded private heap PASS\n");
    return 0;
#elif PROBE_MODE == 6
    SAY("[probe] Noncooperative spin entered\n");
    for (;;)
        __asm__ volatile("pause");
#elif PROBE_MODE == 7
    __asm__ volatile("fninit");
    ArkThreadRequest bad = {0};
    bad.op = ARK_THREAD_CREATE;
    bad.entry = 0x1000000;
    bad.trampoline = (uintptr_t)thread_return;
    if (ark_thread(&bad) != -14)
        return 73;
    uint32_t tids[4];
    for (unsigned i = 0; i < 4; i++) {
        ArkThreadRequest r = {0};
        r.op = ARK_THREAD_CREATE;
        r.entry = (uintptr_t)worker;
        r.trampoline = (uintptr_t)thread_return;
        r.argument = i;
        if (ark_thread(&r) < 0)
            return 74;
        tids[i] = r.tid;
    }
    for (unsigned i = 0; i < 4; i++) {
        ArkThreadRequest r = {0};
        r.op = ARK_THREAD_JOIN;
        r.tid = tids[i];
        int64_t result;
        while ((result = ark_thread(&r)) == -11)
            ark_yield();
        if (result || r.status != 17)
            return 75;
    }
    if (completed != 4)
        return 76;
#ifdef ARK_SMP_TEST
    if (!(seen_cpus & (seen_cpus - 1))) {
        SAY("[probe] User threads never ran on distinct CPUs FAIL\n");
        return 78;
    }
    SAY("[probe] User threads executed on distinct hardware CPUs PASS\n");
#endif
    SAY("[probe] Shared-address threads, guarded entry, sleep/join, isolated SSE PASS\n");
    return 0;
#else
    /* The survivor is first and acts as a tiny system desktop stand-in. All
     * peers fault/exit except the non-yielding spinner, which timer must preempt. */
    for (unsigned i = 0; i < 100; i++) {
        if (ark_syscall6(102, 0, 0, 0, 0, 0, 0) == 1) {
            SAY("[probe] Ring3 survivor running after faults and preemption PASS\n");
            return 0;
        }
        ArkThreadRequest pause = {0};
        pause.op = ARK_THREAD_SLEEP;
        pause.ticks = 1;
        (void)ark_thread(&pause);
    }
    SAY("[probe] Survivor timed out FAIL\n");
    return 99;
#endif
    SAY("[probe] Forbidden instruction unexpectedly survived FAIL\n");
    return 100;
}
