#define _POSIX_C_SOURCE 200809L
#include "motion.h"
#include "raster.h"
#include <limits.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* Frozen pre-optimization blit only, retained as a test oracle. It contains no
 * old animation/UI implementation and is never linked into ArkOS programs.
 * Compare bit-for-bit before reporting render-only scalar host timings. */
static int floor_q16(int64_t value) {
    int64_t quotient = value / MOTION_ONE;
    if (value < 0 && value % MOTION_ONE)
        --quotient;
    return (int)quotient;
}

static int ceil_q16(int64_t value) {
    int64_t quotient = value / MOTION_ONE;
    if (value > 0 && value % MOTION_ONE)
        ++quotient;
    return (int)quotient;
}

/* 256-based interpolation of red/blue lanes and green, using only 32-bit
 * arithmetic. The lane products together remain below UINT32_MAX. */
static inline uint32_t mix_rgb(uint32_t a, uint32_t b, unsigned fraction) {
    unsigned inverse = 256 - fraction;
    uint32_t rb = (((a & 0x00ff00ffu) * inverse + (b & 0x00ff00ffu) * fraction) >> 8) & 0x00ff00ffu;
    uint32_t g = (((a & 0x0000ff00u) * inverse + (b & 0x0000ff00u) * fraction) >> 8) & 0x0000ff00u;
    return rb | g;
}

static inline uint32_t opacity_rgb(uint32_t below, uint32_t above, unsigned alpha) {
    /* Map exact endpoint 255 to weight 256 without per-channel division. */
    unsigned weight = alpha + (alpha >> 7);
    return mix_rgb(below, above, weight);
}

typedef struct {
    int64_t coordinate, step;
    uint32_t remainder, step_remainder, denominator;
} Sampler;

static Sampler sampler(int first_pixel, int origin_q16, int extent_q16, int source_size) {
    Sampler result;
    int64_t numerator = ((int64_t)first_pixel * MOTION_ONE + MOTION_ONE / 2 - origin_q16) *
                        source_size * MOTION_ONE;
    int64_t quotient = numerator / extent_q16;
    int64_t remainder = numerator % extent_q16;
    if (remainder < 0) {
        --quotient;
        remainder += extent_q16;
    }
    uint64_t advance = (uint64_t)source_size * MOTION_ONE * MOTION_ONE;
    result.coordinate = quotient - MOTION_ONE / 2;
    result.remainder = (uint32_t)remainder;
    result.step = (int64_t)(advance / (unsigned)extent_q16);
    result.step_remainder = (uint32_t)(advance % (unsigned)extent_q16);
    result.denominator = (unsigned)extent_q16;
    return result;
}

static inline void sampler_next(Sampler *s) {
    s->coordinate += s->step;
    /* Each remainder is < INT_MAX, hence their sum fits uint32_t. */
    s->remainder += s->step_remainder;
    if (s->remainder >= s->denominator) {
        s->remainder -= s->denominator;
        ++s->coordinate;
    }
}

static inline int sample_index(int64_t coordinate, int dimension, unsigned *fraction) {
    if (coordinate <= 0) {
        *fraction = 0;
        return 0;
    }
    if (coordinate >= (int64_t)(dimension - 1) * MOTION_ONE) {
        *fraction = 0;
        return dimension - 1;
    }
    *fraction = (unsigned)((uint64_t)coordinate >> 8) & 255;
    return (int)(coordinate >> 16);
}

static unsigned edge_coverage(int pixel, int64_t begin, int64_t end) {
    int64_t lo = (int64_t)pixel * MOTION_ONE;
    int64_t hi = lo + MOTION_ONE;
    if (lo < begin)
        lo = begin;
    if (hi > end)
        hi = end;
    return hi <= lo ? 0 : (unsigned)(hi - lo);
}

static bool valid_surface(const void *buffer, int w, int h) {
    return buffer && w > 0 && h > 0 && w <= MOTION_MAX_DIM && h <= MOTION_MAX_DIM;
}

