#include "ark.h"
#include "gpu.h"
#include "process.h"
#include "accounts.h"
#include "virtio_input.h"
extern void services_init(const BootInfo *);
extern const uint8_t controller_start[], controller_end[];
void kernel_main(uint32_t magic, uint32_t address) {
    BootInfo boot;
    platform_init(magic, address, &boot);
    gpu_init(&boot);
    vfs_init();
    accounts_init();
    services_init(&boot);
    if (!process_init())
        for (;;)
            platform_idle();
    int pid = process_spawn_elf(controller_start, (size_t)(controller_end - controller_start),
                                "Count controller", UINT32_MAX, 31);
    if (pid != 1)
        for (;;)
            platform_idle();
    process_run();
}
