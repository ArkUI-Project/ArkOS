/* Host-only numerical/reference tests for the native freestanding renderer.
 * cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   -Iinclude user/motion.c user/raster.c tests/motion_host_test.c -lm -o /tmp/motion-test
 * ASAN_OPTIONS=detect_leaks=0 /tmp/motion-test
 */
#include "motion.h"
#include "raster.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double reference_ease(double p) {
    double lo = 0, hi = 1;
    for (unsigned i = 0; i < 60; ++i) {
        double t = (lo + hi) / 2;
        double x = .6 * t * (1 - t) + t * t * t;
        if (x < p)
            lo = t;
        else
            hi = t;
    }
    double t = (lo + hi) / 2;
    return t * t * (3 - 2 * t);
}

static void test_motion(void) {
    assert(motion_ease(-1) == 0 && motion_ease(INT_MIN) == 0);
    assert(motion_ease(MOTION_ONE) == MOTION_ONE && motion_ease(INT_MAX) == MOTION_ONE);
    int previous = 0;
    for (int p = 0; p <= MOTION_ONE; ++p) {
        int value = motion_ease(p);
        assert(value >= previous && value <= MOTION_ONE);
        if (!(p % 97)) {
            double exact = reference_ease((double)p / MOTION_ONE) * MOTION_ONE;
            assert(fabs(value - exact) < 7);
        }
        previous = value;
    }
    assert(motion_lerp(INT_MIN, INT_MAX, 0) == INT_MIN);
    assert(motion_lerp(INT_MIN, INT_MAX, MOTION_ONE) == INT_MAX);
    assert(motion_lerp(INT_MIN, INT_MAX, MOTION_ONE / 2) == -1);
    assert(motion_lerp(INT_MAX, INT_MIN, MOTION_ONE / 2) == 0);
    assert(motion_lerp(50, -70, -10) == 50);
    assert(motion_lerp(50, -70, INT_MAX) == -70);

    MotionTrack t = {.value = 0};
    motion_start(&t, MOTION_ONE, 100, 240);
    assert(t.active && t.value == 0);
    assert(!motion_update(&t, 99) && t.value == 0);
    previous = t.value;
    for (uint64_t now = 100; now <= 340; ++now) {
        int before = t.value;
        bool changed = motion_update(&t, now);
        assert(changed == (t.value != before));
        assert(t.value >= previous && t.value <= MOTION_ONE);
        previous = t.value;
    }
    assert(t.value == MOTION_ONE && !t.active);
    assert(!motion_update(&t, UINT64_MAX));

    motion_start(&t, 0, 500, 200);
    MotionTrack evaluation = t;
    motion_update(&evaluation, 573);
    int exact_current = evaluation.value;
    motion_start(&t, 90000, 573, 173);
    assert(t.from == exact_current && t.value == exact_current);
    assert(!motion_update(&t, 573));
    assert(motion_update(&t, 746) && t.value == 90000 && !t.active);
    motion_start(&t, -12000, 747, 0);
    assert(t.value == -12000 && !t.active);
    motion_start(&t, -12000, 748, 100);
    assert(!t.active && !motion_update(&t, 750));

    t = (MotionTrack){.value = INT_MIN};
    motion_start(&t, INT_MAX, UINT64_MAX - 100, UINT_MAX);
    assert(!motion_update(&t, 0)); /* regressing / wrapped timestamp stays bounded */
    motion_update(&t, UINT64_MAX);
    assert(t.active && t.value >= INT_MIN && t.value <= INT_MAX);
    t = (MotionTrack){.value = INT_MIN};
    motion_start(&t, INT_MAX, 0, UINT_MAX);
    motion_update(&t, UINT_MAX);
    assert(!t.active && t.value == INT_MAX);
    t = (MotionTrack){.from = 10, .to = 20, .value = 10, .active = true, .duration = 0};
    assert(motion_update(&t, 0) && t.value == 20 && !t.active);
    motion_start(NULL, 1, 1, 1);
    assert(!motion_update(NULL, 1));
    puts("PASS: monotonic cubic-bezier, Q16 endpoints, continuous retarget, signed extremes, time "
         "boundaries");
}

