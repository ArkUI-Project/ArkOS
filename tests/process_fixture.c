/* Standalone protection test kernel. No GUI, services or disk mutation. */
#include "process.h"
#ifdef ARK_SMP_TEST
#include "smp.h"
#endif
static BootInfo boot;
extern const unsigned char _binary_probe0_elf_start[], _binary_probe0_elf_end[];
extern const unsigned char _binary_probe1_elf_start[], _binary_probe1_elf_end[];
extern const unsigned char _binary_probe2_elf_start[], _binary_probe2_elf_end[];
extern const unsigned char _binary_probe3_elf_start[], _binary_probe3_elf_end[];
extern const unsigned char _binary_probe4_elf_start[], _binary_probe4_elf_end[];
extern const unsigned char _binary_probe5_elf_start[], _binary_probe5_elf_end[];
extern const unsigned char _binary_probe6_elf_start[], _binary_probe6_elf_end[];
extern const unsigned char _binary_probe7_elf_start[], _binary_probe7_elf_end[];
static uint32_t pids[8];
static bool dead[8];
static int statuses[8];
static unsigned char malformed[65536];
static void require(bool yes, const char *m) {
    if (!yes) {
        serial_write("[process-test] FAIL ");
        serial_write(m);
        serial_write("\n");
        for (;;)
            platform_idle();
    }
}
void process_exit_notify(uint32_t pid) {
    for (unsigned i = 0; i < 8; i++)
        if (pids[i] == pid) {
            ProcessInfo info;
            require(process_get_info(pid, &info), "exit info");
            dead[i] = true;
            statuses[i] = info.exit_status;
        }
}
int64_t process_syscall_dispatch(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                 uint64_t e, uint64_t f) {
    (void)d;
    (void)e;
    (void)f;
    if (n == 100) {
        char text[256];
        if (b > 255 || !process_copy_from_user(text, a, b))
            return -14;
        text[b] = 0;
        serial_write(text);
        return 0;
    }
    if (n == 101) {
        char copy[64];
        if (b > sizeof copy || !process_copy_from_user(copy, a, b) ||
            !process_copy_to_user(c, copy, b))
            return -14;
        return 0;
    }
    if (n == 102) {
        if (!(dead[1] && dead[2] && dead[3] && dead[4] && dead[5] && dead[7]))
            return 0;
        require(statuses[1] == -142, "kernel read must #PF");
        require(statuses[2] == -141, "port IO must #GP");
        require(statuses[3] == -142, "data execute must NX #PF");
        require(statuses[4] == -142, "text write must #PF");
        require(statuses[5] == 0, "bad pointer probe must survive");
        require(statuses[7] == 0, "thread and FPU context probes must pass");
        require(process_kill(pids[6], 0), "preempted spinner kill");
        serial_write(
            "[process-test] PASS isolation, W^X/NX, IO, pointers, ELF, preemption, survivor\n");
        return 1;
    }
    return -38;
}
void kernel_main(uint32_t magic, uint32_t mb) {
    platform_init(magic, mb, &boot);
#ifdef ARK_SMP_TEST
    smp_init();
#endif
    require(process_init(), "initialization");
    size_t original = (size_t)(_binary_probe0_elf_end - _binary_probe0_elf_start);
    require(original < sizeof malformed, "test ELF size");
    require(process_spawn_elf(_binary_probe0_elf_start, 31, "short", 1000, 0) < 0,
            "truncated ELF rejected");
    memcpy(malformed, _binary_probe0_elf_start, original);
    malformed[0] = 0;
    require(process_spawn_elf(malformed, original, "magic", 1000, 0) < 0,
            "wrong ELF magic rejected");
    memcpy(malformed, _binary_probe0_elf_start, original);
    *(uint64_t *)(void *)(malformed + 32) = UINT64_MAX - 16;
    require(process_spawn_elf(malformed, original, "phoff", 1000, 0) < 0,
            "program header overflow rejected");
    memcpy(malformed, _binary_probe0_elf_start, original);
    uint64_t off = *(uint64_t *)(void *)(malformed + 32);
    *(uint32_t *)(void *)(malformed + off + 4) = 7;
    require(process_spawn_elf(malformed, original, "wx", 1000, 0) < 0, "W+X segment rejected");
    memcpy(malformed, _binary_probe0_elf_start, original);
    *(uint64_t *)(void *)(malformed + off + 16) = 0x100000;
    require(process_spawn_elf(malformed, original, "kernel-va", 1000, 0) < 0,
            "kernel virtual segment rejected");
    memcpy(malformed, _binary_probe0_elf_start, original);
    *(uint64_t *)(void *)(malformed + 24) = 0x50000000;
    require(process_spawn_elf(malformed, original, "entry", 1000, 0) < 0,
            "non-executable entry rejected");
    serial_write("[process-test] Malformed ELF rejection PASS\n");
    const unsigned char *starts[] = {_binary_probe0_elf_start, _binary_probe1_elf_start,
                                     _binary_probe2_elf_start, _binary_probe3_elf_start,
                                     _binary_probe4_elf_start, _binary_probe5_elf_start,
                                     _binary_probe6_elf_start, _binary_probe7_elf_start};
    const unsigned char *ends[] = {_binary_probe0_elf_end, _binary_probe1_elf_end,
                                   _binary_probe2_elf_end, _binary_probe3_elf_end,
                                   _binary_probe4_elf_end, _binary_probe5_elf_end,
                                   _binary_probe6_elf_end, _binary_probe7_elf_end};
    const char *names[] = {"survivor",   "kernel-read", "port-io", "nx-exec",
                           "text-write", "bad-pointer", "spinner", "fpu"};
    for (unsigned i = 0; i < 8; i++) {
        int pid = process_spawn_elf(starts[i], (size_t)(ends[i] - starts[i]), names[i], 1000,
                                    i == 0 ? PROCESS_CAP_SYSTEM : 0);
        require(pid > 0, "spawn probe");
        pids[i] = (uint32_t)pid;
    }
    process_run();
}
