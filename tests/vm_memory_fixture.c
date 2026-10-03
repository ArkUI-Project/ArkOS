#include "ark.h"
#include "gpu.h"
#include "process.h"
#include "storage.h"
#include "accounts.h"
#include "net.h"
#include "virtio_input.h"
extern const uint8_t _binary_controller_elf_start[], _binary_controller_elf_end[],
    _binary_untrusted_elf_start[], _binary_untrusted_elf_end[];
extern int64_t services_real_dispatch(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t);
extern void services_init(const BootInfo *);
static uint32_t children[3];
int64_t process_syscall_dispatch(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                 uint64_t e, uint64_t f) {
    if (n == 903) {
        for (unsigned i = 0; i < 3; i++)
            if (children[i] == process_current_pid())
                return i + 1;
        return 0;
    }
    if (n >= 900 && n <= 905) {
        if (!process_has_cap(PROCESS_CAP_SYSTEM))
            return -1;
        if (n == 900 && a >= 1 && a <= 3) {
            int pid =
                process_spawn_elf(_binary_untrusted_elf_start,
                                  (size_t)(_binary_untrusted_elf_end - _binary_untrusted_elf_start),
                                  "VM protection probe", 1000, 0);
            if (pid > 0)
                children[a - 1] = (uint32_t)pid;
            return pid;
        }
        if (n == 901) {
            ProcessInfo q;
            if (!process_get_info((uint32_t)a, &q))
                return -2;
            return q.state == PROCESS_DEAD ? q.exit_status : INT64_MIN;
        }
        if (n == 905) {
            int i = vfs_find("/vm-sentinel.txt");
            return i >= 0 && !strcmp(vfs_files[i].data, "Existing files survive paging.\n");
        }
        return -22;
    }
    return services_real_dispatch(n, a, b, c, d, e, f);
}
void kernel_main(uint32_t magic, uint32_t mb) {
    BootInfo boot;
    platform_init(magic, mb, &boot);
    gpu_init(&boot);
    vfs_init();
    virtio_input_init();
    net_init();
    accounts_init();
    services_init(&boot);
    if (vfs_find("/vm-sentinel.txt") < 0) {
        int i = vfs_create("/vm-sentinel.txt");
        if (i < 0 || !vfs_write(i, "Existing files survive paging.\n") || !storage_sync()) {
            serial_write("[vm-probe] FAIL fixture storage\n");
            for (;;)
                platform_idle();
        }
    }
    if (!process_init() ||
        process_spawn_elf(_binary_controller_elf_start,
                          (size_t)(_binary_controller_elf_end - _binary_controller_elf_start),
                          "VM SYSTEM probe", 1000, 31) != 1) {
        serial_write("[vm-probe] FAIL initialization\n");
        for (;;)
            platform_idle();
    }
    serial_write("[vm-probe] Scheduler ready\n");
    process_run();
}