static int64_t floor_div(int64_t numerator, int64_t denominator) {
    int64_t q = numerator / denominator;
    if (numerator < 0 && numerator % denominator)
        --q;
    return q;
}
static int floor16(int64_t n) {
    return (int)floor_div(n, MOTION_ONE);
}
static int ceil16(int64_t n) {
    return (int)-floor_div(-n, MOTION_ONE);
}
static unsigned coverage(int p, int64_t left, int64_t right) {
    int64_t lo = (int64_t)p * MOTION_ONE, hi = lo + MOTION_ONE;
    if (lo < left)
        lo = left;
    if (hi > right)
        hi = right;
    return hi > lo ? (unsigned)(hi - lo) : 0;
}
static uint32_t channel_mix(uint32_t a, uint32_t b, unsigned f) {
    uint32_t out = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        unsigned v = (((a >> shift) & 255) * (256 - f) + ((b >> shift) & 255) * f) / 256;
        out |= v << shift;
    }
    return out;
}

/* Deliberately uses per-pixel division and separate color channels, independent
 * of the production DDA stepping and packed RB arithmetic. */
static void reference_blit(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh,
                           int xq, int yq, int wq, int hq, unsigned opacity, int radius) {
    if (wq <= 0 || hq <= 0 || !opacity)
        return;
    if (opacity > 255)
        opacity = 255;
    int64_t right = (int64_t)xq + wq, bottom = (int64_t)yq + hq;
    int ox = floor16(xq), oy = floor16(yq), rw = ceil16(right) - ox, rh = ceil16(bottom) - oy;
    if (radius < 0)
        radius = 0;
    if (radius > rw / 2)
        radius = rw / 2;
    if (radius > rh / 2)
        radius = rh / 2;
    if (radius > 2048)
        radius = 2048;
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            unsigned cx = coverage(x, xq, right), cy = coverage(y, yq, bottom);
            if (!cx || !cy)
                continue;
            unsigned alpha = (opacity * cy + 32768u) / MOTION_ONE;
            alpha = (alpha * cx + 32768u) / MOTION_ONE;
            unsigned mask = raster_round_coverage(x - ox, y - oy, rw, rh, radius);
            alpha = (alpha * mask + 127) / 255;
            if (!alpha)
                continue;
            int64_t sxq =
                floor_div(((int64_t)x * MOTION_ONE + MOTION_ONE / 2 - xq) * sw * MOTION_ONE, wq) -
                MOTION_ONE / 2;
            int64_t syq =
                floor_div(((int64_t)y * MOTION_ONE + MOTION_ONE / 2 - yq) * sh * MOTION_ONE, hq) -
                MOTION_ONE / 2;
            if (sxq < 0)
                sxq = 0;
            if (syq < 0)
                syq = 0;
            if (sxq > (int64_t)(sw - 1) * MOTION_ONE)
                sxq = (int64_t)(sw - 1) * MOTION_ONE;
            if (syq > (int64_t)(sh - 1) * MOTION_ONE)
                syq = (int64_t)(sh - 1) * MOTION_ONE;
            int sx = (int)(sxq / MOTION_ONE), sy = (int)(syq / MOTION_ONE);
            int nx = sx + (sx + 1 < sw), ny = sy + (sy + 1 < sh);
            unsigned fx = (unsigned)(sxq / 256) % 256, fy = (unsigned)(syq / 256) % 256;
            uint32_t upper = channel_mix(src[sy * sw + sx], src[sy * sw + nx], fx);
            uint32_t lower = channel_mix(src[ny * sw + sx], src[ny * sw + nx], fx);
            uint32_t color = channel_mix(upper, lower, fy);
            dst[y * dw + x] = channel_mix(dst[y * dw + x], color, alpha + (alpha >> 7));
        }
}

