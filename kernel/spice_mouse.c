/* Original ArkOS mouse transport, not a port of the Linux/Windows agent.
 * Wire facts: OASIS VIRTIO 1.2 sections 2.7, 4.1, 5.3; SPICE protocol
 * v0.14.4 spice/vd_agent.h; guest tests use an explicit protocol peer.
 * A second GPU disables SPICE's tablet-only client mouse path. Opening the
 * native agent port enables absolute mouse input without downgrading display.
 */
#include "spice_mouse.h"
#include "mmio.h"

#define RING_CAP 128u
#define BUFFER_CAP 512u
#define EVENT_CAP 64u
#define CHUNK_CAP 2048u
#define MESSAGE_CAP 65536u
typedef struct {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
} Descriptor;
typedef struct {
    uint16_t flags, index, ring[RING_CAP], used_event;
} Available;
typedef struct {
    uint32_t id, length;
} UsedElement;
typedef struct {
    uint16_t flags, index;
    UsedElement ring[RING_CAP];
    uint16_t avail_event;
} Used;
typedef struct {
    Descriptor descriptors[RING_CAP] __attribute__((aligned(16)));
    Available available __attribute__((aligned(4)));
    volatile Used used __attribute__((aligned(4)));
    uint8_t buffers[RING_CAP][BUFFER_CAP];
    bool busy[RING_CAP];
    uint16_t size, seen, index;
    volatile uint16_t *notify;
} Queue;
typedef struct {
    volatile uint8_t *address;
    uint32_t length;
} Region;
typedef struct {
    uint8_t header[20], body[16];
    unsigned header_used, body_used, size, type;
} Message;
typedef struct {
    uint8_t header[8];
    unsigned header_used, remaining, port;
    Message messages[2];
} Parser;
static Queue queues[4] __attribute__((aligned(4096))); /* control 2/3, port 1 4/5 */
static Region common, notification, device;
static uint32_t notify_multiplier;
static bool initialized, named, opened, host_open, caps_pending, caps_request, discarding;
static unsigned screen_width, screen_height;
static Parser parser;
static Event events[EVENT_CAP], last_mouse, delivered_mouse;
static unsigned event_read, event_write;

_Static_assert(sizeof(Descriptor) == 16, "VirtIO descriptor layout");
_Static_assert(offsetof(Used, ring) == 4, "VirtIO used ring layout");
static void barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}
static void out32(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(port));
}
static uint32_t in32(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static uint32_t pci_read(uint32_t bdf, unsigned offset) {
    out32(0xcf8, 0x80000000u | bdf | (offset & 0xfcu));
    return in32(0xcfc);
}
static uint8_t pci_byte(uint32_t bdf, unsigned offset) {
    return (uint8_t)(pci_read(bdf, offset) >> ((offset & 3) * 8));
}
static void pci_command(uint32_t bdf) {
    uint16_t value = (uint16_t)pci_read(bdf, 4) | 6 | 0x400;
    out32(0xcf8, 0x80000000u | bdf | 4);
    __asm__ volatile("outw %0,%1" ::"a"(value), "Nd"((uint16_t)0xcfc));
}
static uint16_t read16(Region r, unsigned at) {
    return *(volatile uint16_t *)(r.address + at);
}
static uint32_t read32(Region r, unsigned at) {
    return *(volatile uint32_t *)(r.address + at);
}
static void write16(Region r, unsigned at, uint16_t v) {
    *(volatile uint16_t *)(r.address + at) = v;
}
static void write32(Region r, unsigned at, uint32_t v) {
    *(volatile uint32_t *)(r.address + at) = v;
}
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (i * 8));
}
static void address(unsigned at, const volatile void *p) {
    uint64_t v = (uintptr_t)p;
    write32(common, at, (uint32_t)v);
    write32(common, at + 4, (uint32_t)(v >> 32));
}

