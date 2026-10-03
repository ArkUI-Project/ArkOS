/* Original ArkOS VMware SVGA II backend. No Linux/DRM code is used.
 * Protocol facts: QEMU hw/display/vmware_vga.c (v8.2.2).
 * This offloads 2D rectangle operations to a virtual device. It is not 3D,
 * shader acceleration, VirGL, or evidence of physical host-GPU execution.
 */
#include "gpu.h"
#include "process.h"
#include "cursor.h"
#include "virtio_gpu.h"
extern bool virtio_gpu_init(unsigned, unsigned) __attribute__((weak));
extern bool virtio_gpu_ready(void) __attribute__((weak));
static bool shader_ready(void) {
    return virtio_gpu_ready && virtio_gpu_ready();
}
#define GPU_MAX_W 3840u
#define GPU_MAX_H 2160u
#define TILE_W 32u
#define TILE_H 16u
#define MAX_TILES (((GPU_MAX_W + 31) / 32) * ((GPU_MAX_H + 15) / 16))
#define HASH_BUCKETS 8192u
#define CAP_FILL 1u
#define CAP_COPY 2u
#define CAP_CURSOR (1u << 5)
#define CAP_CURSOR_BYPASS (1u << 7)
enum {
    REG_ID = 0,
    REG_ENABLE = 1,
    REG_WIDTH = 2,
    REG_HEIGHT = 3,
    REG_MAX_W = 4,
    REG_MAX_H = 5,
    REG_BPP = 7,
    REG_RMASK = 9,
    REG_GMASK = 10,
    REG_BMASK = 11,
    REG_PITCH = 12,
    REG_FB_START = 13,
    REG_FB_OFFSET = 14,
    REG_VRAM_SIZE = 15,
    REG_CAPS = 17,
    REG_FIFO_START = 18,
    REG_FIFO_SIZE = 19,
    REG_CONFIG = 20,
    REG_SYNC = 21,
    REG_BUSY = 22,
    REG_CURSOR_ID = 24,
    REG_CURSOR_X = 25,
    REG_CURSOR_Y = 26,
    REG_CURSOR_ON = 27
};
enum { FIFO_MIN = 0, FIFO_MAX = 1, FIFO_NEXT = 2, FIFO_STOP = 3 };
static BootInfo display, firmware_display;
static GpuStats statistics;
static bool svga, cache_valid, failed, pending_mode;
static uint32_t detected_caps;
static uint16_t io_base;
static volatile uint32_t *fifo;
static uint32_t fifo_min, fifo_max, tiles_x, tiles_y;
static uint32_t *shadow;
static void prepare_shadow(void) {
    if (!shadow)
        shadow = process_kernel_alloc((size_t)display.width * display.height * 4);
    if (!shadow)
        statistics.accelerated = false;
}
static uint32_t old_hash[MAX_TILES], new_hash[MAX_TILES];
static bool unchanged[MAX_TILES];
static int hash_head[HASH_BUCKETS], hash_next[MAX_TILES];
static unsigned cursor_scale = 1, cursor_shape;
static inline void out32(uint16_t p, uint32_t v) {
    __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint32_t in32(uint16_t p) {
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void fence(void) {
    __asm__ volatile("mfence" ::: "memory");
}
static uint32_t pci_read(unsigned b, unsigned d, unsigned f, unsigned r) {
    out32(0xcf8, 0x80000000u | (b << 16) | (d << 11) | (f << 8) | (r & 0xfc));
    return in32(0xcfc);
}
static void pci_write(unsigned b, unsigned d, unsigned f, unsigned r, uint32_t v) {
    out32(0xcf8, 0x80000000u | (b << 16) | (d << 11) | (f << 8) | (r & 0xfc));
    out32(0xcfc, v);
}
static uint32_t reg_read(unsigned r) {
    out32(io_base, r);
    return in32((uint16_t)(io_base + 1));
}
static void reg_write(unsigned r, uint32_t v) {
    out32(io_base, r);
    out32((uint16_t)(io_base + 1), v);
}
static void log_number(uint64_t n) {
    char b[24];
    uint_to_str(n, b);
    serial_write(b);
}
static void software_fallback(void) {
    reg_write(REG_CURSOR_ON, 0);
    reg_write(REG_CONFIG, 0);
    reg_write(REG_ENABLE, 0);
    display = firmware_display;
    svga = false;
    fifo = 0;
    cache_valid = false;
    failed = true;
    pending_mode = false;
    statistics.accelerated = false;
    statistics.hardware_cursor = false;
}
static bool wait_fifo(void) {
    if (failed || !fifo)
        return false;
    fence();
    reg_write(REG_SYNC, 1);
    for (unsigned i = 0; i < 2000000; i++)
        if (!reg_read(REG_BUSY)) {
            if (fifo[FIFO_STOP] == fifo[FIFO_NEXT])
                return true;
            reg_write(REG_SYNC, 1); /* QEMU bounds each FIFO service pass. */
        }
    software_fallback();
    serial_write("[gpu] FIFO timeout; restored software framebuffer\n");
    return false;
}
static uint32_t fifo_free(void) {
    uint32_t n = fifo[FIFO_NEXT], s = fifo[FIFO_STOP];
    return n >= s ? fifo_max - fifo_min - (n - s) - 4 : s - n - 4;
}
static bool submit(const uint32_t *words, unsigned count) {
    if (failed || !fifo || count * 4 > fifo_max - fifo_min - 4)
        return false;
    if (fifo_free() < count * 4 && !wait_fifo())
        return false;
    uint32_t next = fifo[FIFO_NEXT];
    for (unsigned i = 0; i < count; i++) {
        fifo[next / 4] = words[i];
        next += 4;
        if (next == fifo_max)
            next = fifo_min;
    }
    fence();
    fifo[FIFO_NEXT] = next;
    return true;
}
static bool rect_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint32_t c) {
    uint32_t cmd[] = {2, c, x, y, w, h};
    if (!submit(cmd, 6))
        return false;
    statistics.fill_commands++;
    statistics.filled_pixels += (uint64_t)w * h;
    return true;
}
static bool rect_copy(unsigned sx, unsigned sy, unsigned dx, unsigned dy, unsigned w, unsigned h) {
    uint32_t cmd[] = {3, sx, sy, dx, dy, w, h};
    if (!submit(cmd, 7))
        return false;
    statistics.copy_commands++;
    statistics.copied_pixels += (uint64_t)w * h;
    return true;
}
static void update(unsigned x, unsigned y, unsigned w, unsigned h) {
    uint32_t cmd[] = {1, x, y, w, h};
    if (submit(cmd, 5))
        statistics.update_commands++;
}
static uint32_t pixel_format(uint32_t c) {
    return (((c >> 16) & 255) << display.red_pos) | (((c >> 8) & 255) << display.green_pos) |
           ((c & 255) << display.blue_pos);
}
static void upload(const uint32_t *canvas, unsigned stride, unsigned x, unsigned y, unsigned w,
                   unsigned h) {
    if (display.red_pos == 16 && display.green_pos == 8 && display.blue_pos == 0) {
        /* VRAM is a linear memory aperture. REP MOVSQ writes exactly the supplied
         * bytes and avoids a per-pixel loop without requiring SSE/FPU kernel state. */
        if (x == 0 && w == stride && display.pitch == w * 4) {
            memcpy((void *)(uintptr_t)(display.framebuffer + (uint64_t)y * display.pitch),
                   canvas + (size_t)y * stride, (size_t)w * h * 4);
        } else
            for (unsigned yy = y; yy < y + h; yy++) {
                memcpy(
                    (void *)(uintptr_t)(display.framebuffer + (uint64_t)yy * display.pitch + x * 4),
                    canvas + (size_t)yy * stride + x, (size_t)w * 4);
            }
        statistics.uploaded_pixels += (uint64_t)w * h;
        return;
    }
    for (unsigned yy = y; yy < y + h; yy++) {
        volatile uint32_t *dst =
            (volatile uint32_t *)(uintptr_t)(display.framebuffer + (uint64_t)yy * display.pitch);
        const uint32_t *src = canvas + (size_t)yy * stride;
        for (unsigned xx = x; xx < x + w; xx++)
            dst[xx] = pixel_format(src[xx]);
    }
    statistics.uploaded_pixels += (uint64_t)w * h;
}
static bool find_adapter(unsigned *bus, unsigned *dev, unsigned *fun) {
    for (unsigned b = 0; b < 256; b++)
        for (unsigned d = 0; d < 32; d++) {
            if ((pci_read(b, d, 0, 0) & 0xffff) == 0xffff)
                continue;
            unsigned functions = (pci_read(b, d, 0, 0x0c) & 0x00800000) ? 8 : 1;
            for (unsigned f = 0; f < functions; f++)
                if (pci_read(b, d, f, 0) == 0x040515ad) {
                    *bus = b;
                    *dev = d;
                    *fun = f;
                    return true;
                }
        }
    return false;
}
static void define_cursor(void) {
    if (!statistics.hardware_cursor)
        return;
    /* DWORD-aligned widths avoid the QEMU legacy mask-stride ambiguity. */
    unsigned size = 32 * cursor_scale, mask_words = size * size / 32,
             total = 8 + mask_words + size * size;
    static uint32_t command[8 + 128 + 4096];
    command[0] = 19;
    command[1] = 0;
    command[2] = (unsigned)ark_cursor_hot_x(cursor_shape) * cursor_scale;
    command[3] = (unsigned)ark_cursor_hot_y(cursor_shape) * cursor_scale;
    command[4] = size;
    command[5] = size;
    command[6] = 1;
    command[7] = 32;
    for (unsigned i = 0; i < mask_words; i++)
        command[8 + i] = 0xffffffffu;
    for (unsigned y = 0; y < size; y++)
        for (unsigned x = 0; x < size; x++) {
            bool opaque = ark_cursor_inside(cursor_shape, (int)((x * 8 + 4) / cursor_scale),
                                            (int)((y * 8 + 4) / cursor_scale), 0);
            uint32_t color = ark_cursor_inside(cursor_shape, (int)((x * 8 + 4) / cursor_scale),
                                               (int)((y * 8 + 4) / cursor_scale), 1)
                                 ? 0xf5f9ff
                                 : 0x102033;
            if (opaque) {
                unsigned byte = y * (size / 8) + x / 8;
                unsigned shift = (byte % 4) * 8 + 7 - (x % 8);
                command[8 + byte / 4] &= ~(1u << shift);
            }
            command[8 + mask_words + y * size + x] = color;
        }
    if (!submit(command, total) || !wait_fifo())
        statistics.hardware_cursor = false;
    else
        reg_write(REG_CURSOR_ID, 0);
}
void gpu_cursor_set_scale(unsigned scale) {
    scale = scale > 1 ? 2 : 1;
    if (scale == cursor_scale)
        return;
    cursor_scale = scale;
    define_cursor();
}
void gpu_cursor_set_shape(unsigned shape) {
    if (shape >= ARK_CURSOR_COUNT || shape == cursor_shape)
        return;
    cursor_shape = shape;
    define_cursor();
}
bool gpu_cursor_move(int x, int y, bool visible) {
    if (!statistics.hardware_cursor || failed)
        return false;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    reg_write(REG_CURSOR_X, (uint32_t)x);
    reg_write(REG_CURSOR_Y, (uint32_t)y);
    reg_write(REG_CURSOR_ON, visible ? 1 : 0);
    statistics.cursor_updates++;
    return true;
}
const char *gpu_backend_name(void) {
    return shader_ready()           ? "VirtIO-GPU / VirGL shaders"
           : statistics.accelerated ? "VMware SVGA II / FIFO 2D"
           : pending_mode           ? "SVGA II / waiting for display refresh"
                                    : "Software framebuffer";
}
const GpuStats *gpu_stats(void) {
    statistics.shader_accelerated = shader_ready();
    return &statistics;
}
/* Only automatic display refresh consumes the startup UPDATE: unlike SYNC,
 * that path first commits QEMU's actual display-surface stride and bounds.
 * A headless frontend may not refresh until its first QMP screendump. Keep
 * uploading pixels while pending, then enable commands on a later present.
 */
static bool finish_mode(void) {
    if (!pending_mode || fifo[FIFO_STOP] != fifo[FIFO_NEXT])
        return false;
    pending_mode = false;
    unsigned sx = display.width - 16, dx = display.width - 8, y = display.height - 4;
    for (unsigned yy = y; yy < display.height; yy++)
        for (unsigned xx = sx; xx < display.width; xx++)
            *(volatile uint32_t *)(uintptr_t)(display.framebuffer + (uint64_t)yy * display.pitch +
                                              xx * 4) = 0xabcdef;
    rect_fill(sx, y, 8, 4, 0x123456);
    rect_copy(sx, y, dx, y, 8, 4);
    bool valid = wait_fifo();
    if (valid)
        for (unsigned yy = y; yy < display.height; yy++)
            for (unsigned xx = sx; xx < display.width; xx++)
                if (*(volatile uint32_t *)(uintptr_t)(display.framebuffer +
                                                      (uint64_t)yy * display.pitch + xx * 4) !=
                    0x123456)
                    valid = false;
    if (!valid) {
        if (!failed)
            software_fallback();
        serial_write("[gpu] Full-mode FIFO readback failed; restored software framebuffer\n");
        return false;
    }
    statistics.capabilities = detected_caps;
    statistics.accelerated = true;
    statistics.hardware_cursor =
        (detected_caps & (CAP_CURSOR | CAP_CURSOR_BYPASS)) == (CAP_CURSOR | CAP_CURSOR_BYPASS);
    statistics.frames = statistics.fill_commands = statistics.copy_commands =
        statistics.update_commands = 0;
    statistics.filled_pixels = statistics.copied_pixels = statistics.uploaded_pixels =
        statistics.cursor_updates = 0;
    tiles_x = (display.width + TILE_W - 1) / TILE_W;
    tiles_y = (display.height + TILE_H - 1) / TILE_H;
    cache_valid = false;
    define_cursor();
    gpu_cursor_move(0, 0, false);
    serial_write("[gpu] SVGA II FIFO fill/copy verified at full-mode bounds; caps=");
    log_number(detected_caps);
    serial_write(statistics.hardware_cursor ? "; hardware cursor (binary mask)\n"
                                            : "; software cursor (device capability absent)\n");
    return statistics.accelerated;
}
bool gpu_init(BootInfo *boot) {
    if (virtio_gpu_init)
        virtio_gpu_init(boot->width, boot->height);
    display = firmware_display = *boot;
    memset(&statistics, 0, sizeof statistics);
    cache_valid = false;
    svga = false;
    failed = false;
    pending_mode = false;
    if (boot->width > GPU_MAX_W || boot->height > GPU_MAX_H) {
        serial_write("[gpu] Software framebuffer (geometry)\n");
        return false;
    }
    unsigned b, d, f;
    if (!find_adapter(&b, &d, &f)) {
        serial_write("[gpu] Software framebuffer (no SVGA II adapter)\n");
        return false;
    }
    uint32_t bar0 = pci_read(b, d, f, 0x10), bar1 = pci_read(b, d, f, 0x14),
             bar2 = pci_read(b, d, f, 0x18);
    if (!(bar0 & 1) || (bar0 & ~3u) > 0xfff0 || !bar1 || !bar2 || (bar1 & 7) || (bar2 & 7)) {
        serial_write("[gpu] Unsupported SVGA II PCI BARs; software framebuffer\n");
        return false;
    }
    io_base = (uint16_t)(bar0 & ~3u);
    pci_write(b, d, f, 4, (pci_read(b, d, f, 4) & 0xffff) | 3);
    reg_write(REG_ID, 0x90000002);
    if (reg_read(REG_ID) != 0x90000002)
        return false;
    uint32_t caps = reg_read(REG_CAPS), fifo_size = reg_read(REG_FIFO_SIZE),
             vram_size = reg_read(REG_VRAM_SIZE);
    if ((caps & (CAP_FILL | CAP_COPY)) != (CAP_FILL | CAP_COPY) || fifo_size < 16 + 10 * 1024 ||
        fifo_size > 16 * 1024 * 1024 || boot->width > reg_read(REG_MAX_W) ||
        boot->height > reg_read(REG_MAX_H) || (uint64_t)boot->width * boot->height * 4 > vram_size)
        return false;
    uint32_t fb_address = bar1 & ~15u, fifo_address = bar2 & ~15u;
    if (!fb_address || !fifo_address || (uint64_t)fb_address + vram_size > 0x100000000ull ||
        (uint64_t)fifo_address + fifo_size > 0x100000000ull)
        return false;
    /* These legacy QEMU protocol modes have exactly an XRGB8888 front buffer. */
    reg_write(REG_CONFIG, 0);
    reg_write(REG_ENABLE, 0);
    reg_write(REG_WIDTH, boot->width);
    reg_write(REG_HEIGHT, boot->height);
    reg_write(REG_BPP, 32);
    fifo = (volatile uint32_t *)(uintptr_t)fifo_address;
    fifo_min = 16;
    fifo_max = fifo_size & ~3u;
    fifo[FIFO_MIN] = fifo_min;
    fifo[FIFO_MAX] = fifo_max;
    fifo[FIFO_NEXT] = fifo_min;
    fifo[FIFO_STOP] = fifo_min;
    fence();
    reg_write(REG_ENABLE, 1);
    reg_write(REG_CONFIG, 1);
    uint32_t pitch = reg_read(REG_PITCH), offset = reg_read(REG_FB_OFFSET);
    if (reg_read(REG_BPP) != 32 || reg_read(REG_RMASK) != 0xff0000 ||
        reg_read(REG_GMASK) != 0xff00 || reg_read(REG_BMASK) != 0xff || pitch < boot->width * 4 ||
        (uint64_t)pitch * boot->height + offset > vram_size) {
        reg_write(REG_CONFIG, 0);
        reg_write(REG_ENABLE, 0);
        fifo = 0;
        serial_write("[gpu] Unsupported native mode; retained boot framebuffer\n");
        return false;
    }
    display.framebuffer = (uint64_t)fb_address + offset;
    display.pitch = pitch;
    display.bpp = 32;
    display.red_pos = 16;
    display.green_pos = 8;
    display.blue_pos = 0;
    display.red_size = display.green_size = display.blue_size = 8;
    *boot = display;
    svga = true;
    /* Do not use SYNC to force this startup command: QEMU has not necessarily
     * committed the requested mode yet. A later real refresh will consume it.
     */
    detected_caps = caps;
    pending_mode = true;
    update(0, 0, display.width, display.height);
    uint64_t deadline = platform_ticks() + 10;
    while (fifo[FIFO_STOP] != fifo[FIFO_NEXT] && platform_ticks() < deadline)
        platform_idle();
    if (fifo[FIFO_STOP] == fifo[FIFO_NEXT]) {
        bool ready = finish_mode();
        *boot = display;
        return ready;
    }
    serial_write("[gpu] SVGA II mode prepared; waiting for display refresh before 2D activation\n");
    return false;
}

static void tile_rect(unsigned index, unsigned *x, unsigned *y, unsigned *w, unsigned *h) {
    *x = (index % tiles_x) * TILE_W;
    *y = (index / tiles_x) * TILE_H;
    *w = display.width - *x;
    if (*w > TILE_W)
        *w = TILE_W;
    *h = display.height - *y;
    if (*h > TILE_H)
        *h = TILE_H;
}
static uint32_t tile_hash(const uint32_t *c, unsigned stride, unsigned x, unsigned y, unsigned w,
                          unsigned h) {
    uint32_t hash = 2166136261u;
    for (unsigned yy = y; yy < y + h; yy++)
        for (unsigned xx = x; xx < x + w; xx++) {
            hash ^= c[(size_t)yy * stride + xx];
            hash *= 16777619u;
        }
    return hash;
}
static bool tile_equal(const uint32_t *a, unsigned stride, unsigned ax, unsigned ay,
                       const uint32_t *b, unsigned bs, unsigned bx, unsigned by, unsigned w,
                       unsigned h) {
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
            if (a[(size_t)(ay + y) * stride + ax + x] != b[(size_t)(by + y) * bs + bx + x])
                return false;
    return true;
}
static bool tile_solid(const uint32_t *c, unsigned stride, unsigned x, unsigned y, unsigned w,
                       unsigned h) {
    uint32_t v = c[(size_t)y * stride + x];
    for (unsigned yy = y; yy < y + h; yy++)
        for (unsigned xx = x; xx < x + w; xx++)
            if (c[(size_t)yy * stride + xx] != v)
                return false;
    return true;
}
static void shadow_copy(const uint32_t *c, unsigned stride, unsigned x, unsigned y, unsigned w,
                        unsigned h) {
    for (unsigned yy = y; yy < y + h; yy++)
        memcpy(shadow + (size_t)yy * display.width + x, c + (size_t)yy * stride + x, w * 4);
}
void gpu_present_animation(const uint32_t *canvas, unsigned stride) {
    gpu_present_animation_damage(canvas, stride, 0);
}
void gpu_present_animation_damage(const uint32_t *canvas, unsigned stride, const GpuRect *damage) {
    if (!canvas || stride < display.width)
        return;
    prepare_shadow();
    int x = damage ? damage->x : 0, y = damage ? damage->y : 0;
    int64_t right = damage ? (int64_t)x + damage->w : display.width,
            bottom = damage ? (int64_t)y + damage->h : display.height;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (right > display.width)
        right = display.width;
    if (bottom > display.height)
        bottom = display.height;
    if (right <= x || bottom <= y)
        return;
    statistics.frames++;
    /* Streaming damage bypasses retained-tile copies and hashes. Invalidate the
     * shadow so later frames cannot compare against stale animation/cursor data. */
    cache_valid = false;
    if (pending_mode && fifo[FIFO_STOP] == fifo[FIFO_NEXT])
        finish_mode();
    /* Complete earlier fills/copies before their destination pixels are replaced
     * by this CPU stream. Do not SYNC an uncommitted startup display mode. */
    if (svga && !failed && !pending_mode)
        wait_fifo();
    upload(canvas, stride, (unsigned)x, (unsigned)y, (unsigned)(right - x), (unsigned)(bottom - y));
    if (svga && !failed && !pending_mode) {
        update((unsigned)x, (unsigned)y, (unsigned)(right - x), (unsigned)(bottom - y));
        if (!wait_fifo())
            upload(canvas, stride, 0, 0, display.width, display.height);
    }
}
void gpu_present(const uint32_t *canvas, unsigned stride, const GpuRect *damage) {
    if (!canvas || stride < display.width)
        return;
    prepare_shadow();
    statistics.frames++;
    if (pending_mode && fifo[FIFO_STOP] == fifo[FIFO_NEXT]) {
        finish_mode();
        /* The full-mode readback probe changed a corner: restore the whole canvas. */
        damage = 0;
    }
    if (damage) {
        int x = damage->x, y = damage->y;
        int64_t right = (int64_t)x + damage->w, bottom = (int64_t)y + damage->h;
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if (right > (int64_t)display.width)
            right = display.width;
        if (bottom > (int64_t)display.height)
            bottom = display.height;
        if (right <= x || bottom <= y)
            return;
        if (svga && !failed && !pending_mode && !wait_fifo()) {
            upload(canvas, stride, 0, 0, display.width, display.height);
            return;
        }
        upload(canvas, stride, (unsigned)x, (unsigned)y, (unsigned)(right - x),
               (unsigned)(bottom - y));
        if (svga && !failed && !pending_mode) {
            update((unsigned)x, (unsigned)y, (unsigned)(right - x), (unsigned)(bottom - y));
            if (!wait_fifo())
                upload(canvas, stride, 0, 0, display.width, display.height);
        }
        /* Track the actual device contents, including temporary software cursors.
         * Caller may immediately restore its canvas in RAM; the retained shadow
         * must keep the uploaded cursor until a later damage/full-frame restore.
         */
        if (cache_valid) {
            shadow_copy(canvas, stride, (unsigned)x, (unsigned)y, (unsigned)(right - x),
                        (unsigned)(bottom - y));
            for (unsigned ty = (unsigned)y / TILE_H; ty <= (unsigned)(bottom - 1) / TILE_H; ty++)
                for (unsigned tx = (unsigned)x / TILE_W; tx <= (unsigned)(right - 1) / TILE_W;
                     tx++) {
                    unsigned index = ty * tiles_x + tx, xx, yy, w, h;
                    tile_rect(index, &xx, &yy, &w, &h);
                    old_hash[index] = tile_hash(shadow, display.width, xx, yy, w, h);
                }
        }
        return;
    }
    if (!statistics.accelerated || failed) {
        upload(canvas, stride, 0, 0, display.width, display.height);
        if (svga && !failed && !pending_mode) {
            update(0, 0, display.width, display.height);
            wait_fifo();
        }
        return;
    }
    unsigned count = tiles_x * tiles_y;
    for (unsigned i = 0; i < HASH_BUCKETS; i++)
        hash_head[i] = -1;
    for (unsigned i = 0; i < count; i++) {
        unsigned x, y, w, h;
        tile_rect(i, &x, &y, &w, &h);
        new_hash[i] = tile_hash(canvas, stride, x, y, w, h);
        unchanged[i] = cache_valid && new_hash[i] == old_hash[i] &&
                       tile_equal(canvas, stride, x, y, shadow, display.width, x, y, w, h);
        hash_next[i] = -1;
        if (unchanged[i] && w == TILE_W && h == TILE_H) {
            unsigned bucket = new_hash[i] & (HASH_BUCKETS - 1);
            hash_next[i] = hash_head[bucket];
            hash_head[bucket] = (int)i;
        }
    }
    bool touched = false;
    for (unsigned i = 0; i < count; i++) {
        if (unchanged[i])
            continue;
        unsigned x, y, w, h;
        tile_rect(i, &x, &y, &w, &h);
        bool done = false;
        touched = true;
        if (tile_solid(canvas, stride, x, y, w, h))
            done = rect_fill(x, y, w, h, canvas[(size_t)y * stride + x]);
        else if (w == TILE_W && h == TILE_H) {
            unsigned probes = 0;
            for (int source = hash_head[new_hash[i] & (HASH_BUCKETS - 1)];
                 source >= 0 && probes++ < 32; source = hash_next[source]) {
                if (new_hash[source] != new_hash[i])
                    continue;
                unsigned sx, sy, ww, hh;
                tile_rect((unsigned)source, &sx, &sy, &ww, &hh);
                /* Only unchanged source tiles are eligible: queued fills/uploads cannot
                 * overwrite a source before the device executes this copy.
                 */
                if (tile_equal(canvas, stride, x, y, canvas, stride, sx, sy, w, h)) {
                    done = rect_copy(sx, sy, x, y, w, h);
                    break;
                }
            }
        }
        if (!done)
            upload(canvas, stride, x, y, w, h);
    }
    if (touched) {
        update(0, 0, display.width, display.height);
        wait_fifo();
    }
    if (failed)
        upload(canvas, stride, 0, 0, display.width, display.height);
    shadow_copy(canvas, stride, 0, 0, display.width, display.height);
    memcpy(old_hash, new_hash, count * sizeof(uint32_t));
    cache_valid = !failed;
}

bool gpu_panic_display(BootInfo *out) {
    if (fifo) {
        (void)wait_fifo();
        reg_write(REG_CURSOR_ON, 0);
    }
    if (!display.framebuffer)
        return false;
    *out = display;
    return true;
}
void gpu_panic_publish(void) {
    if (fifo) {
        update(0, 0, display.width, display.height);
        (void)wait_fifo();
    }
}
