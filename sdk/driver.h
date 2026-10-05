#ifndef ARK_DRIVER_SDK_H
#define ARK_DRIVER_SDK_H
/* ArkOS .arco kernel driver SDK 0.13.0.
 *
 * A driver is one freestanding C file that defines arco_entry. It runs in kernel
 * mode on a private 32 KiB stack inside the kernel's module window; it must not
 * touch user memory, page tables or another module's state. Everything a driver
 * may reach comes through the ArkDriverHost table handed to arco_entry, whose
 * shape is fixed by include/ark_driver.h.
 *
 * Build:  python3 scripts/arco.py sdk/driver_demo.c -o build/demo.arco \
 *            --name demo --version 1.0.0
 * Install: the desktop device page or the shell command
 *            dev install /mnt/fat32/demo.arco
 *         requires an active admin session; the kernel verifies the image
 *         against the protected manifest and only then maps and calls it.
 */
#include "ark_driver.h"
/* Device descriptors and class/flag/state constants come from the public ABI
 * header. ARK_KERNEL selects the declaration-only form: a driver never issues
 * int 0x80, it calls the host table instead. */
#ifndef ARK_KERNEL
#define ARK_KERNEL
#endif
#include "ark_api.h"

/* Freestanding memory and string helpers: a .arco image links no libc. */
static inline void *ark_memset(void *p, int value, unsigned bytes) {
    unsigned char *out = (unsigned char *)p;
    while (bytes--)
        *out++ = (unsigned char)value;
    return p;
}
static inline void ark_strcopy(char *dst, const char *src, unsigned cap) {
    if (!dst || !cap)
        return;
    unsigned i = 0;
    for (; i + 1 < cap && src && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static inline void ark_strcat(char *dst, const char *src, unsigned cap) {
    if (!dst || !cap)
        return;
    unsigned at = 0;
    while (at + 1 < cap && dst[at])
        at++;
    unsigned i = 0;
    for (; i + 1 < cap - at && src && src[i]; i++)
        dst[at + i] = src[i];
    dst[at + i] = 0;
}

/* Fill a device descriptor. ARK_DEV_MODULE marks the node as module-owned, so
 * the loader can retire it when the module is removed; bus ARK_BUS_VIRTUAL
 * states that no fixed hardware address claims the node. */
static inline void ark_drv_node(ArkDeviceInfo *node, uint32_t class_id,
                                uint32_t unit, const char *name,
                                const char *driver, const char *detail) {
    ark_memset(node, 0, sizeof *node);
    node->class_id = class_id;
    node->bus = ARK_BUS_VIRTUAL;
    node->unit = unit;
    node->flags = ARK_DEV_PRESENT | ARK_DEV_MODULE;
    node->state = ARK_DEV_STATE_OK;
    ark_strcopy(node->name, name, sizeof node->name);
    ark_strcopy(node->driver, driver, sizeof node->driver);
    ark_strcopy(node->detail, detail, sizeof node->detail);
}

#endif