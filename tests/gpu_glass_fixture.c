#include "ark.h"
#include "gpu.h"
#include "process.h"
#include "accounts.h"
#include "virtio_input.h"
#include "smp.h"
extern const uint8_t _binary_controller_elf_start[], _binary_controller_elf_end[],
    _binary_untrusted_elf_start[], _binary_untrusted_elf_end[];
extern int64_t services_real_dispatch(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t);
extern void services_init(const BootInfo *);
int64_t process_syscall_dispatch(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                 uint64_t e, uint64_t f) {
    if (n == 900 || n == 901) {
        if (!process_has_cap(PROCESS_CAP_SYSTEM))
            return -1;
        if (n == 900)
            return process_spawn_elf(
                _binary_untrusted_elf_start,
                (size_t)(_binary_untrusted_elf_end - _binary_untrusted_elf_start),
                "GPU isolation probe", 1000, PROCESS_CAP_UI);
        ProcessInfo q;
        if (!process_get_info((uint32_t)a, &q))
            return -2;
        return q.state == PROCESS_DEAD ? q.exit_status : INT64_MIN;
    }
    return services_real_dispatch(n, a, b, c, d, e, f);
}
void kernel_main(uint32_t magic, uint32_t mb) {
    BootInfo boot;
    platform_init(magic, mb, &boot);
    gpu_init(&boot);
    vfs_init();
    virtio_input_init();
    smp_init();
    accounts_init();
    services_init(&boot);
    if (!process_init() ||
        process_spawn_elf(_binary_controller_elf_start,
                          (size_t)(_binary_controller_elf_end - _binary_controller_elf_start),
                          "GPU SYSTEM probe", 1000, 31) != 1) {
        serial_write("[glass-probe] FAIL initialization\n");
        for (;;)
            platform_idle();
    }
    serial_write("[glass-probe] Scheduler ready\n");
    process_run();
}
