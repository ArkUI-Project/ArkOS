/* ArkOS native virtio-input PCI driver, x86_64, firmware-assigned PCI BARs.
 * Protocol: OASIS VIRTIO 1.2 sections 2.7, 3.1, 4.1 and 5.8.
 * All queues are split rings; packed rings, event_idx, MSI-X and IOMMU
 * translation are deliberately not negotiated. No Linux kernel dependency.
 */
#include "virtio_input.h"
#include "mmio.h"
#include "spice_mouse.h"

#define QUEUE_CAP 128u
#define CONTACT_CAP 16u
#define OUTPUT_CAP 64u
#define INPUT_ABS 3u
#define ABS_X 0u
#define ABS_Y 1u
#define ABS_MT_SLOT 0x2fu
#define ABS_MT_POSITION_X 0x35u
#define ABS_MT_POSITION_Y 0x36u
#define ABS_MT_TRACKING_ID 0x39u

typedef struct {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
} Descriptor;
typedef struct {
    uint16_t flags, index, ring[QUEUE_CAP], used_event;
} Available;
typedef struct {
    uint32_t id, length;
} UsedElement;
typedef struct {
    uint16_t flags, index;
    UsedElement ring[QUEUE_CAP];
    uint16_t avail_event;
} Used;
typedef struct {
    uint16_t type, code;
    int32_t value;
} InputEvent;
typedef struct {
    Descriptor descriptors[QUEUE_CAP] __attribute__((aligned(16)));
    Available available __attribute__((aligned(4)));
    volatile Used used __attribute__((aligned(4)));
    volatile InputEvent events[QUEUE_CAP] __attribute__((aligned(8)));
    uint16_t size, seen;
    volatile uint16_t *notify;
} Queue;
typedef struct {
    int32_t minimum, maximum;
} Axis;
typedef struct {
    int32_t id, x, y;
    bool active;
} Contact;
typedef struct {
    volatile uint8_t *address;
    uint32_t length;
} Region;

_Static_assert(sizeof(Descriptor) == 16, "Virtio descriptor layout");
_Static_assert(sizeof(InputEvent) == 8, "Virtio input layout");
_Static_assert(offsetof(Used, ring) == 4, "Virtio used ring layout");
#define DEVICE_CAP 3u
extern void platform_keyboard_input(unsigned, int) __attribute__((weak));
extern void platform_keyboard_device_input(unsigned, unsigned, int) __attribute__((weak));
/* Tablet and touchscreen keep separate PCI mappings, DMA rings and state. */
typedef struct {
    Queue queues[2] __attribute__((aligned(4096)));
    Region common, device, notification, isr;
    uint32_t bdf;
    uint32_t notify_multiplier;
    bool ready, multitouch, frame_dirty, dropping, wait_release, suppress_output, discard_frame;
    char name[129];
    Contact contacts[CONTACT_CAP];
    Axis x_axis, y_axis;
    unsigned current_slot, contact_count;
    int primary_slot, tablet_x, tablet_y, event_type;
    uint8_t buttons;
    bool keys[128];
    uint16_t discard_until;
    bool discarding;
    Event last_event;
} InputDevice;
typedef struct {
    Event event;
    bool edge;
    InputDevice *source;
} QueuedEvent;
static InputDevice devices[DEVICE_CAP] __attribute__((aligned(4096)));
static InputDevice spice_pointer;
static unsigned device_count;
static QueuedEvent output[OUTPUT_CAP];
static unsigned output_read, output_write;
static Event last_output;
static InputDevice *last_source;
static bool touch_priority;
static char device_names[260];

