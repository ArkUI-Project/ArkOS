/* Inbox .arco driver images linked into kernel.elf.
 *
 * These blobs are kernel build artifacts — trusted exactly like kernel text —
 * but they still pass module_load's structural checks, so a corrupt or
 * truncated image fails closed at boot instead of executing garbage. The
 * module loader loads each entry after manifest-installed drivers, so an
 * on-disk update under the same name shadows the inbox copy.
 *
 * e1000: network adapter driver (sdk/driver_e1000.c). Keeping the NIC driver
 * in the image means a bare ISO boot, a non-ArkFS disk and a fresh install
 * all have networking; without it the driver could only come from the
 * very medium it is needed to reach.
 *
 * nvme: PCIe NVM storage driver (sdk/driver_nvme.c). MSI-X only on QEMU, so
 * it also exercises the kernel's dynamic MSI-X vector path. It declines to
 * bind when no controller is present. */
    .section .rodata
    .balign 8
    .globl _arkos_driver_e1000_start
_arkos_driver_e1000_start:
    .incbin "build/e1000.arco"
    /* No padding before _end: module_load requires end-start to equal the
     * file's own byte count (header + image_size), so alignment padding
     * would read as a corrupt image. */
    .globl _arkos_driver_e1000_end
_arkos_driver_e1000_end:
    .balign 8
    .globl _arkos_driver_nvme_start
_arkos_driver_nvme_start:
    .incbin "build/nvme.arco"
    .globl _arkos_driver_nvme_end
_arkos_driver_nvme_end:
    .section .note.GNU-stack,"",@progbits
