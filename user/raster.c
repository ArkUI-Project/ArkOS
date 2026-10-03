#include "raster.h"
#include "cursor.h"
unsigned raster_cursor_shape_coverage(int x, int y, int scale, int inner, unsigned shape) {
    if (shape == ARK_CURSOR_ARROW)
        return raster_cursor_coverage(x, y, scale, inner);
    if (scale < 1 || scale > 2 || x < 0 || y < 0 || x >= 32 * scale || y >= 32 * scale)
        return 0;
    unsigned hits = 0;
    for (int sy = 1; sy < 8; sy += 2)
        for (int sx = 1; sx < 8; sx += 2)
            hits += (unsigned)ark_cursor_inside(shape, (x * 8 + sx) / scale, (y * 8 + sy) / scale,
                                                inner);
    return (hits * 255 + 8) / 16;
}

unsigned raster_round_coverage(int x, int y, int w, int h, int r) {
    if (x < 0 || y < 0 || x >= w || y >= h || w <= 0 || h <= 0)
        return 0;
    if (r < 0)
        r = 0;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (!r || ((x >= r && x < w - r) || (y >= r && y < h - r)))
        return 255;
    /* Reflect all four corners to one quadrant. |x|^3+|y|^3=r^3 makes
     * curvature meet the straight spans continuously, unlike a circle. */
    int ax = x < r ? r - x - 1 : x - (w - r), ay = y < r ? r - y - 1 : y - (h - r);
    uint64_t dx = (unsigned)ax * 16u, dy = (unsigned)ay * 16u, rr = (unsigned)r * 16u;
    uint64_t limit = rr * rr * rr, ex = dx + 15, ey = dy + 15;
    if (ex * ex * ex + ey * ey * ey <= limit)
        return 255;
    if (dx * dx * dx + dy * dy * dy > limit)
        return 0;
    unsigned hits = 0;
    for (int sy = 1; sy < 16; sy += 2)
        for (int sx = 1; sx < 16; sx += 2) {
            ex = dx + (unsigned)sx;
            ey = dy + (unsigned)sy;
            hits += ex * ex * ex + ey * ey * ey <= limit;
        }
    return (hits * 255 + 32) / 64;
}

unsigned raster_cursor_coverage(int x, int y, int scale, int inner) {
    static const int outer[][2] = {{1, 1},   {1, 24},  {6, 19}, {10, 28},
                                   {14, 26}, {10, 17}, {18, 17}};
    static const int inset[][2] = {{2, 3}, {2, 21}, {6, 17}, {10, 25}, {12, 24}, {8, 16}, {15, 16}};
    if (scale < 1 || scale > 2)
        return 0;
    const int (*p)[2] = inner ? inset : outer;
    unsigned hits = 0;
    for (int sy = 1; sy < 8; sy += 2)
        for (int sx = 1; sx < 8; sx += 2) {
            int px = x * 8 + sx, py = y * 8 + sy;
            int inside = 0;
            for (int i = 0, j = 6; i < 7; j = i++) {
                int ax = p[i][0] * 8 * scale, ay = p[i][1] * 8 * scale;
                int bx = p[j][0] * 8 * scale, by = p[j][1] * 8 * scale;
                if ((ay > py) != (by > py)) {
                    int d = (px - ax) * (by - ay) - (bx - ax) * (py - ay);
                    if ((by > ay && d < 0) || (by < ay && d > 0))
                        inside = !inside;
                }
            }
            hits += (unsigned)inside;
        }
    return (hits * 255 + 8) / 16;
}

/* Four XRGB pixels per SSE2 operation. Integer division by 255 retains the
 * exact scalar floor for every channel (0..65025), including alpha endpoints. */
void raster_blend_span(uint32_t *pixels, int count, uint32_t color, unsigned alpha) {
    if (!alpha)
        return;
    color &= 0x00ffffffu;
    int i = 0;
#if defined(__SSE2__)
    typedef uint8_t Bytes __attribute__((vector_size(16)));
    typedef uint16_t Words __attribute__((vector_size(16)));
    typedef uint32_t Dwords __attribute__((vector_size(16)));
    Dwords rgb = {color, color, color, color};
    Words inverse = {255 - alpha, 255 - alpha, 255 - alpha, 255 - alpha,
                     255 - alpha, 255 - alpha, 255 - alpha, 255 - alpha};
    Words a = {alpha, alpha, alpha, alpha, alpha, alpha, alpha, alpha},
          one = {1, 1, 1, 1, 1, 1, 1, 1};
    Bytes zero = {0};
    Words low = (Words)__builtin_shufflevector((Bytes)rgb, zero, 0, 16, 1, 16, 2, 16, 3, 16, 4, 16,
                                               5, 16, 6, 16, 7, 16) *
                a;
    Words high = (Words)__builtin_shufflevector((Bytes)rgb, zero, 8, 16, 9, 16, 10, 16, 11, 16, 12,
                                                16, 13, 16, 14, 16, 15, 16) *
                 a;
    for (; i + 4 <= count; i += 4) {
        Bytes b;
        __builtin_memcpy(&b, pixels + i, 16);
        Words lo = (Words)__builtin_shufflevector(b, zero, 0, 16, 1, 16, 2, 16, 3, 16, 4, 16, 5, 16,
                                                  6, 16, 7, 16) *
                       inverse +
                   low;
        Words hi = (Words)__builtin_shufflevector(b, zero, 8, 16, 9, 16, 10, 16, 11, 16, 12, 16, 13,
                                                  16, 14, 16, 15, 16) *
                       inverse +
                   high;
        lo = (lo + one + (lo >> 8)) >> 8;
        hi = (hi + one + (hi >> 8)) >> 8;
        Bytes out = __builtin_shufflevector((Bytes)lo, (Bytes)hi, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18,
                                            20, 22, 24, 26, 28, 30);
        Dwords masked = (Dwords)out & (Dwords){0xffffffu, 0xffffffu, 0xffffffu, 0xffffffu};
        __builtin_memcpy(pixels + i, &masked, 16);
    }
#endif
    unsigned inv = 255 - alpha;
    for (; i < count; i++) {
        uint32_t c = pixels[i];
        pixels[i] = ((((c >> 16) & 255) * inv + ((color >> 16) & 255) * alpha) / 255 << 16) |
                    ((((c >> 8) & 255) * inv + ((color >> 8) & 255) * alpha) / 255 << 8) |
                    (((c & 255) * inv + (color & 255) * alpha) / 255);
    }
}