static inline void barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}
static inline void pause_cpu(void) {
    __asm__ volatile("pause");
}
static inline void out32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0,%1" ::"a"(value), "Nd"(port));
}
static inline uint32_t in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void out16(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0,%1" ::"a"(value), "Nd"(port));
}
static uint32_t pci_read(uint32_t bdf, unsigned offset) {
    out32(0xcf8, 0x80000000u | bdf | (offset & 0xfcu));
    return in32(0xcfc);
}
static uint8_t pci_byte(uint32_t bdf, unsigned offset) {
    return (uint8_t)(pci_read(bdf, offset) >> ((offset & 3) * 8));
}
static void pci_write16(uint32_t bdf, unsigned offset, uint16_t value) {
    out32(0xcf8, 0x80000000u | bdf | (offset & 0xfcu));
    out16((uint16_t)(0xcfc + (offset & 2)), value);
}
static uint16_t read16(Region r, unsigned offset) {
    return *(volatile uint16_t *)(r.address + offset);
}
static uint32_t read32(Region r, unsigned offset) {
    return *(volatile uint32_t *)(r.address + offset);
}
static void write16(Region r, unsigned offset, uint16_t value) {
    *(volatile uint16_t *)(r.address + offset) = value;
}
static void write32(Region r, unsigned offset, uint32_t value) {
    *(volatile uint32_t *)(r.address + offset) = value;
}
static void write_address(InputDevice *d, unsigned offset, const volatile void *pointer) {
    uint64_t address = (uint64_t)(uintptr_t)pointer;
    write32(d->common, offset, (uint32_t)address);
    write32(d->common, offset + 4, (uint32_t)(address >> 32));
}

static bool map_capability(uint32_t bdf, unsigned cap, Region *region) {
    unsigned bar = pci_byte(bdf, cap + 4);
    if (bar > 5)
        return false;
    /* A 64-bit BAR's high word is not itself an addressable BAR. */
    for (unsigned i = 0; i < bar; i++) {
        uint32_t previous = pci_read(bdf, 0x10 + i * 4);
        if (!(previous & 1) && (previous & 6) == 4) {
            if (i + 1 == bar)
                return false;
            i++;
        }
    }
    uint32_t low = pci_read(bdf, 0x10 + bar * 4);
    if ((low & 1) || (low & 6) == 2 || (low & 6) == 6)
        return false;
    if ((low & 6) == 4 && bar == 5)
        return false;
    uint64_t base = low & ~15u;
    if ((low & 6) == 4)
        base |= (uint64_t)pci_read(bdf, 0x14 + bar * 4) << 32;
    uint32_t offset = pci_read(bdf, cap + 8), length = pci_read(bdf, cap + 12);
    if (base > UINT64_MAX - offset)
        return false;
    uint64_t begin = base + offset;
    if (!base || !length || begin > UINT64_MAX - length)
        return false;
    /* OVMF may place BAR4 far above 4 GiB (QEMU commonly uses 768 GiB).
     * Map the firmware-assigned range; never relocate it over another BAR.
     * Only map the fields needed now. A queue's notify word is mapped when
     * its offset is known, so an oversized advertised capability is harmless. */
    unsigned type = pci_byte(bdf, cap + 3);
    unsigned required = type == 1 ? 56 : type == 2 ? 2 : type == 3 ? 1 : type == 4 ? 136 : 0;
    if (!required || length < required || !platform_map_mmio(begin, required))
        return false;
    region->address = (volatile uint8_t *)(uintptr_t)begin;
    region->length = length;
    return true;
}

static bool discover_regions(InputDevice *d, uint32_t bdf) {
    d->common = (Region){0};
    d->device = (Region){0};
    d->notification = (Region){0};
    d->isr = (Region){0};
    if (!(pci_read(bdf, 4) & 0x100000u))
        return false;
    uint8_t visited[256] = {0};
    unsigned cap = pci_byte(bdf, 0x34) & 0xfcu;
    for (unsigned count = 0; cap && count < 48; count++) {
        if (cap < 0x40 || cap > 0xfc || visited[cap])
            return false;
        visited[cap] = 1;
        unsigned id = pci_byte(bdf, cap), next = pci_byte(bdf, cap + 1) & 0xfcu;
        if (id == 9) {
            unsigned length = pci_byte(bdf, cap + 2), type = pci_byte(bdf, cap + 3);
            if (length >= 16 && cap + length <= 256) {
                Region region;
                if (map_capability(bdf, cap, &region)) {
                    if (type == 1 && !d->common.address && region.length >= 56 &&
                        !((uintptr_t)region.address & 3))
                        d->common = region;
                    if (type == 2 && !d->notification.address && length >= 20 &&
                        region.length >= 2 && !((uintptr_t)region.address & 1)) {
                        d->notification = region;
                        d->notify_multiplier = pci_read(bdf, cap + 16);
                    }
                    if (type == 3 && !d->isr.address)
                        d->isr = region;
                    if (type == 4 && !d->device.address && region.length >= 136 &&
                        !((uintptr_t)region.address & 3))
                        d->device = region;
                }
            }
        }
        cap = next;
    }
    return d->common.address && d->notification.address && d->device.address && d->isr.address;
}

