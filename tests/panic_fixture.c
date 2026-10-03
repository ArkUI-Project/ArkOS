#include "ark.h"
#include "gpu.h"
#include "smp.h"
void kernel_main(uint32_t magic, uint32_t address) {
    BootInfo b;
    platform_init(magic, address, &b);
    gpu_init(&b);
    smp_init();
    serial_write("[panic-test] Triggering a real privileged invalid opcode\n");
    __asm__ volatile("ud2");
    for (;;)
        platform_idle();
}