static bool map_region(uint32_t bdf, unsigned cap, unsigned needed, Region *r) {
    unsigned bar = pci_byte(bdf, cap + 4);
    if (bar > 5)
        return false;
    for (unsigned i = 0; i < bar; i++) {
        uint32_t v = pci_read(bdf, 0x10 + i * 4);
        if (!(v & 1) && (v & 6) == 4) {
            if (i + 1 == bar)
                return false;
            i++;
        }
    }
    uint32_t low = pci_read(bdf, 0x10 + bar * 4);
    if ((low & 1) || (low & 6) == 2 || (low & 6) == 6 || ((low & 6) == 4 && bar == 5))
        return false;
    uint64_t base = low & ~15u;
    if ((low & 6) == 4)
        base |= (uint64_t)pci_read(bdf, 0x14 + bar * 4) << 32;
    uint32_t at = pci_read(bdf, cap + 8), length = pci_read(bdf, cap + 12);
    if (!base || length < needed || base > UINT64_MAX - at)
        return false;
    base += at;
    if (base > UINT64_MAX - length || !platform_map_mmio(base, needed))
        return false;
    *r = (Region){(volatile uint8_t *)(uintptr_t)base, length};
    return true;
}
static bool discover(uint32_t bdf) {
    common = (Region){0};
    notification = (Region){0};
    device = (Region){0};
    if (!(pci_read(bdf, 4) & 0x100000u))
        return false;
    bool visited[256] = {0};
    unsigned cap = pci_byte(bdf, 0x34) & 0xfcu;
    for (unsigned n = 0; cap && n < 48; n++) {
        if (cap < 0x40 || cap > 0xfc || visited[cap])
            return false;
        visited[cap] = true;
        unsigned length = pci_byte(bdf, cap + 2), type = pci_byte(bdf, cap + 3);
        if (pci_byte(bdf, cap) == 9 && length >= 16 && cap + length <= 256) {
            Region r;
            if (type == 1 && !common.address && map_region(bdf, cap, 56, &r) &&
                !((uintptr_t)r.address & 3))
                common = r;
            if (type == 2 && !notification.address && length >= 20 && map_region(bdf, cap, 2, &r) &&
                !((uintptr_t)r.address & 1)) {
                notification = r;
                notify_multiplier = pci_read(bdf, cap + 16);
            }
            if (type == 4 && !device.address && map_region(bdf, cap, 8, &r) &&
                !((uintptr_t)r.address & 3))
                device = r;
        }
        cap = pci_byte(bdf, cap + 1) & 0xfcu;
    }
    return common.address && notification.address && device.address;
}
static bool configure_queue(Queue *q, unsigned index) {
    memset(q, 0, sizeof(*q));
    write16(common, 22, (uint16_t)index);
    unsigned maximum = read16(common, 24);
    if (maximum < 16 || read16(common, 28))
        return false;
    unsigned size = 16;
    while (size * 2 <= maximum && size * 2 <= RING_CAP)
        size *= 2;
    q->size = (uint16_t)size;
    q->index = (uint16_t)index;
    uint64_t at = (uint64_t)read16(common, 30) * notify_multiplier;
    if (at + 2 > notification.length || (at & 1) ||
        !platform_map_mmio((uintptr_t)notification.address + at, 2))
        return false;
    q->notify = (volatile uint16_t *)(notification.address + at);
    q->available.flags = 1;
    for (unsigned i = 0; i < size; i++) {
        q->descriptors[i] =
            (Descriptor){(uintptr_t)q->buffers[i], BUFFER_CAP, (index & 1) ? 0 : 2, 0};
        if (!(index & 1)) {
            q->available.ring[i] = (uint16_t)i;
            q->busy[i] = true;
        }
    }
    if (!(index & 1))
        q->available.index = (uint16_t)size;
    write16(common, 24, (uint16_t)size);
    write16(common, 26, 0xffff);
    address(32, q->descriptors);
    address(40, &q->available);
    address(48, &q->used);
    barrier();
    write16(common, 28, 1);
    return read16(common, 28) == 1;
}
static unsigned event_free(void) {
    return (event_read + EVENT_CAP - event_write - 1) % EVENT_CAP;
}
static void emit(Event event) {
    if (discarding)
        return;
    if (event.type == EV_POINTER && event_read != event_write) {
        unsigned at = (event_write + EVENT_CAP - 1) % EVENT_CAP;
        Event *previous = &events[at];
        /* Coalesce hover/drag motion only. A button edge is never overwritten. */
        unsigned before = (at + EVENT_CAP - 1) % EVENT_CAP;
        if (previous->type == EV_POINTER && previous->buttons == event.buttons &&
            at != event_read && events[before].type == EV_POINTER &&
            events[before].buttons == event.buttons) {
            *previous = event;
            return;
        }
    }
    if (event_free()) {
        events[event_write] = event;
        event_write = (event_write + 1) % EVENT_CAP;
    }
}
static void disconnect(void) {
    memset(&parser, 0, sizeof(parser));
    event_read = event_write = 0;
    if (last_mouse.buttons || delivered_mouse.buttons) {
        Event release = last_mouse;
        release.buttons = 0;
        emit(release);
    }
    last_mouse.buttons = 0;
    delivered_mouse.buttons = 0;
}
static void fail(const char *reason) {
    initialized = opened = host_open = false;
    disconnect();
    if (common.address)
        common.address[20] |= 128;
    serial_write("[spice-input] ");
    serial_write(reason);
    serial_write("\n");
}
static bool reclaim(Queue *q) {
    uint16_t used = q->used.index;
    barrier();
    if ((uint16_t)(used - q->seen) > q->size) {
        fail("Invalid transmit queue index.");
        return false;
    }
    while (q->seen != used) {
        UsedElement entry = q->used.ring[q->seen % q->size];
        if (entry.id >= q->size || !q->busy[entry.id] || entry.length) {
            fail("Invalid transmit descriptor.");
            return false;
        }
        q->busy[entry.id] = false;
        q->seen++;
    }
    return true;
}
static bool send(Queue *q, const uint8_t *p, unsigned length) {
    if (length > BUFFER_CAP || !reclaim(q))
        return false;
    for (unsigned i = 0; i < q->size; i++)
        if (!q->busy[i]) {
            memcpy(q->buffers[i], p, length);
            q->descriptors[i].length = length;
            q->busy[i] = true;
            uint16_t at = q->available.index;
            q->available.ring[at % q->size] = (uint16_t)i;
            barrier();
            q->available.index = (uint16_t)(at + 1);
            barrier();
            *q->notify = q->index;
            return true;
        }
    return false;
}
static bool control_send(unsigned id, unsigned type, unsigned value) {
    uint8_t p[8];
    put32(p, id);
    put16(p + 4, (uint16_t)type);
    put16(p + 6, (uint16_t)value);
    return send(&queues[1], p, 8);
}
static void capabilities(void) {
    if (!opened || !host_open || !caps_pending)
        return;
    uint8_t p[36] = {0};
    put32(p, 1);
    put32(p + 4, 28);
    put32(p + 8, 1);
    put32(p + 12, 6);
    put32(p + 24, 8);
    put32(p + 28, caps_request ? 1 : 0);
    put32(p + 32, 1 | (1u << 13)); /* mouse, file transfer disabled */
    if (send(&queues[3], p, sizeof(p)))
        caps_pending = false;
}
static bool finish_message(Message *m, unsigned port) {
    if (m->type == 1 && port == 2) {
        if (m->size != 13)
            return false;
        if (m->body[12] != 0)
            return true; /* only the VMware desktop display */
        unsigned x = le32(m->body), y = le32(m->body + 4), mask = le32(m->body + 8);
#ifdef ARK_SPICE_INPUT_TRACE
        static unsigned traces;
        if (traces++ < 96) {
            char number[24];
            serial_write("[spice-trace] ");
            uint_to_str(x, number);
            serial_write(number);
            serial_write(",");
            uint_to_str(y, number);
            serial_write(number);
            serial_write(" mask=");
            uint_to_str(mask, number);
            serial_write(number);
            serial_write("\n");
        }
#endif
        if (mask & ~0xfeu)
            return false;
        if (x >= screen_width)
            x = screen_width - 1;
        if (y >= screen_height)
            y = screen_height - 1;
        uint8_t buttons = (uint8_t)(((mask >> 1) & 1) | ((mask >> 2) & 2) | ((mask & 4) ? 4 : 0) |
                                    ((mask & 64) ? 8 : 0) | ((mask & 128) ? 16 : 0));
        Event event = {
            EV_POINTER, 0, (int)(((uint64_t)x * 32767 + screen_width - 2) / (screen_width - 1)),
            (int)(((uint64_t)y * 32767 + screen_height - 2) / (screen_height - 1)), buttons};
        if (event.dx != last_mouse.dx || event.dy != last_mouse.dy || buttons != last_mouse.buttons)
            emit(event);
        last_mouse = event;
        if (mask & 48)
            emit((Event){EV_SCROLL, 0, 0, ((mask & 32) ? 1 : 0) - ((mask & 16) ? 1 : 0), buttons});
    } else if (m->type == 6) {
        if (m->size < 4 || (m->size & 3))
            return false;
        if (le32(m->body)) {
            caps_pending = true;
            caps_request = false;
        }
    } else if (m->type == 13 && port == 2) {
        if (m->size)
            return false;
        disconnect();
        caps_pending = true;
        caps_request = true;
    }
    return true;
}
/* Each VDP port has its own message reassembly; client messages may be
 * fragmented around server mouse chunks. Unused bodies are skipped in place. */