/* Configuration selectors can themselves change config_generation. Read it
 * after writing both selector bytes, and retry the data snapshot on change. */
static unsigned configuration(InputDevice *d, uint8_t select, uint8_t subsel, uint8_t out[128]) {
    for (unsigned tries = 0; tries < 8; tries++) {
        d->device.address[0] = select;
        d->device.address[1] = subsel;
        barrier();
        uint8_t generation = d->common.address[21];
        unsigned size = d->device.address[2];
        if (size > 128)
            return 0;
        for (unsigned i = 0; i < size; i++)
            out[i] = d->device.address[8 + i];
        barrier();
        if (generation == d->common.address[21])
            return size;
    }
    return 0;
}
static bool has_bit(const uint8_t *bits, unsigned size, unsigned bit) {
    return bit / 8 < size && (bits[bit / 8] & (1u << (bit & 7))) != 0;
}
static int32_t little_i32(const uint8_t *b) {
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
                     ((uint32_t)b[3] << 24));
}
static bool axis_info(InputDevice *d, unsigned axis, Axis *out) {
    uint8_t data[128];
    if (configuration(d, 0x12, (uint8_t)axis, data) < 20)
        return false;
    out->minimum = little_i32(data);
    out->maximum = little_i32(data + 4);
    return out->maximum > out->minimum;
}
static int normalize(int32_t value, Axis axis) {
    if (value <= axis.minimum)
        return 0;
    if (value >= axis.maximum)
        return 32767;
    return (int)(((int64_t)value - axis.minimum) * 32767 / ((int64_t)axis.maximum - axis.minimum));
}

static bool configure_queue(InputDevice *d, unsigned index) {
    Queue *q = &d->queues[index];
    memset(q, 0, sizeof(*q));
    write16(d->common, 22, (uint16_t)index);
    unsigned maximum = read16(d->common, 24);
    if (!maximum || read16(d->common, 28))
        return false;
    unsigned size = 1;
    while (size * 2 <= maximum && size * 2 <= QUEUE_CAP)
        size *= 2;
    if (index == 0 && size < 16)
        return false;
    q->size = (uint16_t)size;
    uint64_t offset = (uint64_t)read16(d->common, 30) * d->notify_multiplier;
    if (offset + 2 > d->notification.length || (offset & 1))
        return false;
    if (!platform_map_mmio((uintptr_t)d->notification.address + offset, 2))
        return false;
    q->notify = (volatile uint16_t *)(d->notification.address + offset);
    q->available.flags = 1; /* VIRTQ_AVAIL_F_NO_INTERRUPT: polled on each tick. */
    if (index == 0) {
        for (unsigned i = 0; i < size; i++) {
            q->descriptors[i] = (Descriptor){(uintptr_t)&q->events[i], 8, 2, 0};
            q->available.ring[i] = (uint16_t)i;
        }
        q->available.index = (uint16_t)size;
    }
    write16(d->common, 24, (uint16_t)size);
    write16(d->common, 26, 0xffff); /* VIRTIO_MSI_NO_VECTOR. */
    write_address(d, 32, q->descriptors);
    write_address(d, 40, &q->available);
    write_address(d, 48, &q->used);
    barrier();
    write16(d->common, 28, 1);
    return read16(d->common, 28) == 1;
}

