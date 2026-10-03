/* Actual Ring3 -> VirtIO/VirGL -> GPU -> DMA readback validation. */
#include "ark_api.h"
#include "liquid_glass.h"
#include "raster.h"
#ifndef GLASS_CONTROLLER
#define GLASS_CONTROLLER 0
#endif
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern void uint_to_str(uint64_t, char *);
extern size_t strlen(const char *);
static void log_text(const char *s) {
    ark_syscall6(ARK_SYS_LOG, (uintptr_t)s, strlen(s), 0, 0, 0, 0);
}
static void check(bool ok, unsigned id) {
    if (!ok) {
        char n[24];
        uint_to_str(id, n);
        log_text("[glass-probe] FAIL ");
        log_text(n);
        log_text("\n");
        ark_exit(1);
    }
}
#if GLASS_CONTROLLER
#define W 192
#define H 128
#define BW (W + 112)
#define BH (H + 112)
static uint32_t pixels[W * H], before[W * H], expected[W * H], a[BW * BH], b[BW * BH];
static int clamp(int n, int lo, int hi) {
    return n < lo ? lo : n > hi ? hi : n;
}
static uint32_t blend(uint32_t x, uint32_t y, unsigned alpha) {
    unsigned inv = 255 - alpha;
    return (((((x >> 16) & 255) * inv + ((y >> 16) & 255) * alpha) / 255) << 16) |
           (((((x >> 8) & 255) * inv + ((y >> 8) & 255) * alpha) / 255) << 8) |
           (((x & 255) * inv + (y & 255) * alpha) / 255);
}
static uint32_t sample(float x, float y, int w, int h) {
    x = x < 0 ? 0 : x > w - 1 ? w - 1 : x;
    y = y < 0 ? 0 : y > h - 1 ? h - 1 : y;
    int ix = (int)x, iy = (int)y, nx = ix + 1 < w ? ix + 1 : ix, ny = iy + 1 < h ? iy + 1 : iy;
    float fx = x - ix, fy = y - iy;
    uint32_t c[4] = {a[iy * w + ix], a[iy * w + nx], a[ny * w + ix], a[ny * w + nx]}, out = 0;
    for (unsigned ch = 0; ch < 3; ch++) {
        unsigned shift = ch * 8;
        float top = ((c[0] >> shift) & 255) * (1 - fx) + ((c[1] >> shift) & 255) * fx,
              bottom = ((c[2] >> shift) & 255) * (1 - fx) + ((c[3] >> shift) & 255) * fx;
        out |= (uint32_t)(top * (1 - fy) + bottom * fy) << shift;
    }
    return out;
}
static void reference(const ArkGlassRequest *q) {
    int pad = q->flags & ARK_GLASS_DISPERSION ? 56 : 32, w = q->rect.w + pad * 2,
        h = q->rect.h + pad * 2;
    static const unsigned weights[] = {1, 2, 7, 17, 31, 45, 50, 45, 31, 17, 7, 2, 1};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sx = clamp(q->rect.x + x - pad, 0, W - 1),
                sy = clamp(q->rect.y + y - pad, 0, H - 1);
            uint32_t c = before[sy * W + sx];
            if ((q->flags & ARK_GLASS_SHADOW) && sx >= q->shadow.x &&
                sx < q->shadow.x + q->shadow.w && sy >= q->shadow.y &&
                sy < q->shadow.y + q->shadow.h)
                for (unsigned n = 0; n < 8; n++)
                    c = blend(c, 0x071c3c, 7);
            a[y * w + x] = liquid_glass_vibrancy(c);
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned r = 0, g = 0, blue = 0;
            for (int k = 0; k < 13; k++) {
                uint32_t c = a[y * w + clamp(x + k - 6, 0, w - 1)];
                r += ((c >> 16) & 255) * weights[k];
                g += ((c >> 8) & 255) * weights[k];
                blue += (c & 255) * weights[k];
            }
            b[y * w + x] = ((r / 256) << 16) | ((g / 256) << 8) | blue / 256;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned r = 0, g = 0, blue = 0;
            for (int k = 0; k < 13; k++) {
                uint32_t c = b[clamp(y + k - 6, 0, h - 1) * w + x];
                r += ((c >> 16) & 255) * weights[k];
                g += ((c >> 8) & 255) * weights[k];
                blue += (c & 255) * weights[k];
            }
            a[y * w + x] =
                blend(((r / 256) << 16) | ((g / 256) << 8) | blue / 256, q->color, q->tint);
        }
    memcpy(expected, before, sizeof expected);
    for (int y = clamp(q->rect.y, 0, H); y < clamp(q->rect.y + q->rect.h, 0, H); y++)
        for (int x = clamp(q->rect.x, 0, W); x < clamp(q->rect.x + q->rect.w, 0, W); x++) {
            int ax = x - q->rect.x, ay = y - q->rect.y;
            unsigned cover = raster_round_coverage(ax, ay, q->rect.w, q->rect.h, (int)q->radius);
            if (!cover)
                continue;
            LiquidLens l;
            liquid_glass_lens(ax + .5f, ay + .5f, q->rect.w, q->rect.h, q->radius, 12, 24, false,
                              (q->flags & ARK_GLASS_DISPERSION) != 0, &l);
            uint32_t c[7];
            for (int i = 0; i < 7; i++)
                c[i] = sample(ax + pad + l.dx + l.dispersion_x * (3 - i) / 3,
                              ay + pad + l.dy + l.dispersion_y * (3 - i) / 3, w, h);
            unsigned r = ((((c[0] >> 16) & 255) + ((c[1] >> 16) & 255) + ((c[2] >> 16) & 255)) * 2 +
                          ((c[6] >> 16) & 255)) /
                         7;
            unsigned g = (((c[1] >> 8) & 255) +
                          (((c[2] >> 8) & 255) + ((c[3] >> 8) & 255) + ((c[4] >> 8) & 255)) * 2) /
                         7,
                     blue = ((c[4] & 255) + (c[5] & 255) + (c[6] & 255)) / 3;
            unsigned inner = raster_round_coverage(ax - 1, ay - 1, q->rect.w - 2, q->rect.h - 2,
                                                   (int)q->radius - 1),
                     light = (unsigned)((cover > inner ? cover - inner : 0) * l.highlight * .5f) *
                             q->edge / 255;
            r = (unsigned)clamp((int)(r + light), 0, 255);
            g = (unsigned)clamp((int)(g + light), 0, 255);
            blue = (unsigned)clamp((int)(blue + light), 0, 255);
            expected[y * W + x] = blend(before[y * W + x], (r << 16) | (g << 8) | blue, cover);
        }
}
int main(void) {
    ArkGlassRequest query = {.op = ARK_GLASS_QUERY};
    check(ark_call(ARK_SYS_COMPOSITOR, &query, sizeof query) == 0, 1);
    for (unsigned scene = 0; scene < 7; scene++) {
        for (unsigned y = 0; y < H; y++)
            for (unsigned x = 0; x < W; x++) {
                unsigned r = (x * 2 + y / 3) & 255, g = (y * 2 + x / 4) & 255,
                         blue = (((x / 13 + y / 11) & 1) ? 180 : 30);
                before[y * W + x] = (r << 16) | (g << 8) | blue;
            }
        memcpy(pixels, before, sizeof pixels);
        ArkGlassRequest q = {.op = ARK_GLASS_RENDER,
                             .pixels = (uintptr_t)pixels,
                             .width = W,
                             .height = H,
                             .stride = W,
                             .radius = 16,
                             .rect = {24, 16, 128, 96},
                             .color = 0xf3f9ff,
                             .tint = 170,
                             .edge = 80};
        if (scene == 1) {
            q.flags = ARK_GLASS_DISPERSION;
            q.rect = (ArkRect){16, 32, 160, 72};
            q.radius = 30;
            q.tint = q.edge = 44;
        }
        if (scene == 2) {
            q.rect = (ArkRect){-37, -11, 150, 110};
            q.radius = 24;
            q.color = 0x17243d;
            q.tint = 160;
        }
        if (scene == 3) {
            q.rect = (ArkRect){171, 91, 160, 92};
            q.radius = 24;
        }
        if (scene == 4) {
            q.rect = (ArkRect){70, 50, 35, 35};
            q.radius = 17;
        }
        if (scene == 5) {
            q.rect = (ArkRect){0, 0, W, H};
            q.radius = q.tint = q.edge = 0;
        }
        if (scene == 6) {
            q.flags = ARK_GLASS_SHADOW;
            q.rect.h = 80;
            q.radius = 18;
            q.shadow = (ArkRect){8, 8, 176, 110};
        }
        reference(&q);
        check(ark_call(ARK_SYS_COMPOSITOR, &q, sizeof q) == 0, 10 + scene);
        uint64_t total = 0;
        unsigned maximum = 0;
        for (unsigned i = 0; i < W * H; i++) {
            int x = (int)(i % W), y = (int)(i / W);
            bool outside = x < q.rect.x || x >= q.rect.x + q.rect.w || y < q.rect.y ||
                           y >= q.rect.y + q.rect.h;
            check(!(pixels[i] & 0xff000000u), 20 + scene);
            if (outside)
                check(pixels[i] == before[i], 30 + scene);
            for (unsigned shift = 0; shift < 24; shift += 8) {
                int d = (int)((pixels[i] >> shift) & 255) - (int)((expected[i] >> shift) & 255);
                if (d < 0)
                    d = -d;
                total += (unsigned)d;
                if ((unsigned)d > maximum)
                    maximum = (unsigned)d;
            }
        }
        char n[24];
        log_text("[glass-probe] scene=");
        uint_to_str(scene, n);
        log_text(n);
        log_text(" max_error=");
        uint_to_str(maximum, n);
        log_text(n);
        log_text(" mean_error_milli=");
        uint_to_str(total * 1000 / (W * H * 3), n);
        log_text(n);
        log_text("\n");
        check(maximum <= 8 && total * 1000 / (W * H * 3) <= 1000, 40 + scene);
        /* Change the real backing pixels and require a fresh GPU sample. */
        uint32_t prior = pixels[64 * W + 96];
        memset(pixels, 0x44, sizeof pixels);
        q.rect = (ArkRect){0, 0, W, H};
        q.radius = q.tint = q.edge = q.flags = 0;
        q.shadow = (ArkRect){0};
        check(ark_call(ARK_SYS_COMPOSITOR, &q, sizeof q) == 0, 50 + scene);
        check(pixels[64 * W + 96] != prior, 60 + scene);
    }
    ArkGlassRequest q = {.op = ARK_GLASS_RENDER,
                         .pixels = (uintptr_t)pixels,
                         .width = W,
                         .height = H,
                         .stride = W,
                         .rect = {0, 0, W, H}};
    q.pixels = UINT64_MAX - 4;
    check(ark_call(ARK_SYS_COMPOSITOR, &q, sizeof q) == -14, 80);
    q.pixels = (uintptr_t)pixels;
    q.rect.w = INT32_MAX;
    check(ark_call(ARK_SYS_COMPOSITOR, &q, sizeof q) == -22, 81);
    q.rect.w = W;
    q.flags = UINT32_MAX;
    check(ark_call(ARK_SYS_COMPOSITOR, &q, sizeof q) == -22, 82);
    q.flags = 0;
    q.shadow = (ArkRect){INT32_MAX, 0, 1, 1};
    check(ark_call(ARK_SYS_COMPOSITOR, &q, sizeof q) == -22, 83);
    q.shadow = (ArkRect){0};
    q.radius = UINT32_MAX;
    check(ark_call(ARK_SYS_COMPOSITOR, &q, sizeof q) == -22, 84);
    check(ark_syscall6(ARK_SYS_COMPOSITOR, 0x1000000, sizeof query, 0, 0, 0, 0) == -14, 85);
    /* Fully hidden windows must neither touch pixels nor enqueue GPU work. */
    memcpy(before, pixels, sizeof before);
    q.radius = 0;
    const ArkRect hidden[] = {{-W, 0, W, H}, {W, 0, W, H}, {0, -H, W, H}, {0, H, W, H}};
    for (unsigned i = 0; i < 4; i++) {
        q.rect = hidden[i];
        check(ark_call(ARK_SYS_COMPOSITOR, &q, sizeof q) == 0, 86 + i);
        for (unsigned p = 0; p < W * H; p++)
            check(pixels[p] == before[p], 86 + i);
    }
    uint32_t child = (uint32_t)ark_syscall6(900, 0, 0, 0, 0, 0, 0);
    check(child > 1, 90);
    int64_t status;
    do {
        ark_yield();
        status = ark_syscall6(901, child, 0, 0, 0, 0, 0);
    } while (status == INT64_MIN);
    check(status == 0, 91);
    query = (ArkGlassRequest){.op = ARK_GLASS_QUERY};
    check(ark_call(ARK_SYS_COMPOSITOR, &query, sizeof query) == 0 && query.jobs == 14 &&
              query.passes == 56 && query.fences >= 14 && query.uploaded_bytes &&
              query.readback_bytes,
          92);
    log_text("[glass-probe] GPU pixels, live sampling, clipping, XRGB, pointer/geometry/capability "
             "isolation PASS\n");
    return 0;
}
#else
int main(void) {
    ArkGlassRequest q = {.op = ARK_GLASS_QUERY};
    check(ark_call(ARK_SYS_COMPOSITOR, &q, sizeof q) == -1, 100);
    check(ark_syscall6(ARK_SYS_COMPOSITOR, 0, sizeof q, 0, 0, 0, 0) == -1, 101);
    log_text("[glass-probe] untrusted GPU access rejected\n");
    return 0;
}
#endif
