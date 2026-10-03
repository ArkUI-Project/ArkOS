#include "ribbon.h"
#include "raster.h"
#include "motion.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define W 960
#define H 640
#define TW 680
#define TH 460
static uint32_t *screen, *texture;
void ribbon_draw_reference(uint32_t *, int, int, const uint32_t *, int, int, int, int, int, int,
                           int, int, int);
static void background(void) {
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            screen[y * W + x] = 0x16243cu + (uint32_t)(y * 33 / H) * 0x010101u;
}
static void fixture(void) {
    for (int y = 0; y < TH; ++y)
        for (int x = 0; x < TW; ++x) {
            uint32_t c = y < 48 ? 0xdce9f5u : x < 152 ? 0xcbdceeu : 0xf7f9fcu;
            if (y > 64 && x > 178 && (y - 70) % 40 < 9 && x < 610 - ((y / 40) % 4) * 55)
                c = 0x294f70;
            if (y > 70 && x > 15 && x < 124 && (y - 70) % 46 < 9)
                c = 0x6b86a5;
            if (y < 30 && y > 18 && x > 38 && x < 205)
                c = 0x345174;
            if (x > TW - 90 && y < 29 && y > 17)
                c = 0x7c91a9;
            texture[y * TW + x] = c;
        }
}
static unsigned changed(void) {
    unsigned n = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            n += screen[y * W + x] != (0x16243cu + (uint32_t)(y * 33 / H) * 0x010101u);
    return n;
}
static void ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; ++i) {
        unsigned char b[3] = {(unsigned char)(screen[i] >> 16), (unsigned char)(screen[i] >> 8),
                              (unsigned char)screen[i]};
        assert(fwrite(b, 1, 3, f) == 3);
    }
    fclose(f);
}
int main(int argc, char **argv) {
    screen = malloc((W * H + 32) * sizeof(uint32_t));
    texture = malloc((TW * TH + 32) * sizeof(uint32_t));
    assert(screen && texture);
    fixture();
    for (int i = 0; i < 32; ++i) {
        screen[W * H + i] = 0xc001face;
        texture[TW * TH + i] = 0xc001face;
    }
    background();
    ribbon_draw(screen, W, H, texture, TW, TH, 110, 58, TW, TH, 440, 590, 0);
    assert(!changed());
    ribbon_draw(screen, W, H, texture, TW, TH, 110, 58, TW, TH, 440, 590, -7);
    assert(!changed());
    ribbon_draw(screen, W, H, texture, TW, TH, 110, 58, TW, TH, 440, 590, 65536);
    for (int y = 18; y < TH - 18; ++y)
        for (int x = 18; x < TW - 18; ++x)
            assert(screen[(y + 58) * W + x + 110] == texture[y * TW + x]);
    assert(screen[58 * W + 110] == 0x16243cu + (uint32_t)(58 * 33 / H) * 0x010101u);
    uint32_t last = screen[200 * W + 300];
    ribbon_draw(screen, W, H, texture, TW, TH, 110, 58, TW, TH, 440, 590, INT_MAX);
    assert(last == screen[200 * W + 300]);
    uint32_t *complete = malloc(W * H * sizeof(uint32_t));
    assert(complete);
    memcpy(complete, screen, W * H * sizeof(uint32_t));
    background();
    ribbon_draw(screen, W, H, texture, TW, TH, 110, 58, TW, TH, 440, 590, 65535);
    unsigned discontinuity = 0;
    for (int i = 0; i < W * H; i++) {
        int a = (screen[i] & 255) - (complete[i] & 255);
        if (a > 8 || a < -8)
            discontinuity++;
    }
    assert(discontinuity < 2000);
    free(complete);
    unsigned counts[7];
    int phases[] = {4096, 12000, 23000, 34000, 45000, 57000, 65535};
    uint32_t tiles[((TW + 7) / 8) * ((TH + 7) / 8)];
    motion_texture_tiles(tiles, texture, TW, TH);
    uint32_t *cached = malloc(W * H * sizeof(uint32_t));
    assert(cached);
    for (unsigned k = 0; k < sizeof(phases) / sizeof(*phases); ++k) {
        background();
        ribbon_draw(screen, W, H, texture, TW, TH, 110, 58, TW, TH, 440, 590, phases[k]);
        counts[k] = changed();
        assert(counts[k] > 0);
        if (k)
            assert(counts[k] > counts[k - 1]);
        if (argc > 1) {
            char path[512];
            snprintf(path, sizeof(path), "%s/ribbon-%02u.ppm", argv[1], k);
            ppm(path);
        }
    }
    for (unsigned k = 0; k < sizeof(phases) / sizeof(*phases); ++k) {
        background();
        ribbon_draw_reference(screen, W, H, texture, TW, TH, 110, 58, TW, TH, 440, 590, phases[k]);
        memcpy(cached, screen, W * H * 4);
        background();
        ribbon_draw_cached(screen, W, H, texture, TW, TH, 110, 58, TW, TH, 440, 590, phases[k],
                           tiles);
        assert(!memcmp(cached, screen, W * H * 4));
    }
    for (int phase = 3000; phase < 65536; phase += 4501) {
        background();
        ribbon_draw_reference(screen, W, H, texture, TW, TH, -140, -89, 907, 658, 825, 599, phase);
        memcpy(cached, screen, W * H * 4);
        background();
        ribbon_draw_cached(screen, W, H, texture, TW, TH, -140, -89, 907, 658, 825, 599, phase,
                           tiles);
        assert(!memcmp(cached, screen, W * H * 4));
    }
    free(cached);
    /* The upper half unfolds before the lower right: intermediate shape is not a scaled rectangle.
     */
    background();
    ribbon_draw(screen, W, H, texture, TW, TH, 110, 58, TW, TH, 440, 590, 34000);
    assert(screen[160 * W + 250] != (0x16243cu + (uint32_t)(160 * 33 / H) * 0x010101u));
    assert(screen[450 * W + 650] == (0x16243cu + (uint32_t)(450 * 33 / H) * 0x010101u));
    /* Interior remains colored/textured; no white sheet overlay replaces source content. */
    unsigned dark = 0, light = 0;
    for (int y = 60; y < 500; y++)
        for (int x = 110; x < 790; x++) {
            uint32_t c = screen[y * W + x];
            if ((c & 255) > 100 && (c & 255) < 150)
                dark++;
            if ((c & 255) > 220)
                light++;
        }
    assert(dark > 100 && light > 1000);
    /* Clipped and extreme inputs exercise all triangle bounds under ASan/UBSan. */
    const int positions[][4] = {{-300, -200, 1200, 900},
                                {940, 620, 300, 300},
                                {-4090, 100, 4096, 500},
                                {16000, -16000, 4096, 4096},
                                {-16000, 16000, 4096, 4096},
                                {0, 0, 1, 1},
                                {-2, -2, 3, 3}};
    for (unsigned j = 0; j < sizeof(positions) / sizeof(*positions); ++j)
        for (int p = 1; p <= 65536; p += 8191) {
            ribbon_draw(screen, W, H, texture, TW, TH, positions[j][0], positions[j][1],
                        positions[j][2], positions[j][3], 440, 590, p);
        }
    background();
    ribbon_draw(screen, W, H, screen, W, H, 0, 0, W, H, 1, 1, 32768);
    assert(!changed());
    ribbon_draw(screen, W, H, texture, TW, TH, INT_MAX, 0, TW, TH, 0, 0, 30000);
    assert(!changed());
    ribbon_draw(screen, W, H, texture, TW, TH, 0, 0, 0, TH, 0, 0, 30000);
    assert(!changed());
    ribbon_draw(screen, W, H, texture, 0, TH, 0, 0, TW, TH, 0, 0, 30000);
    assert(!changed());
    ribbon_draw(0, W, H, texture, TW, TH, 0, 0, TW, TH, 0, 0, 30000);
    for (int i = 0; i < 32; ++i) {
        assert(screen[W * H + i] == 0xc001face);
        assert(texture[TW * TH + i] == 0xc001face);
    }
    clock_t start = clock();
    for (int i = 0; i < 20; ++i) {
        background();
        ribbon_draw(screen, W, H, texture, TW, TH, 110, 58, TW, TH, 440, 590, 36000 + i * 500);
    }
    printf("Ribbon endpoint/texture/curvature/clipping tests passed; midframe %.2f ms; areas",
           (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC / 20.0);
    for (unsigned i = 0; i < 7; i++)
        printf(" %u", counts[i]);
    puts("");
    free(texture);
    free(screen);
    return 0;
}