static unsigned output_free(void) {
    return (output_read + OUTPUT_CAP - output_write - 1) % OUTPUT_CAP;
}
static void queue_event(InputDevice *d, Event event, bool edge) {
    if (output_read != output_write && !edge) {
        unsigned previous = (output_write + OUTPUT_CAP - 1) % OUTPUT_CAP;
        QueuedEvent *last = &output[previous];
        if (!last->edge && last->source == d && last->event.type == event.type &&
            last->event.buttons == event.buttons && last->event.key == event.key) {
            last->event = event;
            last_output = event;
            last_source = d;
            return;
        }
    }
    /* poll_device reserves room for every edge a complete frame can create. */
    if (!output_free())
        return;
    output[output_write] = (QueuedEvent){event, edge, d};
    output_write = (output_write + 1) % OUTPUT_CAP;
    last_output = event;
    last_source = d;
}
static void refresh_priority(void) {
    bool active = false, pointer = spice_pointer.ready;
    for (unsigned i = 0; i < device_count; i++)
        if (devices[i].ready) {
            if (devices[i].event_type == EV_POINTER)
                pointer = true;
            else if (devices[i].contact_count)
                active = true;
        }
    if (active != touch_priority) {
        if (spice_pointer.last_event.buttons)
            spice_pointer.wait_release = true;
        spice_mouse_discard_events();
        /* Discard mouse frames already in DMA rings at BOTH boundaries.
         * Otherwise releasing a finger can replay old hover coordinates and
         * snap the cursor back to the mouse's position before the gesture. */
        for (unsigned i = 0; i < device_count; i++) {
            InputDevice *p = &devices[i];
            if (p->ready && p->event_type == EV_POINTER) {
                p->discard_until = p->queues[0].used.index;
                p->discarding = p->queues[0].seen != p->discard_until;
                if (p->buttons)
                    p->wait_release = true;
            }
        }
        if (active && last_source && last_output.type == EV_POINTER && last_output.buttons) {
            Event release = last_output;
            release.buttons = 0;
            queue_event(last_source, release, true);
        }
        touch_priority = active;
    }
    platform_set_relative_pointer_enabled(!pointer && !active);
}
static void push_event(InputDevice *d, int x, int y, uint8_t buttons, int id) {
    Event event = {d->event_type, id, x, y, buttons};
    bool edge = event.buttons != d->last_event.buttons || event.key != d->last_event.key;
    bool changed = edge || event.dx != d->last_event.dx || event.dy != d->last_event.dy;
    d->last_event = event;
    if (d->event_type == EV_POINTER) {
        if (touch_priority || d->suppress_output) {
            d->wait_release = buttons != 0;
            return;
        }
        if (d->wait_release) {
            if (buttons)
                return;
            d->wait_release = false;
        }
    }
    if (!changed)
        return;
    /* A source switch terminates any prior drag before moving the cursor. */
    if (last_source && last_source != d && last_output.buttons) {
        Event release = last_output;
        release.buttons = 0;
        queue_event(last_source, release, true);
    }
    queue_event(d, event, edge || last_source != d);
}
static void keyboard_emit(InputDevice *d, unsigned code, int value) {
    if (platform_keyboard_device_input)
        platform_keyboard_device_input(1u + (unsigned)(d - devices), code, value);
    else if (platform_keyboard_input)
        platform_keyboard_input(code, value);
}
static void release_all(InputDevice *d) {
    if (d->event_type == EV_KEY)
        for (unsigned i = 0; i < 128; i++)
            if (d->keys[i]) {
                keyboard_emit(d, i, 0);
                d->keys[i] = false;
            }
    for (unsigned i = 0; i < CONTACT_CAP; i++)
        d->contacts[i].active = false;
    d->buttons = 0;
    d->contact_count = 0;
    d->primary_slot = -1;
    if (d->last_event.buttons)
        push_event(d, d->last_event.dx, d->last_event.dy, 0, d->last_event.key);
    refresh_priority();
}
static void finish_frame(InputDevice *d) {
    if (!d->frame_dirty)
        return;
    d->frame_dirty = false;
    if (!d->multitouch) {
        d->contact_count = d->event_type == EV_TOUCH && (d->buttons & 1) ? 1 : 0;
        if (d->event_type == EV_TOUCH)
            refresh_priority();
        push_event(d, d->tablet_x, d->tablet_y, d->buttons, 0);
        return;
    }
    d->contact_count = 0;
    for (unsigned i = 0; i < CONTACT_CAP; i++)
        if (d->contacts[i].active)
            d->contact_count++;
    refresh_priority();
    if (d->primary_slot >= 0 && (!d->contacts[d->primary_slot].active ||
                                 d->contacts[d->primary_slot].id != d->last_event.key)) {
        push_event(d, d->last_event.dx, d->last_event.dy, 0, d->last_event.key);
        d->primary_slot = -1;
    }
    if (d->primary_slot < 0) {
        for (unsigned i = 0; i < CONTACT_CAP; i++)
            if (d->contacts[i].active) {
                d->primary_slot = (int)i;
                break;
            }
    }
    if (d->primary_slot >= 0) {
        Contact *c = &d->contacts[d->primary_slot];
        push_event(d, c->x, c->y, 1, c->id);
    }
}
static void receive_event(InputDevice *d, InputEvent event) {
    if (event.type == 0 && event.code == 3) {
        release_all(d);
        d->dropping = true;
        d->frame_dirty = false;
        return;
    }
    if (event.type == 0 && event.code == 0) {
        if (d->dropping)
            d->dropping = false;
        else
            finish_frame(d);
        d->discard_frame = false;
        return;
    }
    if (d->dropping)
        return;
    if (d->event_type == EV_KEY) {
        if (event.type == 1 && event.code < 128 && event.value >= 0 && event.value <= 2) {
            d->keys[event.code] = event.value != 0;
            keyboard_emit(d, event.code, event.value);
        }
        return;
    }
    if (event.type == 2 && !d->multitouch && (event.code == 8 || event.code == 6)) {
        if (!touch_priority && !d->suppress_output && event.value) {
            int v = event.value;
            if (v > 127)
                v = 127;
            if (v < -127)
                v = -127;
            queue_event(d,
                        (Event){EV_SCROLL, 0, event.code == 6 ? v : 0, event.code == 8 ? -v : 0,
                                d->buttons},
                        true);
        }
    } else if (event.type == 1 && !d->multitouch) {
        unsigned bit = 0;
        if (d->event_type == EV_POINTER && event.code >= 0x110 && event.code <= 0x114)
            bit = 1u << (event.code - 0x110);
        else if (d->event_type == EV_TOUCH && (event.code == 0x14a || event.code == 0x110))
            bit = 1;
        if (bit) {
            if (event.value)
                d->buttons |= (uint8_t)bit;
            else
                d->buttons &= (uint8_t)~bit;
            d->frame_dirty = true;
        }
    } else if (event.type == INPUT_ABS) {
        if (!d->multitouch) {
            if (event.code == ABS_X) {
                d->tablet_x = normalize(event.value, d->x_axis);
                d->frame_dirty = true;
            }
            if (event.code == ABS_Y) {
                d->tablet_y = normalize(event.value, d->y_axis);
                d->frame_dirty = true;
            }
        } else if (event.code == ABS_MT_SLOT) {
            d->current_slot = event.value >= 0 && event.value < (int)CONTACT_CAP
                                  ? (unsigned)event.value
                                  : CONTACT_CAP;
        } else if (d->current_slot < CONTACT_CAP) {
            Contact *c = &d->contacts[d->current_slot];
            if (event.code == ABS_MT_TRACKING_ID) {
                c->active = event.value >= 0;
                c->id = event.value;
                d->frame_dirty = true;
            } else if (event.code == ABS_MT_POSITION_X) {
                c->x = normalize(event.value, d->x_axis);
                d->frame_dirty = true;
            } else if (event.code == ABS_MT_POSITION_Y) {
                c->y = normalize(event.value, d->y_axis);
                d->frame_dirty = true;
            }
        }
    }
}
static void fail_device(InputDevice *d, const char *reason) {
    d->ready = false;
    if (d->common.address)
        d->common.address[20] |= 128;
    d->suppress_output = false;
    d->wait_release = false;
    release_all(d);
    serial_write("[input] ");
    serial_write(reason);
    serial_write("\n");
}