static uint32_t random_state = 0x9a073f21;
static uint32_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void blit_case(int dw, int dh, int sw, int sh, int x, int y, int w, int h, unsigned alpha,
                      int radius) {
    size_t dn = (size_t)dw * dh, sn = (size_t)sw * sh;
    uint32_t *storage = malloc((dn + 16) * sizeof(*storage));
    uint32_t *source_storage = malloc((sn + 16) * sizeof(*source_storage));
    uint32_t *reference = malloc(dn * sizeof(*reference));
    assert(storage && source_storage && reference);
    for (size_t i = 0; i < dn + 16; ++i)
        storage[i] = 0x51793bad;
    for (size_t i = 0; i < sn + 16; ++i)
        source_storage[i] = 0x7bf51083;
    uint32_t *dest = storage + 8, *source = source_storage + 8;
    for (size_t i = 0; i < dn; ++i)
        dest[i] = reference[i] = next_random() & 0xffffff;
    for (size_t i = 0; i < sn; ++i)
        source[i] = next_random();
    motion_blit(dest, dw, dh, source, sw, sh, x, y, w, h, alpha, radius);
    reference_blit(reference, dw, dh, source, sw, sh, x, y, w, h, alpha, radius);
    for (size_t i = 0; i < dn; ++i) {
        if (dest[i] != reference[i]) {
            fprintf(stderr,
                    "blit mismatch (%dx%d <- %dx%d) at %zu, rect %d,%d,%d,%d, a=%u r=%d: %08x != "
                    "%08x\n",
                    dw, dh, sw, sh, i, x, y, w, h, alpha, radius, dest[i], reference[i]);
            abort();
        }
    }
    for (unsigned i = 0; i < 8; ++i) {
        assert(storage[i] == 0x51793bad && storage[dn + 8 + i] == 0x51793bad);
        assert(source_storage[i] == 0x7bf51083 && source_storage[sn + 8 + i] == 0x7bf51083);
    }
    free(reference);
    free(source_storage);
    free(storage);
}