static void motion_blit_before(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh,
                               int x_q16, int y_q16, int w_q16, int h_q16, unsigned opacity,
                               int corner_radius) {
    if (!valid_surface(dst, dw, dh) || !valid_surface(src, sw, sh) || w_q16 <= 0 || h_q16 <= 0 ||
        !opacity)
        return;
    if (opacity > 255)
        opacity = 255;
    size_t dst_bytes = (size_t)dw * (unsigned)dh * sizeof(*dst);
    size_t src_bytes = (size_t)sw * (unsigned)sh * sizeof(*src);
    uintptr_t da = (uintptr_t)dst, sa = (uintptr_t)src;
    if (da > UINTPTR_MAX - dst_bytes || sa > UINTPTR_MAX - src_bytes ||
        (da < sa + src_bytes && sa < da + dst_bytes))
        return;

    int64_t right_q16 = (int64_t)x_q16 + w_q16;
    int64_t bottom_q16 = (int64_t)y_q16 + h_q16;
    int origin_x = floor_q16(x_q16), origin_y = floor_q16(y_q16);
    int finish_x = ceil_q16(right_q16), finish_y = ceil_q16(bottom_q16);
    int x0 = origin_x < 0 ? 0 : origin_x;
    int y0 = origin_y < 0 ? 0 : origin_y;
    int x1 = finish_x > dw ? dw : finish_x;
    int y1 = finish_y > dh ? dh : finish_y;
    if (x0 >= x1 || y0 >= y1)
        return;

    int rectangle_w = finish_x - origin_x, rectangle_h = finish_y - origin_y;
    int radius = corner_radius < 0 ? 0 : corner_radius;
    if (radius > rectangle_w / 2)
        radius = rectangle_w / 2;
    if (radius > rectangle_h / 2)
        radius = rectangle_h / 2;
    /* raster_round_coverage uses signed 32-bit squared distances. */
    if (radius > MOTION_MAX_DIM / 2)
        radius = MOTION_MAX_DIM / 2;

    unsigned first_coverage = edge_coverage(x0, x_q16, right_q16);
    unsigned last_coverage = edge_coverage(x1 - 1, x_q16, right_q16);
    Sampler initial_x = sampler(x0, x_q16, w_q16, sw);
    Sampler vertical = sampler(y0, y_q16, h_q16, sh);
    for (int y = y0; y < y1; ++y) {
        unsigned fy;
        int sy = sample_index(vertical.coordinate, sh, &fy);
        int sy_next = sy + (sy + 1 < sh);
        const uint32_t *top = src + (size_t)sy * (unsigned)sw;
        const uint32_t *bottom = src + (size_t)sy_next * (unsigned)sw;
        uint32_t *out = dst + (size_t)y * (unsigned)dw;
        unsigned row_alpha = (opacity * edge_coverage(y, y_q16, bottom_q16) + 32768u) >> 16;
        int local_y = y - origin_y;
        bool corner_row = radius && (local_y < radius || local_y >= rectangle_h - radius);
        Sampler horizontal = initial_x;
        for (int x = x0; x < x1; ++x) {
            unsigned alpha = row_alpha;
            if (x == x0)
                alpha = (alpha * first_coverage + 32768u) >> 16;
            else if (x == x1 - 1)
                alpha = (alpha * last_coverage + 32768u) >> 16;
            int local_x = x - origin_x;
            if (corner_row && (local_x < radius || local_x >= rectangle_w - radius)) {
                unsigned coverage =
                    raster_round_coverage(local_x, local_y, rectangle_w, rectangle_h, radius);
                alpha = (alpha * coverage + 127u) / 255u;
            }
            if (alpha) {
                unsigned fx;
                int sx = sample_index(horizontal.coordinate, sw, &fx);
                int sx_next = sx + (sx + 1 < sw);
                uint32_t color = top[sx] & 0x00ffffffu;
                if (fx)
                    color = mix_rgb(color, top[sx_next], fx);
                if (fy) {
                    uint32_t low = bottom[sx] & 0x00ffffffu;
                    if (fx)
                        low = mix_rgb(low, bottom[sx_next], fx);
                    color = mix_rgb(color, low, fy);
                }
                out[x] = alpha == 255 ? color : opacity_rgb(out[x], color, alpha);
            }
            sampler_next(&horizontal);
        }
        sampler_next(&vertical);
    }
}

