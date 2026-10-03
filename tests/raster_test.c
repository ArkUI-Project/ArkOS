#include <assert.h>
#include <stdio.h>
#include "raster.h"
#include "cursor.h"
static uint32_t mix_reference(uint32_t a, uint32_t b, unsigned n) {
    uint32_t out = 0;
    for (unsigned shift = 0; shift < 24; shift += 8)
        out |= ((((a >> shift) & 255) * (255 - n) + ((b >> shift) & 255) * n) / 255) << shift;
    return out;
}
int main(void) {
    uint32_t random = 42;
    for (unsigned alpha = 0; alpha <= 255; alpha++)
        for (unsigned count = 0; count < 18; count++) {
            uint32_t guard[20], expected[20];
            for (unsigned i = 0; i < 20; i++) {
                random = random * 1664525u + 1013904223u;
                guard[i] = expected[i] = random;
            }
            uint32_t color = random ^ 0xf5ab9372u;
            for (unsigned i = 1; i <= count; i++)
                if (alpha)
                    expected[i] = mix_reference(expected[i], color, alpha);
            raster_blend_span(guard + 1, (int)count, color, alpha);
            for (unsigned i = 0; i < 20; i++)
                assert(guard[i] == expected[i]);
        }
    puts("PASS four-pixel SSE2 blend equals independent integer channel reference for every alpha, "
         "unaligned spans, tails and guards");
    unsigned partial = 0;
    unsigned long area = 0;
    for (int y = -1; y <= 80; y++)
        for (int x = -1; x <= 120; x++) {
            unsigned a = raster_round_coverage(x, y, 120, 80, 18);
            assert(a <= 255);
            if (x < 0 || y < 0 || x == 120 || y == 80) {
                assert(!a);
                continue;
            }
            assert(a == raster_round_coverage(119 - x, y, 120, 80, 18));
            assert(a == raster_round_coverage(x, 79 - y, 120, 80, 18));
            if (a && a < 255)
                partial++;
            area += a;
        }
    /* Cubic superellipse: quarter-area factor approximately 0.88332. */
    assert(area / 255 > 9435 && area / 255 < 9460 && partial > 40);
    assert(!raster_round_coverage(0, 0, 0, 0, 99));
    assert(raster_round_coverage(3, 3, 8, 8, 0) == 255);
    assert(raster_round_coverage(10, 10, 20, 20, 100) == 255);
    for (int scale = 1; scale <= 2; scale++) {
        unsigned ink = 0, soft = 0;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                unsigned a = raster_cursor_coverage(x, y, scale, 0),
                         b = raster_cursor_coverage(x, y, scale, 1);
                assert(a <= 255 && b <= 255);
                if (a)
                    ink++;
                if (a && a < 255)
                    soft++;
                assert(!b || a);
            }
        assert(ink > 170 && soft > 20);
    }
    for (unsigned shape = 0; shape < ARK_CURSOR_COUNT; shape++)
        for (int scale = 1; scale <= 2; scale++) {
            unsigned area = 0, soft = 0;
            for (int y = -1; y <= 64; y++)
                for (int x = -1; x <= 64; x++) {
                    unsigned c = raster_cursor_shape_coverage(x, y, scale, 0, shape);
                    assert(c <= 255);
                    if (x < 0 || y < 0 || x >= 32 * scale || y >= 32 * scale)
                        assert(!c);
                    area += c;
                    if (c && c < 255)
                        soft++;
                }
            assert(area > 1000);
            if (shape != ARK_CURSOR_TEXT && shape != ARK_CURSOR_CROSSHAIR)
                assert(soft > 0);
        }
    puts("PASS: rounded corners have symmetric subpixel coverage; cursor antialias and bounds");
}