static void test_blit(void) {
    uint32_t source[] = {0xff123456, 0x009abcde, 0x01ff00aa, 0xffffffff};
    uint32_t dest[4] = {0};
    motion_blit(dest, 2, 2, source, 2, 2, 0, 0, 2 * MOTION_ONE, 2 * MOTION_ONE, 255, 0);
    for (unsigned i = 0; i < 4; ++i)
        assert(dest[i] == (source[i] & 0xffffff));
    uint32_t saved[4];
    memcpy(saved, dest, sizeof(dest));
    motion_blit(dest, 2, 2, source, 2, 2, 0, 0, 2 * MOTION_ONE, 2 * MOTION_ONE, 0, 0);
    assert(!memcmp(saved, dest, sizeof(dest)));
    motion_blit(dest, 2, 2, dest, 2, 2, 0, 0, MOTION_ONE, MOTION_ONE, 255, 0);
    assert(!memcmp(saved, dest, sizeof(dest)));
    motion_blit(dest, 4097, 1, source, 2, 2, 0, 0, MOTION_ONE, MOTION_ONE, 255, 0);
    motion_blit(dest, -1, 2, source, 2, 2, 0, 0, MOTION_ONE, MOTION_ONE, 255, 0);
    motion_blit(NULL, 1, 1, source, 2, 2, 0, 0, MOTION_ONE, MOTION_ONE, 255, 0);
    motion_blit(dest, 2, 2, NULL, 2, 2, 0, 0, MOTION_ONE, MOTION_ONE, 255, 0);
    assert(!memcmp(saved, dest, sizeof(dest)));
    blit_case(3, 5, 7, 9, 0, 0, 3 * MOTION_ONE, 5 * MOTION_ONE, 255, 0);
    blit_case(7, 5, 3, 2, -MOTION_ONE / 3, MOTION_ONE / 5, 11 * MOTION_ONE / 2, 9 * MOTION_ONE / 2,
              127, 2);
    blit_case(7, 9, 1, 1, -MOTION_ONE, -MOTION_ONE, 13 * MOTION_ONE, 15 * MOTION_ONE, 255, 100);
    blit_case(3, 3, 2, 2, 1, 1, 1, 1, 255, 0); /* subpixel rectangle */
    blit_case(3, 3, 2, 2, -1, -1, 2, 2, 255, INT_MAX);
    blit_case(11, 13, 3, 7, INT_MIN, INT_MIN, INT_MAX, INT_MAX, 300, INT_MAX);
    blit_case(11, 13, 3, 7, INT_MAX, INT_MAX, INT_MAX, INT_MAX, 255, INT_MAX);
    blit_case(11, 13, 3, 7, -10, -10, INT_MAX, INT_MAX, 255, INT_MAX);
    /* Interior scanline reuse and exact native-size span copies must preserve
     * the independent reference's rounding, clipping and ignored alpha byte. */
    for (int i = 0; i < 24; ++i) {
        int scale = MOTION_ONE + (i + 1) * 997;
        blit_case(31, 35, 17 + i, 23 + i, 31 * (MOTION_ONE - scale) / 2,
                  35 * (MOTION_ONE - scale) / 2, 31 * scale, 35 * scale, i & 1 ? 255 : 127, 0);
        blit_case(31, 35, 29, 41, (i - 12) * MOTION_ONE, (7 - i) * MOTION_ONE, 29 * MOTION_ONE,
                  41 * MOTION_ONE, 255, i);
    }
    uint32_t xrgb[23 * 31], native[33 * 37], native_reference[33 * 37];
    for (unsigned i = 0; i < 23 * 31; i++)
        xrgb[i] = next_random() & 0xffffff;
    for (unsigned i = 0; i < 33 * 37; i++)
        native[i] = native_reference[i] = next_random();
    motion_blit(native, 33, 37, xrgb, 23, 31, -3 * MOTION_ONE, 5 * MOTION_ONE, 23 * MOTION_ONE,
                31 * MOTION_ONE, 255, 9);
    reference_blit(native_reference, 33, 37, xrgb, 23, 31, -3 * MOTION_ONE, 5 * MOTION_ONE,
                   23 * MOTION_ONE, 31 * MOTION_ONE, 255, 9);
    assert(!memcmp(native, native_reference, sizeof native));
    uint32_t tiles[3 * 4];
    for (int y = 0; y < 31; y++)
        for (int x = 0; x < 23; x++)
            xrgb[y * 23 + x] =
                ((x / 8 + y / 8) & 1 ? 0x123456u : 0x97c8e2u) | (next_random() & 0xff000000u);
    motion_texture_tiles(tiles, xrgb, 23, 31);
    for (int phase = 0; phase < 24; phase++) {
        for (unsigned i = 0; i < 33 * 37; i++)
            native[i] = native_reference[i] = next_random();
        int scale = MOTION_ONE + phase * 731;
        motion_blit_cached(native, 33, 37, xrgb, 23, 31, -MOTION_ONE / 3, MOTION_ONE / 7,
                           33 * scale, 37 * scale, 31 + phase * 9, phase % 3, tiles);
        reference_blit(native_reference, 33, 37, xrgb, 23, 31, -MOTION_ONE / 3, MOTION_ONE / 7,
                       33 * scale, 37 * scale, 31 + phase * 9, phase % 3);
        assert(!memcmp(native, native_reference, sizeof native));
    }
    for (unsigned i = 0; i < 600; ++i) {
        int dw = 1 + next_random() % 17, dh = 1 + next_random() % 17;
        int sw = 1 + next_random() % 13, sh = 1 + next_random() % 13;
        int x = (int)(next_random() % (32 * MOTION_ONE)) - 16 * MOTION_ONE;
        int y = (int)(next_random() % (32 * MOTION_ONE)) - 16 * MOTION_ONE;
        int w = 1 + next_random() % (25 * MOTION_ONE), h = 1 + next_random() % (25 * MOTION_ONE);
        blit_case(dw, dh, sw, sh, x, y, w, h, next_random() % 300, (int)(next_random() % 30) - 3);
    }
    puts("PASS: independent per-pixel reference, bilinear DDA, clipping, negative/fractional "
         "coordinates, odd scales, rounded AA, guards");
}

int main(void) {
    test_motion();
    test_blit();
    uint32_t below[19], above[19], blend[19];
    for (unsigned i = 0; i < 19; i++) {
        below[i] = next_random();
        above[i] = next_random();
    }
    for (unsigned alpha = 0; alpha <= 255; alpha++) {
        motion_mix_pixels(blend, below, above, 19, alpha);
        for (unsigned i = 0; i < 19; i++)
            for (unsigned shift = 0; shift <= 16; shift += 8)
                assert(((blend[i] >> shift) & 255) ==
                       ((((below[i] >> shift) & 255) * (255 - alpha) +
                         ((above[i] >> shift) & 255) * alpha) /
                        255));
    }
    puts("PASS: vector crossfade equals independent channel arithmetic for all alpha values");
    return 0;
}