static bool message_byte(Message *m, unsigned port, uint8_t byte) {
    if (m->header_used < 20) {
        m->header[m->header_used++] = byte;
        if (m->header_used < 20)
            return true;
        if (le32(m->header) != 1)
            return false;
        m->type = le32(m->header + 4);
        m->size = le32(m->header + 16);
        m->body_used = 0;
        if (m->size > MESSAGE_CAP)
            return false;
        if (m->size)
            return true;
    } else {
        if (m->body_used < sizeof(m->body))
            m->body[m->body_used] = byte;
        if (++m->body_used < m->size)
            return true;
    }
    bool valid = finish_message(m, port);
    memset(m, 0, sizeof(*m));
    return valid;
}
static bool receive_bytes(const uint8_t *p, unsigned length) {
    for (unsigned i = 0; i < length; i++) {
        if (parser.header_used < 8) {
            parser.header[parser.header_used++] = p[i];
            if (parser.header_used < 8)
                continue;
            parser.port = le32(parser.header);
            parser.remaining = le32(parser.header + 4);
            if (parser.port < 1 || parser.port > 2 || parser.remaining > CHUNK_CAP)
                return false;
            if (!parser.remaining)
                parser.header_used = 0;
        } else {
            unsigned port = parser.port;
            /* A disconnect resets parser from the message callback. */
            if (!message_byte(&parser.messages[port - 1], port, p[i]))
                return false;
            if (parser.remaining && !--parser.remaining)
                parser.header_used = 0;
        }
    }
    return true;
}
static bool control_receive(const uint8_t *p, unsigned length) {
    if (length < 8)
        return false;
    unsigned id = le32(p), type = le16(p + 4), value = le16(p + 6);
    if (id != 1)
        return true;
    if (type == 1)
        return control_send(1, 3, 1);
    if (type == 7) {
        const char name[] = "com.redhat.spice.0";
        if (length != 8 + sizeof(name) || memcmp(p + 8, name, sizeof(name)))
            return true;
        if (!opened) {
            named = true;
            if (!control_send(1, 6, 1))
                return false;
            opened = true;
            caps_pending = caps_request = true;
        }
    } else if (type == 6) {
        if (!value) {
            host_open = false;
            disconnect();
            serial_write("[spice-input] Mouse channel disconnected.\n");
        } else if (!host_open) {
            host_open = true;
            caps_pending = caps_request = true;
            serial_write("[spice-input] Native absolute mouse channel connected.\n");
        }
    } else if (type == 2) {
        named = opened = host_open = false;
        disconnect();
    }
    return true;
}
static void poll_receive(Queue *q, bool control) {
    bool consumed = false;
    for (unsigned n = 0; n < 4 && initialized; n++) {
        /* A chunk can contain consecutive 33-byte mouse messages. With a
         * partially assembled message, 512 bytes yield at most 32 events. */
        if (!control && event_free() < 32)
            break;
        uint16_t used = q->used.index;
        if (q->seen == used)
            break;
        barrier();
        if ((uint16_t)(used - q->seen) > q->size) {
            fail("Invalid receive queue index.");
            break;
        }
        UsedElement entry = q->used.ring[q->seen % q->size];
        if (entry.id >= q->size || entry.length > BUFFER_CAP) {
            fail("Invalid receive descriptor.");
            break;
        }
        bool valid = !entry.length || (control ? control_receive(q->buffers[entry.id], entry.length)
                                               : receive_bytes(q->buffers[entry.id], entry.length));
        if (!valid) {
            fail("Invalid mouse channel message.");
            break;
        }
        q->seen++;
        uint16_t at = q->available.index;
        q->available.ring[at % q->size] = (uint16_t)entry.id;
        barrier();
        q->available.index = (uint16_t)(at + 1);
        consumed = true;
    }
    if (consumed && initialized) {
        barrier();
        *q->notify = q->index;
    }
}
static bool initialize(uint32_t bdf) {
    if (!discover(bdf) || read32(device, 4) != 2)
        return false;
    pci_command(bdf);
    common.address[20] = 0;
    barrier();
    unsigned n = 100000;
    while (common.address[20] && --n)
        __asm__ volatile("pause");
    if (!n)
        return false;
    common.address[20] = 1;
    common.address[20] = 3;
    write32(common, 0, 0);
    if (!(read32(common, 4) & 2))
        return false;
    write32(common, 0, 1);
    if (!(read32(common, 4) & 1))
        return false;
    write32(common, 8, 0);
    write32(common, 12, 2);
    write32(common, 8, 1);
    write32(common, 12, 1);
    common.address[20] = 11;
    barrier();
    if ((common.address[20] & 0xc8) != 8)
        return false;
    for (unsigned i = 0; i < 4; i++)
        if (!configure_queue(&queues[i], i + 2))
            return false;
    common.address[20] = 15;
    barrier();
    if (common.address[20] & 0xc0)
        return false;
    initialized = true;
    *queues[0].notify = 2;
    *queues[2].notify = 4;
    if (!control_send(0, 0, 1)) {
        fail("Unable to announce driver.");
        return false;
    }
    serial_write("[spice-input] Native VirtIO-serial mouse transport ready.\n");
    return true;
}
bool spice_mouse_init(unsigned width, unsigned height) {
    if (initialized)
        return true;
    if (width < 2 || height < 2 || width > 3840 || height > 2160)
        return false;
    screen_width = width;
    screen_height = height;
    last_mouse = delivered_mouse = (Event){EV_POINTER, 0, 16384, 16384, 0};
    for (unsigned bus = 0; bus < 256; bus++)
        for (unsigned slot = 0; slot < 32; slot++) {
            uint32_t base = (bus << 16) | (slot << 11);
            if ((pci_read(base, 0) & 0xffff) == 0xffff)
                continue;
            unsigned functions = (pci_byte(base, 0x0e) & 0x80) ? 8 : 1;
            for (unsigned fn = 0; fn < functions; fn++) {
                uint32_t bdf = base | (fn << 8);
                /* The launcher supplies one dedicated, modern-only controller.
                 * Leave UTM's transitional QEMU guest-agent controller untouched. */
                if (pci_read(bdf, 0) == 0x10431af4) {
                    if (initialize(bdf))
                        return true;
                    if (common.address)
                        common.address[20] = 128;
                }
            }
        }
    return false;
}
bool spice_mouse_next_event(Event *event) {
    if (!event)
        return false;
    if (initialized) {
        if (common.address[20] & 0xc0)
            fail("Mouse device needs reset.");
        else {
            poll_receive(&queues[0], true);
            if (initialized) {
                poll_receive(&queues[2], false);
                capabilities();
            }
        }
    }
    if (event_read == event_write)
        return false;
    *event = events[event_read];
    event_read = (event_read + 1) % EVENT_CAP;
    if (event->type == EV_POINTER)
        delivered_mouse = *event;
    return true;
}
bool spice_mouse_ready(void) {
    return initialized && named && opened && host_open;
}
void spice_mouse_discard_events(void) {
    event_read = event_write = 0;
    discarding = true;
    /* Consume existing DMA bytes without discarding half a protocol header.
     * This is bounded by the receive ring, including on touch takeover/lift. */
    if (initialized)
        for (unsigned i = 0; i < RING_CAP / 4; i++)
            poll_receive(&queues[2], false);
    discarding = false;
}
