/* Deterministic exact-pixel comparison and host-only primitive timing.
 * Run through compositor_benchmark.py; timings are not guest frame rates. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint32_t *old_pixels(void), *new_pixels(void);
void old_config(int, int, int, int), new_config(int, int, int, int);
void old_draw(int, int, int, int, int, int, uint32_t, int),
    new_draw(int, int, int, int, int, int, uint32_t, int);
static uint32_t rng = 0x631fa291;
static uint32_t random32(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static void reset(int w, int h) {
    uint32_t *a = old_pixels(), *b = new_pixels();
    for (int i = 0; i < w * h; i++)
        a[i] = b[i] = random32() & 0xffffff;
}
static void compare(int kind, int x, int y, int w, int h, int r, int a, int dark, int reduced,
                    int sw, int sh) {
    old_config(sw, sh, dark, reduced);
    new_config(sw, sh, dark, reduced);
    reset(sw, sh);
    uint32_t c = random32() & 0xffffff;
    old_draw(kind, x, y, w, h, r, c, a);
    new_draw(kind, x, y, w, h, r, c, a);
    if (memcmp(old_pixels(), new_pixels(), (size_t)sw * sh * 4)) {
        for (int i = 0; i < sw * sh; i++)
            if (old_pixels()[i] != new_pixels()[i]) {
                fprintf(stderr,
                        "FAIL kind=%d rect=%d,%d,%d,%d radius=%d alpha=%d dark=%d reduced=%d "
                        "pixel=%d,%d old=%06x new=%06x\n",
                        kind, x, y, w, h, r, a, dark, reduced, i % sw, i / sw, old_pixels()[i],
                        new_pixels()[i]);
                exit(1);
            }
    }
}
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
int main(void) {
    unsigned checks = 0;
    for (int kind = 0; kind < 4; kind++)
        for (int i = 0; i < 800; i++) {
            int w = 1 + random32() % 400, h = 1 + random32() % 280,
                x = (int)(random32() % 500) - 150, y = (int)(random32() % 380) - 100;
            int r = (int)(random32() % 300) - 10,
                a = kind == 3 ? (int)(random32() % 256) : (int)(random32() % 290) - 17;
            compare(kind, x, y, w, h, r, a, i & 1, (i % 5) == 0, 320, 240);
            checks++;
        }
    for (int kind = 0; kind < 5; kind++)
        for (int dark = 0; dark < 2; dark++)
            for (int reduced = 0; reduced < 2; reduced++) {
                compare(kind, 122, 92, 920, 548, 18, 170, dark, reduced, 1280, 800);
                checks++;
                compare(kind, -7, -11, 1920, 1200, 32, 135, dark, reduced, 1920, 1200);
                checks++;
            }
    printf("Exact pixel comparisons: %u cases passed (including clipping, tiny/oversized radii, "
           "light/dark, reduced transparency, 1920x1200).\n",
           checks);
    const char *names[] = {"rounded fill", "stroke", "shadow", "glass", "window composite"};
    old_config(1280, 800, 0, 0);
    new_config(1280, 800, 0, 0);
    reset(1280, 800);
    for (int kind = 0; kind < 5; kind++) {
        const int n = 40;
        old_draw(kind, 122, 92, 920, 548, 18, 0xf8fbff, 170);
        new_draw(kind, 122, 92, 920, 548, 18, 0xf8fbff, 170);
        double before = now();
        for (int i = 0; i < n; i++)
            old_draw(kind, 122, 92, 920, 548, 18, 0xf8fbff, 170);
        double old_ms = (now() - before) * 1000 / n;
        before = now();
        for (int i = 0; i < n; i++)
            new_draw(kind, 122, 92, 920, 548, 18, 0xf8fbff, 170);
        double new_ms = (now() - before) * 1000 / n;
        printf("%-18s old %.3f ms new %.3f ms speedup %.2fx\n", names[kind], old_ms, new_ms,
               old_ms / new_ms);
    }
    printf("checksum %06x %06x\n", old_pixels()[100 * 1280 + 200], new_pixels()[100 * 1280 + 200]);
}
