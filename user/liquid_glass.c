/* Copyright 2025 Kyant. Licensed under Apache-2.0; see
 * third_party/android-liquid-glass/LICENSE and PORT.md.
 * Modified 2026 by ArkOS: native C math, XRGB sampling and clocked springs.
 * Original shader formulae retain signed distance, normal and dispersion. */
#include "liquid_glass.h"
static float minimum(float a, float b) {
    return a < b ? a : b;
}
static float maximum(float a, float b) {
    return a > b ? a : b;
}
static float sign(float x) {
    return x < 0 ? -1 : x > 0 ? 1 : 0;
}
static void gradient(float x, float y, float hw, float hh, float r, float *gx, float *gy) {
    float qx = __builtin_fabsf(x) - (hw - r), qy = __builtin_fabsf(y) - (hh - r);
    if (qx >= 0 || qy >= 0) {
        qx = maximum(qx, 0);
        qy = maximum(qy, 0);
        float len = __builtin_sqrtf(qx * qx + qy * qy);
        *gx = len ? sign(x) * qx / len : 0;
        *gy = len ? sign(y) * qy / len : 0;
    } else {
        *gx = qx >= qy ? sign(x) : 0;
        *gy = qx < qy ? sign(y) : 0;
    }
}
void liquid_glass_lens(float x, float y, float w, float h, float r, float height, float amount,
                       bool depth, bool dispersion, LiquidLens *out) {
    *out = (LiquidLens){0};
    float hw = w * .5f, hh = h * .5f, cx = x - hw, cy = y - hh;
    if (w <= 0 || h <= 0)
        return;
    r = minimum(maximum(r, 0), minimum(hw, hh));
    float qx = __builtin_fabsf(cx) - (hw - r), qy = __builtin_fabsf(cy) - (hh - r),
          ox = maximum(qx, 0), oy = maximum(qy, 0);
    float sd = __builtin_sqrtf(ox * ox + oy * oy) - r + minimum(maximum(qx, qy), 0);
    out->distance = sd;
    float gx, gy;
    gradient(cx, cy, hw, hh, minimum(r * 1.5f, minimum(hw, hh)), &gx, &gy);
    out->highlight = __builtin_fabsf((gx + gy) * .7071067811865475f);
    if (height <= 0 || amount <= 0 || -sd >= height)
        return;
    float t = 1 + minimum(sd, 0) / height,
          d = (1 - __builtin_sqrtf(maximum(0, 1 - t * t))) * amount;
    if (depth) {
        float len = __builtin_sqrtf(cx * cx + cy * cy);
        if (len) {
            gx += cx / len;
            gy += cy / len;
        }
    }
    float len = __builtin_sqrtf(gx * gx + gy * gy);
    if (len) {
        gx /= len;
        gy /= len;
    }
    out->dx = d * gx;
    out->dy = d * gy;
    if (dispersion) {
        float intensity = cx * cy / (hw * hh);
        out->dispersion_x = out->dx * intensity;
        out->dispersion_y = out->dy * intensity;
    }
}
static int byte(float n) {
    int i = (int)(n + .5f);
    return i < 0 ? 0 : i > 255 ? 255 : i;
}
uint32_t liquid_glass_vibrancy(uint32_t c) {
    float r = (float)((c >> 16) & 255), g = (float)((c >> 8) & 255), b = (float)(c & 255),
          l = (r * .213f + g * .715f + b * .072f) * .5f;
    return ((uint32_t)byte(r * 1.5f - l) << 16) | ((uint32_t)byte(g * 1.5f - l) << 8) |
           (uint32_t)byte(b * 1.5f - l);
}
static uint32_t mix256(uint32_t a, uint32_t b, unsigned n) {
    unsigned inv = 256 - n;
    return ((((a & 0xff00ffu) * inv + (b & 0xff00ffu) * n) >> 8) & 0xff00ffu) |
           ((((a & 0xff00u) * inv + (b & 0xff00u) * n) >> 8) & 0xff00u);
}
uint32_t liquid_glass_sample(const uint32_t *p, int stride, int w, int h, int x, int y) {
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x > (w - 1) * 256)
        x = (w - 1) * 256;
    if (y > (h - 1) * 256)
        y = (h - 1) * 256;
    int ix = x >> 8, iy = y >> 8, nx = ix + 1 < w ? ix + 1 : ix, ny = iy + 1 < h ? iy + 1 : iy;
    unsigned fx = (unsigned)x & 255, fy = (unsigned)y & 255;
    if (!fx && !fy)
        return p[(unsigned)iy * stride + ix];
    if (!fy)
        return mix256(p[(unsigned)iy * stride + ix], p[(unsigned)iy * stride + nx], fx);
    if (!fx)
        return mix256(p[(unsigned)iy * stride + ix], p[(unsigned)ny * stride + ix], fy);
    return mix256(mix256(p[(unsigned)iy * stride + ix], p[(unsigned)iy * stride + nx], fx),
                  mix256(p[(unsigned)ny * stride + ix], p[(unsigned)ny * stride + nx], fx), fy);
}
uint32_t liquid_glass_dispersion(const uint32_t *p, int stride, int w, int h, int x, int y, int dx,
                                 int dy) {
    if (!dx && !dy)
        return liquid_glass_sample(p, stride, w, h, x, y);
    uint32_t c[7];
    for (int i = 0; i < 7; i++)
        c[i] = liquid_glass_sample(p, stride, w, h, x + dx * (3 - i) / 3, y + dy * (3 - i) / 3);
    unsigned r = ((((c[0] >> 16) & 255) + ((c[1] >> 16) & 255) + ((c[2] >> 16) & 255)) * 2 +
                  ((c[6] >> 16) & 255)) /
                 7;
    unsigned g = (((c[1] >> 8) & 255) +
                  (((c[2] >> 8) & 255) + ((c[3] >> 8) & 255) + ((c[4] >> 8) & 255)) * 2) /
                 7;
    unsigned b = ((c[4] & 255) + (c[5] & 255) + (c[6] & 255)) / 3;
    return (r << 16) | (g << 8) | b;
}
/* Bounded exp/sine evaluation keeps the port freestanding. Springs use the
 * upstream damping ratio .5 and stiffness 300 with continuous retargeting. */