static bool initialize_device(InputDevice *d, uint32_t bdf, int wanted_type) {
    memset(d, 0, sizeof(*d));
    d->bdf = bdf;
    d->primary_slot = -1;
    d->tablet_x = d->tablet_y = 16384;
    for (unsigned i = 0; i < CONTACT_CAP; i++)
        d->contacts[i].x = d->contacts[i].y = 16384;
    if (!discover_regions(d, bdf))
        return false;
    uint16_t command = (uint16_t)pci_read(bdf, 4);
    pci_write16(bdf, 4, (uint16_t)(command | 6 | 0x400)); /* Memory, DMA, no INTx. */
    d->common.address[20] = 0;
    barrier();
    unsigned timeout = 100000;
    while (d->common.address[20] && --timeout)
        pause_cpu();
    if (!timeout)
        return false;
    d->common.address[20] = 1;
    d->common.address[20] = 3;
    write32(d->common, 0, 1);
    if (!(read32(d->common, 4) & 1)) {
        d->common.address[20] = 128;
        return false;
    }
    write32(d->common, 8, 0);
    write32(d->common, 12, 0);
    write32(d->common, 8, 1);
    write32(d->common, 12, 1); /* VIRTIO_F_VERSION_1 only. */
    d->common.address[20] = 11;
    barrier();
    if (!(d->common.address[20] & 8) || (d->common.address[20] & 0xc0))
        return false;
    uint8_t bits[128];
    unsigned size = configuration(d, 0x11, INPUT_ABS, bits);
    d->multitouch = has_bit(bits, size, ABS_MT_SLOT) && has_bit(bits, size, ABS_MT_TRACKING_ID) &&
                    has_bit(bits, size, ABS_MT_POSITION_X) &&
                    has_bit(bits, size, ABS_MT_POSITION_Y);
    bool tablet = has_bit(bits, size, ABS_X) && has_bit(bits, size, ABS_Y);
    if (!d->multitouch && !tablet) {
        size = configuration(d, 0x11, 1, bits);
        if (!has_bit(bits, size, 30) || !has_bit(bits, size, 28)) {
            d->common.address[20] = 0;
            return false;
        }
        d->event_type = EV_KEY;
    } else {
        if (!axis_info(d, d->multitouch ? ABS_MT_POSITION_X : ABS_X, &d->x_axis) ||
            !axis_info(d, d->multitouch ? ABS_MT_POSITION_Y : ABS_Y, &d->y_axis)) {
            d->common.address[20] = 0;
            return false;
        }
        size = configuration(d, 0x10, 0, bits);
        d->event_type = (d->multitouch || has_bit(bits, size, 1)) ? EV_TOUCH : EV_POINTER;
    }
    if (d->event_type != wanted_type) {
        d->common.address[20] = 0;
        return false;
    }
    d->last_event = (Event){d->event_type, 0, 16384, 16384, 0};
    size = configuration(d, 1, 0, bits);
    for (unsigned i = 0; i < size; i++)
        d->name[i] = (char)bits[i];
    d->name[size] = 0;
    if (!size)
        strcopy(d->name, "Virtio absolute input", sizeof(d->name));
    if (read16(d->common, 18) < 2 || !configure_queue(d, 0) || !configure_queue(d, 1)) {
        d->common.address[20] = 128;
        return false;
    }
    d->common.address[20] = 15;
    barrier();
    if (d->common.address[20] & 0xc0)
        return false;
    *d->queues[0].notify = 0;
    d->ready = true;
    serial_write("[input] Native virtio PCI: ");
    serial_write(d->name);
    serial_write(d->event_type == EV_KEY       ? "; native keyboard\n"
                 : d->event_type == EV_POINTER ? "; absolute mouse pointer\n"
                 : d->multitouch               ? "; multitouch slots (primary pointer)\n"
                                               : "; direct touch\n");
    return true;
}

