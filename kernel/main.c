#include "ark.h"
#include "gpu.h"
#include "storage.h"
#include "virtio_input.h"
#include "spice_mouse.h"
#include "process.h"
#define ARK_KERNEL
#include "package.h"
#include "accounts.h"
#include "net.h"
#include "smp.h"
#include "microcode.h"
extern const void *platform_microcode(size_t *);
extern const uint8_t _binary_build_user_desktop_elf_start[], _binary_build_user_desktop_elf_end[];
void services_init(const BootInfo *info);
void kernel_main(uint32_t magic, uint32_t mb) {
    BootInfo boot;
    platform_init(magic, mb, &boot);
    size_t ucode_bytes;
    const void *ucode = platform_microcode(&ucode_bytes);
    microcode_init(ucode, ucode_bytes);
    gpu_init(&boot);
    vfs_init();
    virtio_input_init();
    spice_mouse_init(boot.width, boot.height);
    net_init();
    smp_init();
    accounts_init();
    services_init(&boot);
    if (!process_init()) {
        serial_write("[fatal] user isolation initialization failed\n");
        for (;;)
            platform_idle();
    }
    if (!package_system_ready()) {
        serial_write("[fatal] system package integrity failed\n");
        for (;;)
            platform_idle();
    }
    int pid = process_spawn_elf(
        _binary_build_user_desktop_elf_start,
        (size_t)(_binary_build_user_desktop_elf_end - _binary_build_user_desktop_elf_start),
        "Ark Desktop", ACCOUNTS_UID_NONE, 31);
    if (pid < 1) {
        serial_write("[fatal] desktop ELF load failed\n");
        for (;;)
            platform_idle();
    }
    serial_write("[arkos] kernel services ready; entering isolated desktop\n");
    process_run();
}
