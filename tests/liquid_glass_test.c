/* Numerical reference derives from the pinned upstream AGSL and Compose spring.
 * It uses host double math independently of the production float approximations. */
#include "liquid_glass.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static double reference(double x, double y, double w, double h, double radius, double *dx,
                        double *dy, double *highlight) {
    double cx = x - w / 2, cy = y - h / 2, qx = fabs(cx) - (w / 2 - radius),
           qy = fabs(cy) - (h / 2 - radius);
    double distance = hypot(fmax(qx, 0), fmax(qy, 0)) - radius + fmin(fmax(qx, qy), 0);
    double gr = fmin(radius * 1.5, fmin(w, h) / 2), a = fabs(cx) - (w / 2 - gr),
           b = fabs(cy) - (h / 2 - gr), gx, gy;
    if (a >= 0 || b >= 0) {
        a = fmax(a, 0);
        b = fmax(b, 0);
        double len = hypot(a, b);
        gx = len ? copysign(a / len, cx) : 0;
        gy = len ? copysign(b / len, cy) : 0;
    } else {
        gx = a >= b ? copysign(1, cx) : 0;
        gy = a < b ? copysign(1, cy) : 0;
    }
    *highlight = fabs((gx + gy) / sqrt(2));
    double strength =
        -distance >= 12 ? 0 : 24 * (1 - sqrt(fmax(0, 1 - pow(1 + fmin(distance, 0) / 12, 2))));
    *dx = strength * gx;
    *dy = strength * gy;
    return distance;
}
int main(void) {
    unsigned cases = 0;
    for (unsigned shape = 0; shape < 4; shape++) {
        int w = shape == 0   ? 320
                : shape == 1 ? 920
                : shape == 2 ? 72
                             : 36,
            h = shape == 0   ? 72
                : shape == 1 ? 548
                : shape == 2 ? 320
                             : 36,
            r = shape == 1   ? 20
                : shape == 3 ? 18
                             : 34;
        for (int y = 0; y < h; y += 3)
            for (int x = 0; x < w; x += 3) {
                LiquidLens lens;
                liquid_glass_lens(x + .5f, y + .5f, w, h, r, 12, 24, false, true, &lens);
                double dx, dy, light, d = reference(x + .5, y + .5, w, h, r, &dx, &dy, &light);
                assert(fabs(lens.distance - d) < .002);
                assert(fabs(lens.dx - dx) < .006 && fabs(lens.dy - dy) < .006);
                assert(fabs(lens.highlight - light) < .0001);
                assert(fabs(lens.dispersion_x - lens.dx * ((x + .5 - w / 2.) * (y + .5 - h / 2.) /
                                                           (w * h / 4.))) < .0001);
                cases++;
            }
    }
    /* The SIMD and scalar paths must agree, including non-vector tails. */
    uint32_t input[13][41], out[41];
    const uint32_t *rows[13];
    unsigned weights[13] = {1, 2, 7, 17, 31, 45, 50, 45, 31, 17, 7, 2, 1};
    unsigned seed = 42;
    for (unsigned k = 0; k < 13; k++) {
        rows[k] = input[k];
        for (unsigned x = 0; x < 41; x++) {
            seed = seed * 1664525u + 1013904223u;
            input[k][x] = seed & 0xffffff;
        }
    }
    liquid_glass_gaussian_row(out, rows, 41);
    for (unsigned x = 0; x < 41; x++) {
        unsigned c[3] = {0};
        for (unsigned k = 0; k < 13; k++)
            for (unsigned channel = 0; channel < 3; channel++)
                c[channel] += ((input[k][x] >> (channel * 8)) & 255) * weights[k];
        assert(out[x] == ((c[2] >> 8) << 16 | ((c[1] >> 8) << 8) | (c[0] >> 8)));
    }
    uint32_t colors[4] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff};
    assert(liquid_glass_sample(colors, 2, 2, 2, 128, 128) == 0x7f7f7f);
    assert(liquid_glass_sample(colors, 2, 2, 2, -1000, 0) == colors[0]);
    assert(liquid_glass_sample(colors, 2, 2, 2, 9999, 9999) == colors[3]);
    for (int ms = 0; ms <= 900; ms += 5) {
        LiquidSpring s = {0};
        liquid_spring_target(&s, 1, 1000);
        liquid_spring_update(&s, 1000 + ms);
        double t = ms / 1000., expected = 1 - exp(-sqrt(300) * .5 * t) *
                                                  (cos(15 * t) + sqrt(300) * .5 / 15 * sin(15 * t));
        assert(fabs(s.value - expected) < .0011);
    }
    LiquidSpring s = {0};
    liquid_spring_target(&s, 1, 1000);
    liquid_spring_update(&s, 1080);
    float value = s.value, velocity = s.velocity;
    liquid_spring_target(&s, 0, 1080);
    assert(s.value == value && s.velocity == velocity);
    liquid_spring_update(&s, 2500);
    assert(!s.active && s.value == 0);
    for (int i = -120; i <= 120; i++)
        assert(fabs(liquid_glass_tanh(i * .05f) - tanh(i * .05)) < .00005);
    printf("PASS %u upstream lens/dispersion/highlight coordinates, Gaussian channel lanes and "
           "tails, bilinear clipping and spring continuity\n",
           cases);
}
