/* Original ArkOS VirtIO-GPU PCI + fixed VirGL compositor. OASIS VirtIO 1.2
 * split queues; MIT VirGL wire definitions live in third_party. Only kernel
 * staging pages are DMA targets. Guest shaders, geometry and resources execute
 * through the virtual GPU; no host UI/service protocol is used. */
#include "virtio_gpu.h"
#include "process.h"
#include "mmio.h"
#include "../third_party/virgl-protocol/virgl_protocol.h"
#include "../third_party/virgl-protocol/virgl_hw.h"
#include "glass_shaders.inc"

#define QUEUE_SIZE 64u
#define COMMAND_BYTES 65536u
typedef struct {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
} Descriptor;
typedef struct {
    uint16_t flags, index, ring[QUEUE_SIZE], used_event;
} Available;
typedef struct {
    uint32_t id, length;
} UsedElement;
typedef struct {
    uint16_t flags, index;
    UsedElement ring[QUEUE_SIZE];
    uint16_t avail_event;
} Used;
typedef struct {
    volatile uint8_t *address;
    uint32_t length;
} Region;
typedef struct {
    uint32_t type, flags;
    uint64_t fence;
    uint32_t context;
    uint8_t ring, pad[3];
} Header;
_Static_assert(sizeof(Header) == 24, "VirtIO-GPU header");
static Descriptor descriptors[QUEUE_SIZE] __attribute__((aligned(4096)));
static Available available __attribute__((aligned(4096)));
static volatile Used used __attribute__((aligned(4096)));
static uint8_t command[COMMAND_BYTES] __attribute__((aligned(4096)));
static uint8_t response[4096] __attribute__((aligned(4096)));
static Region common, notification, configuration;
static volatile uint16_t *notify;
static uint16_t seen;
static uint64_t fence_id;
static bool device, ready, attempted, failed;
static unsigned texture_w, texture_h;
static uint32_t *source, *result;
static size_t staging_bytes;
static uint32_t stream[COMMAND_BYTES / 4 - 16];
static unsigned stream_words;
static struct {
    uint64_t jobs, passes, uploads, readbacks, fences, millis, validate, stage, upload, submit,
        readback, copy;
} stats;
static inline void barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}
static inline void pause_cpu(void) {
    __asm__ volatile("pause");
}
static inline void out32(uint16_t p, uint32_t v) {
    __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint32_t in32(uint16_t p) {
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static uint32_t pci_read(uint32_t bdf, unsigned r) {
    out32(0xcf8, 0x80000000u | bdf | (r & ~3u));
    return in32(0xcfc);
}
static uint8_t pci_byte(uint32_t bdf, unsigned r) {
    return (uint8_t)(pci_read(bdf, r) >> ((r & 3) * 8));
}
static uint16_t read16(Region r, unsigned off) {
    return *(volatile uint16_t *)(r.address + off);
}
static uint32_t read32(Region r, unsigned off) {
    return *(volatile uint32_t *)(r.address + off);
}
static void write16(Region r, unsigned off, uint16_t v) {
    *(volatile uint16_t *)(r.address + off) = v;
}
static void write32(Region r, unsigned off, uint32_t v) {
    *(volatile uint32_t *)(r.address + off) = v;
}
static void address(unsigned off, const volatile void *p) {
    uint64_t v = (uintptr_t)p;
    write32(common, off, (uint32_t)v);
    write32(common, off + 4, (uint32_t)(v >> 32));
}
static void number(uint64_t n) {
    char b[24];
    uint_to_str(n, b);
    serial_write(b);
}

static bool region(uint32_t bdf, unsigned cap, Region *out, unsigned bytes) {
    unsigned bar = pci_byte(bdf, cap + 4);
    if (bar > 5)
        return false;
    for (unsigned i = 0; i < bar; i++) {
        uint32_t p = pci_read(bdf, 0x10 + i * 4);
        if (!(p & 1) && (p & 6) == 4) {
            if (i + 1 == bar)
                return false;
            i++;
        }
    }
    uint32_t low = pci_read(bdf, 0x10 + bar * 4), len = pci_read(bdf, cap + 12),
             off = pci_read(bdf, cap + 8);
    if ((low & 1) || (low & 6) == 2 || (low & 6) == 6 || ((low & 6) == 4 && bar == 5) ||
        len < bytes)
        return false;
    uint64_t base = low & ~15u;
    if ((low & 6) == 4)
        base |= (uint64_t)pci_read(bdf, 0x14 + bar * 4) << 32;
    if (!base || base > UINT64_MAX - off || base + off > UINT64_MAX - len ||
        !platform_map_mmio(base + off, bytes))
        return false;
    *out = (Region){(volatile uint8_t *)(uintptr_t)(base + off), len};
    return true;
}
bool virtio_gpu_init(unsigned width, unsigned height) {
    uint32_t bdf = 0;
    bool found = false;
    for (unsigned b = 0; b < 256 && !found; b++)
        for (unsigned d = 0; d < 32 && !found; d++) {
            uint32_t base = (b << 16) | (d << 11);
            if ((pci_read(base, 0) & 0xffff) == 0xffff)
                continue;
            unsigned functions = (pci_read(base, 12) & 0x800000) ? 8 : 1;
            for (unsigned f = 0; f < functions; f++)
                if (pci_read(base | (f << 8), 0) == 0x10501af4) {
                    bdf = base | (f << 8);
                    found = true;
                    break;
                }
        }
    if (!found)
        return false;
    if (width > 3840 || height > 2160 || !width || !height)
        return false;
    texture_w = (width + 112 + 63) & ~63u;
    texture_h = (height + 112 + 63) & ~63u;
    uint8_t visited[256] = {0};
    unsigned cap = pci_byte(bdf, 0x34) & ~3u, multiplier = 0;
    for (unsigned count = 0; cap && count < 48; count++) {
        if (cap < 0x40 || cap > 0xfc || visited[cap])
            return false;
        visited[cap] = 1;
        unsigned len = pci_byte(bdf, cap + 2), type = pci_byte(bdf, cap + 3);
        if (pci_byte(bdf, cap) == 9 && len >= 16 && cap + len <= 256) {
            if (type == 1 && !common.address && !region(bdf, cap, &common, 56))
                return false;
            if (type == 2 && !notification.address && len >= 20) {
                if (!region(bdf, cap, &notification, 2))
                    return false;
                multiplier = pci_read(bdf, cap + 16);
            }
            if (type == 4 && !configuration.address && !region(bdf, cap, &configuration, 16))
                return false;
        }
        cap = pci_byte(bdf, cap + 1) & ~3u;
    }
    if (!common.address || !notification.address || !configuration.address)
        return false;
    out32(0xcf8, 0x80000000u | bdf | 4);
    __asm__ volatile("outw %0,%1" ::"a"((uint16_t)(pci_read(bdf, 4) | 6)), "Nd"((uint16_t)0xcfc));
    common.address[20] = 0;
    barrier();
    common.address[20] = 3;
    write32(common, 0, 1);
    if (!(read32(common, 4) & 1))
        return false;
    write32(common, 0, 0);
    if (!(read32(common, 4) & 1)) {
        serial_write("[vgpu] VirtIO GPU has no VirGL feature\n");
        common.address[20] = 0;
        return false;
    }
    write32(common, 8, 0);
    write32(common, 12, 1);
    write32(common, 8, 1);
    write32(common, 12, 1);
    common.address[20] = 11;
    barrier();
    if (!(common.address[20] & 8))
        return false;
    write16(common, 22, 0);
    if (read16(common, 24) < QUEUE_SIZE || read16(common, 28))
        return false;
    uint64_t offset = (uint64_t)read16(common, 30) * multiplier;
    if (offset + 2 > notification.length || (offset & 1) ||
        !platform_map_mmio((uintptr_t)notification.address + offset, 2))
        return false;
    notify = (volatile uint16_t *)(notification.address + offset);
    available.flags = 1;
    write16(common, 24, QUEUE_SIZE);
    write16(common, 26, 0xffff);
    address(32, descriptors);
    address(40, &available);
    address(48, &used);
    barrier();
    write16(common, 28, 1);
    common.address[20] = 15;
    barrier();
    device = true;
    serial_write("[vgpu] native VirtIO-GPU / VirGL transport ready\n");
    return true;
}
static void fault(void) {
    failed = true;
    ready = false;
    device = false;
    common.address[20] = 0;
    barrier();
    /* A completed VirtIO reset quiesces DMA. Retain pages if reset is pending. */
    if (!common.address[20]) {
        if (source)
            process_kernel_free(source, staging_bytes);
        if (result)
            process_kernel_free(result, staging_bytes);
        source = result = 0;
    }
    serial_write("[vgpu] device failure; CPU compositor available\n");
}
static Header header(uint32_t type, bool fenced, uint32_t context) {
    return (Header){.type = type,
                    .flags = fenced ? 1u : 0u,
                    .fence = fenced ? ++fence_id : 0,
                    .context = context};
}
static bool send(unsigned bytes, unsigned reply_type, unsigned minimum) {
    if (!device || failed || bytes > sizeof command || minimum > sizeof response)
        return false;
    Header expected = *(Header *)command;
    memset(response, 0, sizeof response);
    descriptors[0] = (Descriptor){(uintptr_t)command, bytes, 1, 1};
    descriptors[1] = (Descriptor){(uintptr_t)response, sizeof response, 2, 0};
    available.ring[available.index % QUEUE_SIZE] = 0;
    barrier();
    available.index++;
    barrier();
    *notify = 0;
    uint64_t start = platform_millis(), kick = start + 1;
    unsigned remaining = 4;
    while (used.index == seen) {
        uint64_t now = platform_millis();
        if (now - start > 2000) {
            serial_write("[vgpu] fence timeout cmd=");
            number(expected.type);
            serial_write("\n");
            fault();
            return false;
        }
        /* A bounded follow-up notification lets a deferred fence be retired promptly.
         * Buffers and fence identity stay unchanged until the device completes it. */
        if ((expected.flags & 1) && remaining && now >= kick) {
            *notify = 0;
            remaining--;
            kick = now + 1;
        }
        pause_cpu();
    }
    barrier();
    UsedElement done = used.ring[seen % QUEUE_SIZE];
    seen++;
    Header *r = (Header *)response;
    if (done.id || done.length < minimum || done.length > sizeof response ||
        r->type != reply_type ||
        ((expected.flags & 1) && (!(r->flags & 1) || r->fence != expected.fence))) {
        serial_write("[vgpu] command error type=");
        number(expected.type);
        serial_write(" response=");
        number(r->type);
        serial_write(" bytes=");
        number(done.length);
        serial_write("\n");
        fault();
        return false;
    }
    if (expected.flags & 1)
        stats.fences++;
    return true;
}
static bool simple(uint32_t type, uint32_t resource) {
    struct {
        Header h;
        uint32_t id, pad;
    } q = {header(type, false, 1), resource, 0};
    memcpy(command, &q, sizeof q);
    return send(sizeof q, 0x1100, 24);
}
static bool create_resource(uint32_t id, uint32_t target, uint32_t format, uint32_t bind,
                            unsigned w, unsigned h) {
    struct {
        Header h;
        uint32_t id, target, format, bind, w, height, depth, array, last, samples, flags, pad;
    } q = {header(0x204, false, 0), id, target, format, bind, w, h, 1, 1, 0, 0, 0, 0};
    memcpy(command, &q, sizeof q);
    return send(sizeof q, 0x1100, 24) && simple(0x202, id);
}
static bool attach(uint32_t id, void *p) {
    struct {
        Header h;
        uint32_t id, count;
        uint64_t address;
        uint32_t bytes, pad;
    } q = {header(0x106, false, 0), id, 1, (uintptr_t)p, (uint32_t)staging_bytes, 0};
    memcpy(command, &q, sizeof q);
    return send(sizeof q, 0x1100, 24);
}
static bool transfer(uint32_t id, bool from, unsigned x, unsigned y, unsigned w, unsigned h) {
    struct {
        Header h;
        struct virgl_box box;
        uint64_t offset;
        uint32_t id, level, stride, layer;
    } q = {header(from ? 0x206 : 0x205, from, 1),
           {x, y, 0, w, h, 1},
           ((uint64_t)y * texture_w + x) * 4,
           id,
           0,
           texture_w * 4,
           texture_w * texture_h * 4};
    memcpy(command, &q, sizeof q);
    if (!send(sizeof q, 0x1100, 24))
        return false;
    if (from)
        stats.readbacks += (uint64_t)w * h * 4;
    else
        stats.uploads += (uint64_t)w * h * 4;
    return true;
}
static uint32_t *emit(unsigned cmd, unsigned obj, unsigned words) {
    if (stream_words + words + 1 > sizeof stream / 4)
        return 0;
    stream[stream_words++] = VIRGL_CMD0(cmd, obj, words);
    uint32_t *p = stream + stream_words;
    memset(p, 0, words * 4);
    stream_words += words;
    return p;
}
static void bind(unsigned obj, unsigned handle) {
    uint32_t *p = emit(VIRGL_CCMD_BIND_OBJECT, obj, 1);
    p[0] = handle;
}
static void shader(unsigned handle, unsigned stage, const char *text) {
    unsigned len = (unsigned)strlen(text) + 1, words = (len + 3) / 4;
    uint32_t *p = emit(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, 5 + words);
    p[0] = handle;
    p[1] = stage;
    p[2] = len;
    p[3] = 16384;
    p[4] = 0;
    memcpy(p + 5, text, len);
}
static void bind_shader(unsigned handle, unsigned stage) {
    uint32_t *p = emit(VIRGL_CCMD_BIND_SHADER, 0, 2);
    p[0] = handle;
    p[1] = stage;
}
/* Small integer -> IEEE binary32. Kernel never touches SSE/FPU state. */
static uint32_t bits(int n) {
    if (!n)
        return 0;
    uint32_t sign = n < 0 ? 0x80000000u : 0, value = (uint32_t)(n < 0 ? -n : n);
    unsigned exp = 31u - (unsigned)__builtin_clz(value);
    uint32_t mantissa = exp <= 23 ? value << (23 - exp) : value >> (exp - 23);
    return sign | ((exp + 127) << 23) | (mantissa & 0x7fffff);
}
static bool flush(bool fenced) {
    struct {
        Header h;
        uint32_t size, pad;
    } q = {header(0x207, fenced, 1), stream_words * 4, 0};
    memcpy(command, &q, sizeof q);
    memcpy(command + sizeof q, stream, stream_words * 4);
    stream_words = 0;
    return send(sizeof q + q.size, 0x1100, 24);
}
static void framebuffer(unsigned target, unsigned w, unsigned h) {
    uint32_t *p = emit(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    p[0] = 1;
    p[1] = 0;
    p[2] = target;
    p = emit(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
    p[0] = 0;
    p[1] = bits((int)w);
    p[2] = bits((int)h);
    p[3] = 0x3f000000;
    p[4] = p[1];
    p[5] = p[2];
    p[6] = 0x3f000000;
    /* Width/height are even for staging, but the active viewport may be odd. */
    p[1] -= 1u << 23;
    p[2] -= 1u << 23;
    p[4] = p[1];
    p[5] = p[2];
}
static void sample_views(unsigned first, unsigned second) {
    uint32_t *p = emit(VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 4);
    p[0] = 1;
    p[1] = 0;
    p[2] = first;
    p[3] = second;
}
static void draw(void) {
    uint32_t *p = emit(VIRGL_CCMD_DRAW_VBO, 0, 12);
    p[0] = 0;
    p[1] = 3;
    p[2] = 4;
    p[4] = 1;
    p[10] = 2;
}
static bool draw_glass(const ArkGlassRequest *, unsigned, unsigned, unsigned);
static bool pipeline(void) {
    struct {
        Header h;
        uint32_t index, pad;
    } info = {header(0x108, false, 0), 0, 0};
    memcpy(command, &info, sizeof info);
    if (!send(sizeof info, 0x1102, 40))
        return false;
    uint32_t capset = ((uint32_t *)(response + 24))[0];
    serial_write("[vgpu] advertised capset=");
    number(capset);
    serial_write("\n");
    if (capset != 1 && capset != 2)
        return false;
    struct {
        Header h;
        uint32_t id, version;
    } caps = {header(0x109, false, 0), capset, 1};
    memcpy(command, &caps, sizeof caps);
    if (!send(sizeof caps, 0x1103, 24 + sizeof(struct virgl_caps_v1)))
        return false;
    struct virgl_caps_v1 *c = (struct virgl_caps_v1 *)(response + 24);
    /* Legacy caps list exceptional vertex formats; baseline float attributes
     * do not have a set bit. Actual binding/drawing is verified separately. */
    if (!(c->sampler.bitmask[0] & 2) || !(c->render.bitmask[0] & 2) || c->glsl_level < 130)
        return false;
    serial_write("[vgpu] VirGL capset=");
    number(capset);
    serial_write(" GLSL=");
    number(c->glsl_level);
    serial_write("\n");
    struct {
        Header h;
        uint32_t name_len, init;
        char name[64];
    } ctx = {.h = header(0x200, false, 1), .name_len = 19};
    strcopy(ctx.name, "ArkOS glass shaders", sizeof ctx.name);
    memcpy(command, &ctx, sizeof ctx);
    if (!send(sizeof ctx, 0x1100, 24))
        return false;
    staging_bytes = (size_t)texture_w * texture_h * 4;
    source = process_kernel_alloc(staging_bytes);
    result = process_kernel_alloc(staging_bytes);
    if (!source || !result) {
        if (source)
            process_kernel_free(source, staging_bytes);
        if (result)
            process_kernel_free(result, staging_bytes);
        source = result = 0;
        return false;
    }
    for (unsigned id = 1; id <= 6; id++)
        if (id != 5 &&
            !create_resource(id, 2, 1, VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                             texture_w, texture_h))
            return false;
    if (!attach(1, source) || !attach(4, result) ||
        !create_resource(5, 0, VIRGL_FORMAT_R8_UNORM, VIRGL_BIND_VERTEX_BUFFER, 96, 1))
        return false;
    for (unsigned id = 1; id <= 6; id++) {
        if (id == 5)
            continue;
        uint32_t *p = emit(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
        p[0] = 20 + id;
        p[1] = id;
        p[2] = 1;
        p = emit(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_VIEW, 6);
        p[0] = 30 + id;
        p[1] = id;
        p[2] = 1;
        p[5] = 0 | (1 << 3) | (2 << 6) | (3 << 9);
    }
    uint32_t *p = emit(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11);
    p[0] = 10;
    p[3] = 15u << 27;
    bind(VIRGL_OBJECT_BLEND, 10);
    p = emit(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5);
    p[0] = 11;
    bind(VIRGL_OBJECT_DSA, 11);
    p = emit(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, 9);
    p[0] = 12;
    p[1] = (1u << 1) | (1u << 29) | (1u << 30);
    p[2] = 0x3f800000;
    p[5] = 0x3f800000;
    bind(VIRGL_OBJECT_RASTERIZER, 12);
    p = emit(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 9);
    p[0] = 13;
    p[1] = 0;
    p[4] = 31;
    p[5] = 16;
    p[8] = 31;
    bind(VIRGL_OBJECT_VERTEX_ELEMENTS, 13);
    p = emit(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0, 11 + 24);
    p[0] = 5;
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
    p[4] = 0;
    p[5] = 0;
    p[6] = 0;
    p[7] = 0;
    p[8] = 96;
    p[9] = 1;
    p[10] = 1;
    const uint32_t vertices[] = {
        0xbf800000, 0xbf800000, 0, 0x3f800000, 0,          0,          0, 0,
        0x40400000, 0xbf800000, 0, 0x3f800000, 0x40000000, 0,          0, 0,
        0xbf800000, 0x40400000, 0, 0x3f800000, 0,          0x40000000, 0, 0};
    memcpy(p + 11, vertices, sizeof vertices);
    p = emit(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3);
    p[0] = 32;
    p[1] = 0;
    p[2] = 5;
    shader(14, 0, glass_vertex_shader);
    shader(19, 1, glass_preparation_shader);
    shader(15, 1, glass_horizontal_shader);
    if (!flush(false))
        return false;
    shader(16, 1, glass_vertical_shader);
    shader(17, 1, glass_lens_shader);
    if (!flush(false))
        return false;
    bind_shader(14, 0);
    p = emit(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_STATE, 9);
    p[0] = 18;
    p[1] = 2 | (2 << 3) | (2 << 6) | (1 << 9) | (1 << 13);
    p = emit(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 4);
    p[0] = 1;
    p[1] = 0;
    p[2] = 18;
    p[3] = 18;
    framebuffer(24, 16, 16);
    p = emit(VIRGL_CCMD_CLEAR, 0, 8);
    p[0] = 4;
    p[1] = 0x3f800000;
    p[2] = 0x3f000000;
    p[3] = 0;
    p[4] = 0x3f800000;
    if (!flush(true) || !transfer(4, true, 0, 0, 16, 16))
        return false;
    for (unsigned y = 0; y < 16; y++)
        for (unsigned x = 0; x < 16; x++)
            if ((result[(size_t)y * texture_w + x] & 0xffffffu) != 0xff8000u) {
                serial_write("[vgpu] GPU clear/readback mismatch\n");
                return false;
            }
    ArkGlassRequest probe = {
        .rect = {0, 0, 16, 16}, .width = texture_w - 112, .height = texture_h - 112};
    for (unsigned y = 0; y < 80; y++)
        for (unsigned x = 0; x < 80; x++)
            source[(size_t)y * texture_w + x] = 0x183048;
    if (!transfer(1, false, 0, 0, 80, 80) || !draw_glass(&probe, 80, 80, 32) ||
        !transfer(4, true, 32, 32, 16, 16))
        return false;
    for (unsigned y = 32; y < 48; y++)
        for (unsigned x = 32; x < 48; x++) {
            uint32_t c = result[(size_t)y * texture_w + x];
            unsigned channels[3] = {(c >> 16) & 255, (c >> 8) & 255, c & 255},
                     wanted[3] = {14, 50, 86};
            for (unsigned k = 0; k < 3; k++)
                if (channels[k] + 2 < wanted[k] || channels[k] > wanted[k] + 2) {
                    serial_write("[vgpu] shader pixel mismatch got=");
                    number(c);
                    serial_write("\n");
                    return false;
                }
        }
    serial_write("[vgpu] GPU blur/lens draw, pixels and fence/readback verified\n");
    return true;
}
bool virtio_gpu_ready(void) {
    if (!device || failed)
        return false;
    if (attempted)
        return ready;
    attempted = true;
    ready = pipeline();
    if (!ready && !failed) {
        serial_write("[vgpu] shader pipeline unavailable\n");
        fault();
    }
    return ready;
}
static int clamp(int n, int low, int high) {
    return n < low ? low : n > high ? high : n;
}
static void constants(const ArkGlassRequest *q, unsigned bw, unsigned bh, unsigned pad) {
    uint32_t *p = emit(VIRGL_CCMD_SET_CONSTANT_BUFFER, 0, 2 + 8 * 4);
    p[0] = 1;
    p[1] = 0;
    p += 2;
    p[0] = bits((int)bw);
    p[1] = bits((int)bh);
    p[2] = bits((int)texture_w);
    p[3] = bits((int)texture_h);
    p[4] = bits(q->rect.w);
    p[5] = bits(q->rect.h);
    p[6] = bits((int)q->radius);
    p[7] = bits((int)pad);
    p[8] = bits((q->color >> 16) & 255);
    p[9] = bits((q->color >> 8) & 255);
    p[10] = bits(q->color & 255);
    p[11] = bits((int)q->tint);
    p[12] = bits((int)q->edge);
    p[13] = q->flags & ARK_GLASS_DISPERSION ? 0x3f800000 : 0;
    p[16] = bits(q->rect.x);
    p[17] = bits(q->rect.y);
    p[18] = bits((int)q->width);
    p[19] = bits((int)q->height);
    p[20] = bits(q->shadow.x);
    p[21] = bits(q->shadow.y);
    p[22] = bits(q->shadow.x + q->shadow.w);
    p[23] = bits(q->shadow.y + q->shadow.h);
    p[24] = q->flags & ARK_GLASS_SHADOW ? 0x3f800000 : 0;
}
static bool draw_glass(const ArkGlassRequest *q, unsigned bw, unsigned bh, unsigned pad) {
    constants(q, bw, bh, pad);
    framebuffer(26, bw, bh);
    bind_shader(19, 1);
    sample_views(31, 0);
    draw();
    framebuffer(22, bw, bh);
    bind_shader(15, 1);
    sample_views(36, 0);
    draw();
    framebuffer(23, bw, bh);
    bind_shader(16, 1);
    sample_views(32, 0);
    draw();
    framebuffer(24, bw, bh);
    bind_shader(17, 1);
    sample_views(33, 31);
    draw();
    return flush(false);
}
int64_t virtio_gpu_glass(ArkGlassRequest *q) {
    uint64_t at = platform_millis();
    if (q->op > ARK_GLASS_RENDER)
        return -22;
    if (q->op == ARK_GLASS_RENDER) {
        if (!q->width || !q->height || q->width > 3840 || q->height > 2160 ||
            q->stride < q->width || q->stride > 3840 || q->rect.w <= 0 || q->rect.h <= 0 ||
            q->rect.w > (int)q->width || q->rect.h > (int)q->height || q->rect.x < -(int)q->width ||
            q->rect.x > (int)q->width || q->rect.y < -(int)q->height ||
            q->rect.y > (int)q->height || q->radius > (unsigned)q->rect.w / 2 ||
            q->radius > (unsigned)q->rect.h / 2 || q->tint > 255 || q->edge > 255 ||
            q->flags & ~3u || q->reserved || q->shadow.x < 0 || q->shadow.y < 0 ||
            q->shadow.w < 0 || q->shadow.h < 0 || q->shadow.x > (int)q->width - q->shadow.w ||
            q->shadow.y > (int)q->height - q->shadow.h)
            return -22;
        size_t bytes = ((size_t)(q->height - 1) * q->stride + q->width) * 4;
        if (!process_user_range(q->pixels, bytes, true))
            return -14;
        stats.validate += platform_millis() - at;
    }
    if (!virtio_gpu_ready())
        return -19;
    if (q->op == ARK_GLASS_RENDER && q->rect.x < (int)q->width && q->rect.x + q->rect.w > 0 &&
        q->rect.y < (int)q->height && q->rect.y + q->rect.h > 0) {
        uint64_t start = platform_millis();
        unsigned pad = q->flags & ARK_GLASS_DISPERSION ? 56 : 32,
                 bw = (unsigned)q->rect.w + pad * 2, bh = (unsigned)q->rect.h + pad * 2;
        if (bw > texture_w || bh > texture_h)
            return -22;
        int left = clamp(q->rect.x - (int)pad, 0, (int)q->width - 1),
            right = clamp(q->rect.x + q->rect.w + (int)pad - 1, 0, (int)q->width - 1);
        for (unsigned y = 0; y < bh; y++) {
            int sy = clamp(q->rect.y + (int)y - (int)pad, 0, (int)q->height - 1),
                prefix = clamp((int)pad - q->rect.x, 0, (int)bw);
            uint32_t *row = source + (size_t)y * texture_w;
            if (!process_copy_from_user(row + prefix,
                                        q->pixels + ((uint64_t)sy * q->stride + left) * 4,
                                        (size_t)(right - left + 1) * 4))
                return -14;
            uint32_t first = row[prefix], last = row[prefix + right - left];
            for (int i = 0; i < prefix; i++)
                row[i] = first;
            for (unsigned i = (unsigned)prefix + (unsigned)(right - left + 1); i < bw; i++)
                row[i] = last;
        }
        at = platform_millis();
        stats.stage += at - start;
        if (!transfer(1, false, 0, 0, bw, bh))
            return -5;
        stats.upload += platform_millis() - at;
        at = platform_millis();
        if (!draw_glass(q, bw, bh, pad))
            return -5;
        stats.submit += platform_millis() - at;
        at = platform_millis();
        int x0 = clamp(q->rect.x, 0, (int)q->width), y0 = clamp(q->rect.y, 0, (int)q->height),
            x1 = clamp(q->rect.x + q->rect.w, 0, (int)q->width),
            y1 = clamp(q->rect.y + q->rect.h, 0, (int)q->height);
        if (x0 < x1 && y0 < y1) {
            unsigned ox = (unsigned)(x0 - q->rect.x) + pad, oy = (unsigned)(y0 - q->rect.y) + pad;
            if (!transfer(4, true, ox, oy, (unsigned)(x1 - x0), (unsigned)(y1 - y0)))
                return -5;
            stats.readback += platform_millis() - at;
            at = platform_millis();
            for (int y = y0; y < y1; y++)
                if (!process_copy_to_user(q->pixels + ((uint64_t)y * q->stride + x0) * 4,
                                          result + ((size_t)(y - y0) + oy) * texture_w + ox,
                                          (size_t)(x1 - x0) * 4))
                    return -14;
            stats.copy += platform_millis() - at;
        }
        stats.jobs++;
        stats.passes += 4;
        stats.millis += platform_millis() - start;
    }
    q->jobs = stats.jobs;
    q->passes = stats.passes;
    q->uploaded_bytes = stats.uploads;
    q->readback_bytes = stats.readbacks;
    q->fences = stats.fences;
    q->milliseconds = stats.millis;
    q->validate_ms = stats.validate;
    q->stage_ms = stats.stage;
    q->upload_ms = stats.upload;
    q->submit_ms = stats.submit;
    q->readback_ms = stats.readback;
    q->copy_ms = stats.copy;
    return 0;
}