static float exponential(float x) {
    int n = (int)(x * 1.4426950408889634f);
    float t = x - (float)n * .6931471805599453f,
          v = 1 + t * (1 + t * (.5f +
                                t * (.1666666667f +
                                     t * (.0416666667f + t * (.0083333333f + t * .0013888889f)))));
    if (n > 0)
        while (n--)
            v *= 2;
    else
        while (n++)
            v *= .5f;
    return v;
}
static void sine_cosine(float x, float *s, float *c) {
    while (x > 3.14159265359f)
        x -= 6.28318530718f;
    while (x < -3.14159265359f)
        x += 6.28318530718f;
    float xx = x * x;
    *s = x * (1 + xx * (-.166666666667f +
                        xx * (.008333333333f +
                              xx * (-.0001984126984f +
                                    xx * (.0000027557319f +
                                          xx * (-.0000000250521f + xx * .00000000016059f))))));
    *c = 1 + xx * (-.5f +
                   xx * (.041666666667f +
                         xx * (-.001388888889f +
                               xx * (.0000248015873f +
                                     xx * (-.0000002755732f +
                                           xx * (.0000000020877f + xx * (-.00000000001147f)))))));
}
bool liquid_spring_update(LiquidSpring *s, uint64_t now) {
    if (!s->active)
        return false;
    float old = s->value, dt = now > s->at ? (float)(now - s->at) * .001f : 0;
    if (dt > 1) {
        s->value = s->target;
        s->velocity = 0;
        s->active = false;
    } else {
        float sn, cs, a = s->value - s->target, b = (s->velocity + 8.66025403784f * a) / 15;
        sine_cosine(15 * dt, &sn, &cs);
        float e = exponential(-8.66025403784f * dt);
        s->value = s->target + e * (a * cs + b * sn);
        s->velocity =
            e * ((15 * b - 8.66025403784f * a) * cs + (-15 * a - 8.66025403784f * b) * sn);
        if (__builtin_fabsf(s->value - s->target) < .001f && __builtin_fabsf(s->velocity) < .001f) {
            s->value = s->target;
            s->velocity = 0;
            s->active = false;
        }
    }
    s->at = now;
    return old != s->value;
}
void liquid_spring_target(LiquidSpring *s, float target, uint64_t now) {
    liquid_spring_update(s, now);
    s->target = target;
    s->at = now;
    s->active = s->value != target || s->velocity != 0;
}
float liquid_glass_tanh(float x) {
    if (x > 6)
        return 1;
    if (x < -6)
        return -1;
    float e = exponential(2 * x);
    return (e - 1) / (e + 1);
}

/* sigma=2 finite Gaussian. Three channels occupy separate 16-bit lanes in
 * one integer; the total weight is 256, so accumulation cannot cross lanes.
 * This preserves the exact kernel while avoiding vector emulation overhead. */
void liquid_glass_gaussian_row(uint32_t *dst, const uint32_t *const rows[13], int count) {
    for (int x = 0; x < count; x++) {
        uint64_t sum = 0;
#pragma GCC unroll 13
        for (unsigned k = 0; k < 13; k++) {
            static const unsigned weights[13] = {1, 2, 7, 17, 31, 45, 50, 45, 31, 17, 7, 2, 1};
            uint32_t c = rows[k][x];
            uint64_t packed = (uint64_t)(c & 0xff00ffu) | ((uint64_t)(c & 0xff00u) << 24);
            sum += packed * weights[k];
        }
        dst[x] = (uint32_t)(((sum >> 8) & 0xff00ffu) | ((sum >> 32) & 0xff00u));
    }
}