static void poll_device(InputDevice *d) {
    if (!d->ready || output_free() < 8)
        return;
    if (d->common.address[20] & 0xc0) {
        fail_device(d, "Input device needs reset.");
        return;
    }
    Queue *q = &d->queues[0];
    unsigned consumed = 0;
    unsigned before = output_write;
    for (; consumed < QUEUE_CAP; consumed++) {
        uint16_t used_index = q->used.index;
        if (q->seen == used_index)
            break;
        barrier();
        if ((uint16_t)(used_index - q->seen) > q->size) {
            fail_device(d, "Invalid input queue index.");
            break;
        }
        UsedElement used = q->used.ring[q->seen % q->size];
        if (used.id >= q->size || used.length > sizeof(InputEvent)) {
            fail_device(d, "Invalid input descriptor.");
            break;
        }
        if (d->discarding && q->seen == d->discard_until)
            d->discarding = false;
        if (d->discarding)
            d->discard_frame = true;
        d->suppress_output = d->discarding || d->discard_frame;
        InputEvent input = q->events[used.id];
        q->seen++;
        if (used.length == sizeof(InputEvent))
            receive_event(d, input);
        uint16_t available = q->available.index;
        q->available.ring[available % q->size] = (uint16_t)used.id;
        barrier();
        q->available.index = (uint16_t)(available + 1);
        /* One full frame per device per poll prevents either device from
         * starving the other, while every click/drag boundary stays ordered. */
        if (input.type == 0 && input.code == 0 && before != output_write) {
            consumed++;
            break;
        }
    }
    d->suppress_output = false;
    if (consumed && d->ready) {
        barrier();
        *q->notify = 0;
    }
    if (d->isr.address)
        (void)d->isr.address[0];
}
bool virtio_input_init(void) {
    if (device_count)
        return true;
    output_read = output_write = 0;
    touch_priority = false;
    last_source = 0;
    memset(&last_output, 0, sizeof(last_output));
    device_names[0] = 0;
    /* Activate tablet last: QEMU's regular BTN events must go to the mouse,
     * whereas MTT events are routed exclusively to the touchscreen. */
    const int wanted[] = {EV_TOUCH, EV_POINTER, EV_KEY};
    for (unsigned pass = 0; pass < 3; pass++) {
        bool found = false;
        for (unsigned bus = 0; bus < 256 && !found; bus++)
            for (unsigned slot = 0; slot < 32 && !found; slot++) {
                uint32_t base = (bus << 16) | (slot << 11);
                if ((pci_read(base, 0) & 0xffff) == 0xffff)
                    continue;
                unsigned functions = (pci_byte(base, 0x0e) & 0x80) ? 8 : 1;
                for (unsigned function = 0; function < functions && !found; function++) {
                    uint32_t bdf = base | (function << 8);
                    bool owned = false;
                    for (unsigned i = 0; i < device_count; i++)
                        if (devices[i].bdf == bdf)
                            owned = true;
                    if (owned)
                        continue; /* Never reset an already active device. */
                    if (pci_read(bdf, 0) == 0x10521af4 &&
                        initialize_device(&devices[device_count], bdf, wanted[pass])) {
                        if (device_names[0])
                            strcopy(device_names + strlen(device_names), " + ",
                                    sizeof(device_names) - strlen(device_names));
                        strcopy(device_names + strlen(device_names), devices[device_count].name,
                                sizeof(device_names) - strlen(device_names));
                        device_count++;
                        found = true;
                    }
                }
            }
    }
    refresh_priority();
    if (!device_count)
        serial_write("[input] No virtio pointer; native PS/2 fallback.\n");
    return device_count != 0;
}
bool virtio_input_next_event(Event *event) {
    if (!event)
        return false;
    /* Process both devices even when one has output, so mouse frames are
     * consumed while touch owns the cursor rather than replayed afterwards. */
    for (unsigned i = 0; i < device_count; i++)
        poll_device(&devices[i]);
    if (output_free() >= 8) {
        Event incoming;
        bool received = spice_mouse_next_event(&incoming);
        bool ready = spice_mouse_ready();
        if (spice_pointer.ready != ready) {
            spice_pointer.ready = ready;
            refresh_priority();
        }
        if (received) {
            spice_pointer.event_type = EV_POINTER;
            if (incoming.type == EV_POINTER)
                push_event(&spice_pointer, incoming.dx, incoming.dy, incoming.buttons, 0);
            else if (incoming.type == EV_SCROLL && !touch_priority && !spice_pointer.wait_release)
                queue_event(&spice_pointer, incoming, true);
        }
    }
    if (output_read == output_write)
        return false;
    *event = output[output_read].event;
    output_read = (output_read + 1) % OUTPUT_CAP;
    return true;
}
const char *virtio_input_name(void) {
    return device_count ? device_names : "Unavailable";
}
unsigned virtio_input_contacts(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < device_count; i++)
        if (devices[i].ready)
            count += devices[i].contact_count;
    return count;
}
bool virtio_input_pointer_ready(void) {
    for (unsigned i = 0; i < device_count; i++)
        if (devices[i].ready && devices[i].event_type == EV_POINTER)
            return true;
    return false;
}
