/* Boot-time device inventory. Drivers keep their transports; this file turns
 * what each of them actually proved into kernel device nodes, and keeps the
 * counters current from the existing service poll. No value here is invented:
 * every field comes from a driver status structure. */
#include "device.h"
#include "block.h"
#include "storage.h"
#include "extfs.h"
#include "gpu.h"
#include "virtio_input.h"
#include "spice_mouse.h"

#define DETAIL_MAX 96u

static uint32_t display_node, input_node;
static uint64_t gpu_frames;
static uint32_t contacts = UINT32_MAX;

static void append(char *text, size_t cap, const char *tail) {
    size_t at = strlen(text);
    if (!tail || !tail[0])
        return;
    if (at + 1 >= cap)
        return;
    text[at++] = ' ';
    text[at] = 0;
    strcopy(text + at, tail, cap - at);
}
static void append_text(char *text, size_t cap, const char *tail) {
    append(text, cap, tail);
}
static void append_count(char *text, size_t cap, uint64_t value, const char *tail) {
    char number[24];
    uint_to_str(value, number);
    append(text, cap, number);
    append(text, cap, tail);
}
static void register_block(void) {
    for (unsigned unit = 0; unit < 2; unit++) {
        const BlockDevice *d = block_device(unit);
        if (!d || !d->present)
            continue;
        ArkDeviceInfo node = {0};
        node.class_id = ARK_DEV_CLASS_BLOCK;
        node.bus = ARK_BUS_ISA;
        node.unit = unit;
        node.blocks = d->sectors;
        node.flags = ARK_DEV_PRESENT | ARK_DEV_READABLE | ARK_DEV_WRITABLE;
        node.state = ARK_DEV_STATE_OK;
        strcopy(node.name, "Disk port", sizeof node.name);
        append_count(node.name, sizeof node.name, unit, "");
        strcopy(node.driver, "block", sizeof node.driver);
        append_count(node.detail, sizeof node.detail, d->sectors, "sectors x 512 B");
        if (storage_disk_in_use(unit)) {
            node.flags |= ARK_DEV_SYSTEM_VOLUME;
            append_text(node.detail, sizeof node.detail, "ArkFS system volume");
        }
        device_register(&node);
    }
}
/* The network adapter node is registered by the NIC driver module itself
 * (device_register from its INIT, counters from its poll callback) so `dev`
 * attribution and removal semantics belong to the module. */
static void register_display(void) {
    const GpuStats *s = gpu_stats();
    if (!s)
        return;
    ArkDeviceInfo node = {0};
    node.class_id = ARK_DEV_CLASS_DISPLAY;
    node.bus = ARK_BUS_VIRTUAL;
    node.flags = ARK_DEV_PRESENT | ARK_DEV_READABLE;
    node.state = ARK_DEV_STATE_OK;
    strcopy(node.name, gpu_backend_name(), sizeof node.name);
    strcopy(node.driver, "gpu", sizeof node.driver);
    append_text(node.detail, sizeof node.detail,
                s->accelerated ? "2D accelerated" : "software framebuffer");
    if (s->shader_accelerated)
        append_text(node.detail, sizeof node.detail, "VirGL shaders");
    if (s->hardware_cursor)
        append_text(node.detail, sizeof node.detail, "hardware cursor");
    gpu_frames = s->frames;
    int index = device_register(&node);
    display_node = index < 0 ? UINT32_MAX : (uint32_t)index;
}
static void register_input(void) {
    const char *name = virtio_input_name();
    ArkDeviceInfo node = {0};
    node.class_id = ARK_DEV_CLASS_INPUT;
    node.bus = ARK_BUS_PCI;
    node.flags = ARK_DEV_PRESENT | ARK_DEV_READABLE;
    node.state = ARK_DEV_STATE_OK;
    node.blocks = virtio_input_contacts();
    strcopy(node.name, name && name[0] ? name : "Pointer device", sizeof node.name);
    strcopy(node.driver, "virtio-input", sizeof node.driver);
    append_count(node.detail, sizeof node.detail, virtio_input_contacts(), "tracked contacts");
    contacts = virtio_input_contacts();
    int index = device_register(&node);
    input_node = index < 0 ? UINT32_MAX : (uint32_t)index;
    if (!spice_mouse_ready())
        return;
    node.bus = ARK_BUS_PCI;
    node.blocks = 1;
    node.detail[0] = 0;
    strcopy(node.name, "SPICE absolute pointer", sizeof node.name);
    strcopy(node.driver, "virtio-serial", sizeof node.driver);
    append_text(node.detail, sizeof node.detail, "port 1");
    device_register(&node);
}
static void register_volumes(void) {
    if (storage_mounted()) {
        ArkDeviceInfo node = {0};
        node.class_id = ARK_DEV_CLASS_VOLUME;
        node.bus = ARK_BUS_VIRTUAL;
        node.flags = ARK_DEV_PRESENT | ARK_DEV_READABLE | ARK_DEV_WRITABLE;
        node.state = ARK_DEV_STATE_OK;
        node.blocks = storage_capacity_bytes();
        strcopy(node.name, "ArkFS volume", sizeof node.name);
        strcopy(node.driver, "arkfs", sizeof node.driver);
        strcopy(node.detail, "mount /", sizeof node.detail);
        device_register(&node);
    }
    for (unsigned i = 0; i < 2; i++) {
        ExtVolumeInfo v;
        if (!extfs_volume_info(i, &v) || !v.mounted)
            continue;
        ArkDeviceInfo node = {0};
        node.class_id = ARK_DEV_CLASS_VOLUME;
        node.bus = ARK_BUS_VIRTUAL;
        node.flags = ARK_DEV_PRESENT | ARK_DEV_READABLE;
        node.state = ARK_DEV_STATE_OK;
        if (!v.read_only)
            node.flags |= ARK_DEV_WRITABLE;
        node.unit = i;
        node.blocks = v.capacity_bytes;
        strcopy(node.name, "External volume", sizeof node.name);
        strcopy(node.driver, v.read_only ? "fat32-or-ntfs-readonly" : "fat32-or-ntfs",
                sizeof node.driver);
        strcopy(node.detail, "mount ", sizeof node.detail);
        append_text(node.detail, sizeof node.detail, v.mountpoint ? v.mountpoint : "/mnt");
        if (v.read_only)
            append_text(node.detail, sizeof node.detail, "read-only");
        device_register(&node);
    }
}
static void devices_poll(void) {
    const GpuStats *g = gpu_stats();
    if (g && display_node != UINT32_MAX && g->frames != gpu_frames) {
        device_add_counters(display_node, 0, 0, g->frames - gpu_frames, 0);
        gpu_frames = g->frames;
    }
    unsigned now = virtio_input_contacts();
    if (input_node != UINT32_MAX && now != contacts) {
        contacts = now;
        device_notify(input_node);
    }
}
void devices_init(void) {
    register_block();
    register_display();
    register_input();
    register_volumes();
    device_register_poll(devices_poll);
}