/* Original ArkOS curved sheet compositor. Geometry follows the reference's
 * narrow rising tip -> broad upper sheet -> bowed lower edge -> flat window.
 * A regular UV mesh retains the application texture while the right edge curls.
 * MIT license. No floating point, heap allocation or per-pixel division. */
#include "ribbon.h"
#include "raster.h"
#include <stddef.h>
#include <stdbool.h>
#define ONE 65536
#define GRID 24
#define MAX_DIM 4096
#define WORLD_LIMIT 16384
typedef struct {
    int x, y, u, v, light;
} Vertex; /* XY Q8, UV and light Q16 */
typedef struct {
    int left, top, right, bottom, tipx, tipy, bow, w, h, tw, th;
} Shape;
typedef struct {
    int64_t value, dx, dy, half, reciprocal;
    bool outer, top_left;
} Edge;
typedef struct {
    uint32_t *dst;
    const uint32_t *src;
    int dw, dh, tw, th, radius;
    unsigned opacity;
    const uint32_t *tiles;
} Surface;
static int minimum(int a, int b) {
    return a < b ? a : b;
}
static int maximum(int a, int b) {
    return a > b ? a : b;
}
static int clamp(int n, int low, int high) {
    return n < low ? low : n > high ? high : n;
}
static int mul(int a, int b) {
    return (int)((int64_t)a * b / ONE);
}
static int lerp(int a, int b, int p) {
    return a + (int)((int64_t)(b - a) * p / ONE);
}
static int smooth(int p) {
    p = clamp(p, 0, ONE);
    return mul(mul(p, p), 3 * ONE - 2 * p);
}
static int out_cubic(int p) {
    p = ONE - clamp(p, 0, ONE);
    return ONE - mul(mul(p, p), p);
}
static uint32_t mix256(uint32_t a, uint32_t b, unsigned p) {
    unsigned q = 256 - p;
    return ((((a & 0xff00ffu) * q + (b & 0xff00ffu) * p) >> 8) & 0xff00ffu) |
           ((((a & 0x00ff00u) * q + (b & 0x00ff00u) * p) >> 8) & 0x00ff00u);
}
static uint32_t sample(const Surface *s, int64_t u, int64_t v) {
    u -= ONE / 2;
    v -= ONE / 2;
    int64_t umax = (int64_t)(s->tw - 1) * ONE, vmax = (int64_t)(s->th - 1) * ONE;
    if (u < 0)
        u = 0;
    else if (u > umax)
        u = umax;
    if (v < 0)
        v = 0;
    else if (v > vmax)
        v = vmax;
    unsigned x = (unsigned)(u >> 16), y = (unsigned)(v >> 16);
    unsigned xx = x + 1 < (unsigned)s->tw ? x + 1 : x, yy = y + 1 < (unsigned)s->th ? y + 1 : y;
    if (s->tiles && (x >> 3) == (xx >> 3) && (y >> 3) == (yy >> 3)) {
        uint32_t color = s->tiles[(size_t)(y >> 3) * ((s->tw + 7) / 8) + (x >> 3)];
        if (color < 0x01000000u)
            return color;
    }
    unsigned fx = (unsigned)(u >> 8) & 255, fy = (unsigned)(v >> 8) & 255;
    uint32_t aa = s->src[(size_t)y * s->tw + x], ab = s->src[(size_t)y * s->tw + xx];
    uint32_t ba = s->src[(size_t)yy * s->tw + x], bb = s->src[(size_t)yy * s->tw + xx];
    if (aa == ab && aa == ba && aa == bb)
        return aa & 0xffffffu;
    uint32_t a = mix256(aa, ab, fx);
    uint32_t b = mix256(ba, bb, fx);
    return mix256(a, b, fy);
}
static unsigned source_coverage(const Surface *s, int64_t u, int64_t v) {
    int x = clamp((int)(u >> 16), 0, s->tw - 1), y = clamp((int)(v >> 16), 0, s->th - 1),
        r = s->radius;
    if (!r || (x >= r && x < s->tw - r) || (y >= r && y < s->th - r))
        return 256;
    unsigned alpha = raster_round_coverage(x, y, s->tw, s->th, r);
    return (alpha * 257 + 128) >> 8;
}
static int64_t area(const Vertex *a, const Vertex *b, const Vertex *c) {
    return (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(b->y - a->y) * (c->x - a->x);
}
static bool outside_edge(const Vertex *a, const Vertex *b, const Surface *s) {
    return (a->u == b->u && (a->u == 0 || a->u == s->tw * ONE)) ||
           (a->v == b->v && (a->v == 0 || a->v == s->th * ONE));
}
static Edge edge(const Vertex *a, const Vertex *b, int x, int y, const Surface *s) {
    int dx = b->x - a->x, dy = b->y - a->y;
    int64_t ax = dx < 0 ? -(int64_t)dx : dx, ay = dy < 0 ? -(int64_t)dy : dy;
    int64_t length = ax > ay ? ax + ay / 2 : ay + ax / 2;
    Edge e = {.value = (int64_t)dx * (y - a->y) - (int64_t)dy * (x - a->x),
              .dx = -(int64_t)dy * 256,
              .dy = (int64_t)dx * 256,
              .half = length * 128,
              .reciprocal = length ? ((int64_t)1 << 24) / length : 0,
              .outer = outside_edge(a, b, s),
              .top_left = dy < 0 || (dy == 0 && dx > 0)};
    return e;
}
static unsigned coverage(const Edge *e, int64_t value) {
    if (!e->outer)
        return value > 0 || (value == 0 && e->top_left) ? 256 : 0;
    if (value >= e->half)
        return 256;
    if (value <= -e->half || !e->reciprocal)
        return 0;
    int alpha = 128 + (int)((value * e->reciprocal) >> 24);
    return (unsigned)clamp(alpha, 0, 256);
}
/* Intersect a scanline with the exact half-plane. Outer AA edges keep a
 * conservative one-pixel fringe; internal top-left ownership stays exact. */
static bool span_edge(const Edge *e, int left, int64_t value, int *first, int *last) {
    int64_t threshold = e->outer ? 1 - e->half : e->top_left ? 0 : 1;
    if (e->dx > 0) {
        int64_t n = threshold - value, offset = n / e->dx;
        if (n > 0 && n % e->dx)
            offset++;
        if (offset > *last - left)
            return false;
        if (offset > *first - left)
            *first = left + (int)offset;
    } else if (e->dx < 0) {
        int64_t n = value - threshold, denominator = -e->dx, offset = n / denominator;
        if (n < 0 && n % denominator)
            offset--;
        if (offset < *first - left)
            return false;
        if (offset < *last - left)
            *last = left + (int)offset;
    } else if (value < threshold)
        return false;
    return true;
}
/* Gradient numerator uses local vertex differences, keeping Q16 products bounded. */
static int64_t gx(int av, int bv, int cv, const Vertex *a, const Vertex *b, const Vertex *c,
                  int64_t determinant) {
    return ((int64_t)(bv - av) * (c->y - a->y) - (int64_t)(cv - av) * (b->y - a->y)) * 256 /
           determinant;
}
static int64_t gy(int av, int bv, int cv, const Vertex *a, const Vertex *b, const Vertex *c,
                  int64_t determinant) {
    return ((int64_t)(b->x - a->x) * (cv - av) - (int64_t)(c->x - a->x) * (bv - av)) * 256 /
           determinant;
}
/* Interior spans already own every covered pixel. Keeping their shader
 * separate removes three edge accumulators and their spills from the hot loop. */
static __attribute__((noinline)) void interior_span(const Surface *s, uint32_t *row, int first,
                                                    int last, int64_t u, int64_t v, int64_t light,
                                                    int64_t du, int64_t dv, int64_t dl, bool solid,
                                                    bool unlit) {
    for (int x = first; x <= last; x++) {
        unsigned alpha = solid ? s->opacity : (source_coverage(s, u, v) * s->opacity + 128) >> 8;
        if (alpha) {
            uint32_t color = sample(s, u, v);
            int shine = unlit ? 0 : clamp((int)(light >> 16), -64, 32);
            if (shine < 0)
                color = mix256(color, 0, (unsigned)-shine);
            else if (shine > 0)
                color = mix256(color, 0xffffffu, (unsigned)shine);
            row[x] = alpha >= 256 ? color : mix256(row[x], color, alpha);
        }
        u += du;
        v += dv;
        light += dl;
    }
}
static void triangle(const Surface *s, Vertex a, Vertex b, Vertex c) {
    int64_t determinant = area(&a, &b, &c);
    if (!determinant)
        return;
    if (determinant < 0) {
        Vertex swap = b;
        b = c;
        c = swap;
        determinant = -determinant;
    }
    int left = maximum(0, (minimum(a.x, minimum(b.x, c.x)) >> 8) - 1);
    int right = minimum(s->dw - 1, (maximum(a.x, maximum(b.x, c.x)) >> 8) + 1);
    int top = maximum(0, (minimum(a.y, minimum(b.y, c.y)) >> 8) - 1);
    int bottom = minimum(s->dh - 1, (maximum(a.y, maximum(b.y, c.y)) >> 8) + 1);
    if (left > right || top > bottom)
        return;
    int px = left * 256 + 128, py = top * 256 + 128;
    Edge e0 = edge(&a, &b, px, py, s), e1 = edge(&b, &c, px, py, s), e2 = edge(&c, &a, px, py, s);
    int64_t dux = gx(a.u, b.u, c.u, &a, &b, &c, determinant),
            duy = gy(a.u, b.u, c.u, &a, &b, &c, determinant);
    int64_t dvx = gx(a.v, b.v, c.v, &a, &b, &c, determinant),
            dvy = gy(a.v, b.v, c.v, &a, &b, &c, determinant);
    int64_t dlx = gx(a.light, b.light, c.light, &a, &b, &c, determinant),
            dly = gy(a.light, b.light, c.light, &a, &b, &c, determinant);
    int64_t u = a.u + (dux * (px - a.x) + duy * (py - a.y)) / 256;
    int64_t v = a.v + (dvx * (px - a.x) + dvy * (py - a.y)) / 256;
    int64_t light = a.light + (dlx * (px - a.x) + dly * (py - a.y)) / 256;
    bool interior = !e0.outer && !e1.outer && !e2.outer;
    int umin = minimum(a.u, minimum(b.u, c.u)), umax = maximum(a.u, maximum(b.u, c.u));
    int vmin = minimum(a.v, minimum(b.v, c.v)), vmax = maximum(a.v, maximum(b.v, c.v));
    bool solid =
        interior && ((umin >= (s->radius + 1) * ONE && umax <= (s->tw - s->radius - 1) * ONE) ||
                     (vmin >= (s->radius + 1) * ONE && vmax <= (s->th - s->radius - 1) * ONE));
    bool unlit = !a.light && !b.light && !c.light;
    for (int yy = top; yy <= bottom; ++yy) {
        int64_t q0 = e0.value, q1 = e1.value, q2 = e2.value, uu = u, vv = v, ll = light;
        uint32_t *row = s->dst + (size_t)yy * s->dw;
        int first = left, last = right;
        bool visible = span_edge(&e0, left, q0, &first, &last) &&
                       span_edge(&e1, left, q1, &first, &last) &&
                       span_edge(&e2, left, q2, &first, &last);
        int skip = first - left;
        q0 += e0.dx * skip;
        q1 += e1.dx * skip;
        q2 += e2.dx * skip;
        uu += dux * skip;
        vv += dvx * skip;
        ll += dlx * skip;
        if (visible && interior)
            interior_span(s, row, first, last, uu, vv, ll, dux, dvx, dlx, solid, unlit);
        else
            for (int xx = first; visible && xx <= last; ++xx) {
                unsigned alpha = coverage(&e0, q0);
                if (alpha) {
                    unsigned a1 = coverage(&e1, q1);
                    alpha = minimum((int)alpha, (int)a1);
                }
                if (alpha) {
                    unsigned a2 = coverage(&e2, q2);
                    alpha = minimum((int)alpha, (int)a2);
                }
                if (alpha) {
                    alpha = solid ? s->opacity
                                  : (alpha * source_coverage(s, uu, vv) * s->opacity + 32768) >> 16;
                    if (alpha) {
                        uint32_t color = sample(s, uu, vv);
                        int shine = unlit ? 0 : clamp((int)(ll >> 16), -64, 32);
                        if (shine < 0)
                            color = mix256(color, 0, (unsigned)-shine);
                        else if (shine > 0)
                            color = mix256(color, 0xffffffu, (unsigned)shine);
                        row[xx] = alpha >= 256 ? color : mix256(row[xx], color, alpha);
                    }
                }
                q0 += e0.dx;
                q1 += e1.dx;
                q2 += e2.dx;
                uu += dux;
                vv += dvx;
                ll += dlx;
            }
        e0.value += e0.dy;
        e1.value += e1.dy;
        e2.value += e2.dy;
        u += duy;
        v += dvy;
        light += dly;
    }
}
static Vertex point(const Shape *s, int column, int row) {
    int u = column * ONE / GRID, v = row * ONE / GRID;
    int curve = mul(4 * v, ONE - v), v3 = mul(mul(v, v), v);
    int lx = lerp(s->left, s->tipx, v), rx = lerp(s->right, s->bottom, v);
    /* An inward right edge, rather than a trapezoid, creates the characteristic tip. */
    rx -= (int)((int64_t)s->w * mul(s->bow, curve) * 15 / 100);
    int xx = lerp(lx, rx, u);
    int ly = lerp(s->top, s->tipy, v);
    int right_y = s->tipy - (int)((int64_t)s->h * s->bow * 32 / 100);
    int ry = lerp(s->top, right_y, v);
    int yy = lerp(ly, ry, u);
    int across = mul(4 * u, ONE - u);
    yy -= (int)((int64_t)s->h * mul(mul(s->bow, across), v3) * 12 / 100);
    /* Tight, low-amplitude ridge/shadow follows the curl; it never replaces texture. */
    int ridge = ONE - (u > 57000 ? u - 57000 : 57000 - u) * 7;
    ridge = mul(clamp(ridge, 0, ONE), clamp(ridge, 0, ONE));
    int edge_light = mul(mul(u, u), mul(u, u));
    edge_light = mul(edge_light, edge_light);
    int strength = mul(s->bow, ONE / 3 + mul(v, 2 * ONE / 3));
    int lighting = mul(strength, 15 * ridge - 27 * edge_light);
    Vertex result = {xx >> 8, yy >> 8, (int)((int64_t)column * s->tw * ONE / GRID),
                     (int)((int64_t)row * s->th * ONE / GRID), lighting};
    return result;
}
static void final_window(const Surface *s, int x, int y, int w, int h) {
    int left = maximum(0, x), top = maximum(0, y), right = minimum(s->dw, x + w),
        bottom = minimum(s->dh, y + h);
    if (left >= right || top >= bottom)
        return;
    if (w == s->tw && h == s->th) {
        /* The cached native window already has the final dimensions. Only the four
         * small corner squares need coverage work; straight spans are masked copies.
         * Keep clearing XRGB's ignored high byte rather than trusting source alpha. */
        int radius = minimum(18, minimum(w / 2, h / 2));
        for (int yy = top; yy < bottom; ++yy) {
            uint32_t *row = s->dst + (size_t)yy * s->dw;
            const uint32_t *source = s->src + (size_t)(yy - y) * w;
            int solid_left = left, solid_right = right;
            if (yy - y < radius || yy - y >= h - radius) {
                solid_left = maximum(left, minimum(right, x + radius));
                solid_right = minimum(right, maximum(left, x + w - radius));
            }
            for (int xx = left; xx < solid_left; ++xx) {
                unsigned alpha = raster_round_coverage(xx - x, yy - y, w, h, radius);
                if (alpha) {
                    uint32_t color = source[xx - x] & 0xffffffu;
                    row[xx] =
                        alpha == 255 ? color : mix256(row[xx], color, (alpha * 257 + 128) >> 8);
                }
            }
            int xx = solid_left;
            for (; xx + 4 <= solid_right; xx += 4) {
                row[xx] = source[xx - x] & 0xffffffu;
                row[xx + 1] = source[xx + 1 - x] & 0xffffffu;
                row[xx + 2] = source[xx + 2 - x] & 0xffffffu;
                row[xx + 3] = source[xx + 3 - x] & 0xffffffu;
            }
            for (; xx < solid_right; ++xx)
                row[xx] = source[xx - x] & 0xffffffu;
            for (; xx < right; ++xx) {
                unsigned alpha = raster_round_coverage(xx - x, yy - y, w, h, radius);
                if (alpha) {
                    uint32_t color = source[xx - x] & 0xffffffu;
                    row[xx] =
                        alpha == 255 ? color : mix256(row[xx], color, (alpha * 257 + 128) >> 8);
                }
            }
        }
        return;
    }
    int64_t dx = (int64_t)s->tw * ONE / w, dy = (int64_t)s->th * ONE / h;
    int64_t v = ((int64_t)(top - y) * 2 + 1) * dy / 2;
    for (int yy = top; yy < bottom; ++yy) {
        int64_t u = ((int64_t)(left - x) * 2 + 1) * dx / 2;
        uint32_t *row = s->dst + (size_t)yy * s->dw;
        for (int xx = left; xx < right; ++xx) {
            unsigned alpha = raster_round_coverage(xx - x, yy - y, w, h, 18);
            if (alpha) {
                uint32_t color = (w == s->tw && h == s->th)
                                     ? s->src[(size_t)(yy - y) * s->tw + xx - x] & 0xffffffu
                                     : sample(s, u, v);
                row[xx] = alpha == 255 ? color : mix256(row[xx], color, (alpha * 257 + 128) >> 8);
            }
            u += dx;
        }
        v += dy;
    }
}
void ribbon_draw_cached(uint32_t *dst, int width, int height, const uint32_t *texture, int tw,
                        int th, int x, int y, int w, int h, int anchor_x, int anchor_y,
                        int progressQ16, const uint32_t *colors) {
    if (progressQ16 <= 0 || !dst || !texture || width < 1 || height < 1 || tw < 1 || th < 1 ||
        w < 1 || h < 1 || width > MAX_DIM || height > MAX_DIM || tw > MAX_DIM || th > MAX_DIM ||
        w > MAX_DIM || h > MAX_DIM || x < -WORLD_LIMIT || x > WORLD_LIMIT || y < -WORLD_LIMIT ||
        y > WORLD_LIMIT || anchor_x < -WORLD_LIMIT || anchor_x > WORLD_LIMIT ||
        anchor_y < -WORLD_LIMIT || anchor_y > WORLD_LIMIT)
        return;
    size_t db = (size_t)width * height * sizeof(uint32_t), sb = (size_t)tw * th * sizeof(uint32_t);
    uintptr_t dp = (uintptr_t)dst, sp = (uintptr_t)texture;
    if (dp > UINTPTR_MAX - db || sp > UINTPTR_MAX - sb || (dp < sp + sb && sp < dp + db))
        return;
    int p = clamp(progressQ16, 0, ONE);
    Surface surface = {
        dst, texture, width, height, tw, th, minimum(minimum(tw, th) / 2, 18 * tw / w),
        256, colors};
    if (p == ONE) {
        final_window(&surface, x, y, w, h);
        return;
    }
    int top_rise = out_cubic(minimum(ONE, p * 3));
    int left_reach = out_cubic(minimum(ONE, p * 17 / 10));
    int upper_width = smooth(minimum(ONE, p * 112 / 100));
    int bottom_width = smooth(clamp((int)((int64_t)(p - 18350) * ONE / (ONE - 18350)), 0, ONE));
    int bend_t = clamp((int)((int64_t)(p - 13107) * ONE / (ONE - 13107)), 0, ONE);
    int bow = mul(mul(4 * bend_t, ONE - bend_t), ONE - bend_t) * 17 / 10;
    Shape shape = {0};
    shape.left = lerp(anchor_x * ONE, x * ONE, left_reach);
    shape.top = lerp(anchor_y * ONE, y * ONE, top_rise);
    shape.right = shape.left + w * upper_width;
    shape.tipx = lerp(anchor_x * ONE, x * ONE, p);
    shape.tipy = lerp(anchor_y * ONE, (y + h) * ONE, p);
    shape.bottom = shape.tipx + w * bottom_width;
    shape.bow = bow;
    shape.w = w;
    shape.h = h;
    shape.tw = tw;
    shape.th = th;
    surface.opacity = (unsigned)(smooth(minimum(ONE, p * 8)) >> 8);
    Vertex current[GRID + 1], next[GRID + 1];
    for (int col = 0; col <= GRID; ++col)
        current[col] = point(&shape, col, 0);
    for (int row = 0; row < GRID; ++row) {
        for (int col = 0; col <= GRID; ++col)
            next[col] = point(&shape, col, row + 1);
        for (int col = 0; col < GRID; ++col) {
            triangle(&surface, current[col], current[col + 1], next[col + 1]);
            triangle(&surface, current[col], next[col + 1], next[col]);
        }
        for (int col = 0; col <= GRID; ++col)
            current[col] = next[col];
    }
}
void ribbon_draw(uint32_t *dst, int width, int height, const uint32_t *texture, int tw, int th,
                 int x, int y, int w, int h, int anchor_x, int anchor_y, int progressQ16) {
    ribbon_draw_cached(dst, width, height, texture, tw, th, x, y, w, h, anchor_x, anchor_y,
                       progressQ16, 0);
}
