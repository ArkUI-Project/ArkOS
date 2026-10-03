/* Full production services with test-only launch/control hooks. Not release. */
#include "ark.h"
#include "gpu.h"
#include "process.h"
#include "accounts.h"
#include "net.h"
#include "virtio_input.h"
extern const uint8_t _binary_controller_elf_start[], _binary_controller_elf_end[],
    _binary_untrusted_elf_start[], _binary_untrusted_elf_end[];
extern int64_t services_real_dispatch(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t);
extern void services_init(const BootInfo *);
static uint32_t child_pid[16];
static int child_stage[16];
static unsigned spawned;
int64_t process_syscall_dispatch(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                 uint64_t e, uint64_t f) {
    if (n == 903) {
        for (unsigned i = 0; i < spawned; i++)
            if (child_pid[i] == process_current_pid())
                return child_stage[i];
        return -1;
    }
    if (n >= 900 && n <= 902) {
        if (!process_has_cap(PROCESS_CAP_SYSTEM))
            return -1;
        if (n == 900) {
            if (spawned == 16)
                return -12;
            uint32_t uid = a == 2 ? 1001 : 1000;
            uint64_t caps = a == 3   ? 0
                            : a == 4 ? PROCESS_CAP_UI
                                     : PROCESS_CAP_FILES | PROCESS_CAP_UI | PROCESS_CAP_NETWORK;
            int pid =
                process_spawn_elf(_binary_untrusted_elf_start,
                                  (size_t)(_binary_untrusted_elf_end - _binary_untrusted_elf_start),
                                  a == 4 ? "browser" : "API untrusted", uid, caps);
            if (pid > 0) {
                child_pid[spawned] = (uint32_t)pid;
                child_stage[spawned++] = (int)a;
            }
            return pid;
        }
        if (n == 901) {
            ProcessInfo info;
            if (!process_get_info((uint32_t)a, &info))
                return -2;
            return info.state == PROCESS_DEAD ? info.exit_status : INT64_MIN;
        }
        if (n == 902) {
            serial_write("[api-test] Verified kernel SYSTEM PID=");
            char out[24];
            uint_to_str(process_current_pid(), out);
            serial_write(out);
            serial_write(" UID=");
            uint_to_str(process_current_uid(), out);
            serial_write(out);
            serial_write(" survives all probes\n");
            return 0;
        }
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
    if (!process_init()) {
        serial_write("[api-test] FAIL initialization\n");
        for (;;)
            platform_idle();
    }
    int pid = process_spawn_elf(_binary_controller_elf_start,
                                (size_t)(_binary_controller_elf_end - _binary_controller_elf_start),
                                "API SYSTEM controller", UINT32_MAX, 31);
    if (pid != 1) {
        serial_write("[api-test] FAIL controller spawn\n");
        for (;;)
            platform_idle();
    }
    process_run();
}