static uint32_t state = 0x784dc5a1;
static uint32_t rnd(void) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
static uint32_t source[1920 * 1200], old[1920 * 1200], new[1920 * 1200];
static void compare(int dw, int dh, int sw, int sh, int x, int y, int w, int h, unsigned opacity,
                    int radius) {
    for (int i = 0; i < dw * dh; i++)
        old[i] = new[i] = rnd();
    for (int i = 0; i < sw * sh; i++)
        source[i] = rnd();
    motion_blit_before(old, dw, dh, source, sw, sh, x, y, w, h, opacity, radius);
    motion_blit(new, dw, dh, source, sw, sh, x, y, w, h, opacity, radius);
    if (memcmp(old, new, (size_t)dw * dh * 4)) {
        fprintf(stderr, "mismatch %dx%d from %dx%d rectangle %d %d %d %d opacity%u radius%d\n", dw,
                dh, sw, sh, x, y, w, h, opacity, radius);
        abort();
    }
}
static uint64_t ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
int main(int argc, char **argv) {
    bool json = argc > 1 && !strcmp(argv[1], "--json");
    for (int test = 0; test < 6000; test++) {
        int dw = 1 + rnd() % 97, dh = 1 + rnd() % 89, sw = 1 + rnd() % 79, sh = 1 + rnd() % 73;
        if (test % 3 == 0) {
            int scale = MOTION_ONE + (int)(rnd() % (MOTION_ONE / 2));
            compare(dw, dh, sw, sh, (dw * (MOTION_ONE - scale)) / 2,
                    (dh * (MOTION_ONE - scale)) / 2, dw * scale, dh * scale, rnd() % 256, 0);
        } else if (test % 3 == 1)
            compare(dw, dh, sw, sh, ((int)(rnd() % 60) - 30) * MOTION_ONE,
                    ((int)(rnd() % 50) - 25) * MOTION_ONE, sw * MOTION_ONE, sh * MOTION_ONE, 255,
                    (int)(rnd() % 50) - 2);
        else
            compare(dw, dh, sw, sh, (int)(rnd() % (60 * MOTION_ONE)) - 30 * MOTION_ONE,
                    (int)(rnd() % (60 * MOTION_ONE)) - 30 * MOTION_ONE,
                    1 + rnd() % (100 * MOTION_ONE), 1 + rnd() % (100 * MOTION_ONE), rnd() % 300,
                    (int)(rnd() % 25) - 3);
    }
    for (int i = 0; i < 5; i++) {
        int w = i & 1 ? 1920 : 1024, h = i & 1 ? 1200 : 800,
            scale = MOTION_ONE + (MOTION_ONE / 5) * (5 - i) / 5;
        compare(w, h, w, h, w * (MOTION_ONE - scale) / 2, (h - 104) * (MOTION_ONE - scale) / 2,
                w * scale, h * scale, 35 + i * 42, 0);
    }
    if (!json)
        puts("PASS: 6,000 deterministic old/new random blits + five 1024/1920 launcher frames are "
             "pixel-identical");
    const int width = 1024, height = 800, scale = MOTION_ONE + MOTION_ONE / 10;
    for (int i = 0; i < width * height; i++)
        source[i] = rnd() & 0xffffff;
    unsigned reps = 20;
    uint64_t start = ns();
    for (unsigned i = 0; i < reps; i++)
        motion_blit_before(
            old, width, height, source, width, height, width * (MOTION_ONE - scale) / 2,
            (height - 104) * (MOTION_ONE - scale) / 2, width * scale, height * scale, 180, 0);
    uint64_t before = ns() - start;
    start = ns();
    for (unsigned i = 0; i < reps; i++)
        motion_blit(new, width, height, source, width, height, width * (MOTION_ONE - scale) / 2,
                    (height - 104) * (MOTION_ONE - scale) / 2, width * scale, height * scale, 180,
                    0);
    uint64_t after = ns() - start;
    uint64_t launcher_before = before / reps / 1000, launcher_after = after / reps / 1000;
    if (!json)
        printf("host scalar 1024x800 launcher: old %llu us/frame, new %llu us/frame, speedup "
               "%llu.%02llu x\n",
               (unsigned long long)(before / reps / 1000),
               (unsigned long long)(after / reps / 1000), (unsigned long long)(before / after),
               (unsigned long long)(before * 100 / after % 100));
    start = ns();
    for (unsigned i = 0; i < reps; i++)
        motion_blit_before(old, width, height, source, width, height, 0, 0, width * MOTION_ONE,
                           height * MOTION_ONE, 255, 18);
    before = ns() - start;
    start = ns();
    for (unsigned i = 0; i < reps; i++)
        motion_blit(new, width, height, source, width, height, 0, 0, width * MOTION_ONE,
                    height * MOTION_ONE, 255, 18);
    after = ns() - start;
    if (!json)
        printf("host scalar opaque1:1 rounded: old %llu us/frame, new %llu us/frame, speedup "
               "%llu.%02llu x\n",
               (unsigned long long)(before / reps / 1000),
               (unsigned long long)(after / reps / 1000), (unsigned long long)(before / after),
               (unsigned long long)(before * 100 / after % 100));
    if (json)
        printf("{\"pixel_identical\":true,\"random_cases\":6000,\"large_frames\":5,\"width\":1024,"
               "\"height\":800,\"repetitions\":20,\"clock\":\"CLOCK_MONOTONIC\",\"platform\":"
               "\"scalar-host-render-only\",\"launcher_us\":{\"before\":%llu,\"after\":%llu},"
               "\"native_opaque_us\":{\"before\":%llu,\"after\":%llu}}\n",
               (unsigned long long)launcher_before, (unsigned long long)launcher_after,
               (unsigned long long)(before / reps / 1000),
               (unsigned long long)(after / reps / 1000));
}
