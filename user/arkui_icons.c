#include "arkui_icons.h"
#include "raster.h"
#include <stdbool.h>
#include <stddef.h>

/* Original ArkUI artwork. Canonical vector geometry is rasterized once into
 * premultiplied ARGB; subsequent sizes use bilinear sampling. No image files,
 * icon fonts, floating point, malloc or platform APIs are involved. */
#define APP_SIZE 96
#define SYMBOL_SIZE 32
typedef struct {
    uint32_t *pixels;
    int width, height;
} Raster;
typedef struct {
    int x, y;
} Point;
static uint32_t app_pixels[ARKUI_APP_COUNT][APP_SIZE * APP_SIZE];
static uint8_t symbol_alpha[ARKUI_SYMBOL_COUNT][SYMBOL_SIZE * SYMBOL_SIZE];
static bool app_ready[ARKUI_APP_COUNT], symbol_ready[ARKUI_SYMBOL_COUNT];

static int lower(int a, int b) {
    return a < b ? a : b;
}
static int higher(int a, int b) {
    return a > b ? a : b;
}
static uint32_t rgb_mix(uint32_t a, uint32_t b, unsigned weight) {
    unsigned inv = 256 - weight;
    return (((((a >> 16) & 255) * inv + ((b >> 16) & 255) * weight) >> 8) << 16) |
           (((((a >> 8) & 255) * inv + ((b >> 8) & 255) * weight) >> 8) << 8) |
           (((a & 255) * inv + (b & 255) * weight) >> 8);
}
static void over(Raster *r, int x, int y, uint32_t rgb, unsigned alpha) {
    if (!alpha || x < 0 || y < 0 || x >= r->width || y >= r->height)
        return;
    uint32_t *pixel = &r->pixels[y * r->width + x], d = *pixel;
    unsigned inv = 255 - alpha;
    unsigned a = alpha + (((d >> 24) * inv + 127) / 255);
    unsigned red = (((rgb >> 16) & 255) * alpha + ((d >> 16) & 255) * inv + 127) / 255;
    unsigned green = (((rgb >> 8) & 255) * alpha + ((d >> 8) & 255) * inv + 127) / 255;
    unsigned blue = ((rgb & 255) * alpha + (d & 255) * inv + 127) / 255;
    *pixel = (a << 24) | (red << 16) | (green << 8) | blue;
}
static void rounded_gradient(Raster *r, int x, int y, int w, int h, int radius, uint32_t top,
                             uint32_t bottom, unsigned alpha) {
    for (int yy = higher(0, y); yy < lower(r->height, y + h); yy++) {
        unsigned weight = h > 1 ? (unsigned)(yy - y) * 256u / (unsigned)(h - 1) : 0;
        uint32_t color = rgb_mix(top, bottom, weight);
        for (int xx = higher(0, x); xx < lower(r->width, x + w); xx++) {
            unsigned coverage = raster_round_coverage(xx - x, yy - y, w, h, radius);
            over(r, xx, yy, color, coverage * alpha / 255);
        }
    }
}
static void box(Raster *r, int x, int y, int w, int h, int radius, uint32_t rgb, unsigned alpha) {
    rounded_gradient(r, x, y, w, h, radius, rgb, rgb, alpha);
}
static void outline(Raster *r, int x, int y, int w, int h, int radius, int thickness, uint32_t rgb,
                    unsigned alpha) {
    for (int yy = higher(0, y); yy < lower(r->height, y + h); yy++)
        for (int xx = higher(0, x); xx < lower(r->width, x + w); xx++) {
            int outer = (int)raster_round_coverage(xx - x, yy - y, w, h, radius);
            int inner = (int)raster_round_coverage(xx - x - thickness, yy - y - thickness,
                                                   w - 2 * thickness, h - 2 * thickness,
                                                   higher(0, radius - thickness));
            if (outer > inner)
                over(r, xx, yy, rgb, (unsigned)(outer - inner) * alpha / 255);
        }
}
static void disc(Raster *r, int x, int y, int radius, uint32_t rgb, unsigned alpha) {
    box(r, x - radius, y - radius, radius * 2, radius * 2, radius, rgb, alpha);
}
static void ring(Raster *r, int x, int y, int radius, int thickness, uint32_t rgb, unsigned alpha) {
    outline(r, x - radius, y - radius, radius * 2, radius * 2, radius, thickness, rgb, alpha);
}
static void segment(Raster *r, int ax, int ay, int bx, int by, int thickness, uint32_t rgb,
                    unsigned alpha) {
    int radius = thickness * 4, vx = (bx - ax) * 8, vy = (by - ay) * 8;
    int64_t length = (int64_t)vx * vx + (int64_t)vy * vy;
    int x0 = higher(0, lower(ax, bx) - thickness),
        x1 = lower(r->width, higher(ax, bx) + thickness + 1);
    int y0 = higher(0, lower(ay, by) - thickness),
        y1 = lower(r->height, higher(ay, by) + thickness + 1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            unsigned hits = 0;
            for (int sy = 1; sy < 8; sy += 2)
                for (int sx = 1; sx < 8; sx += 2) {
                    int px = x * 8 + sx - ax * 8, py = y * 8 + sy - ay * 8;
                    int64_t dot = (int64_t)px * vx + (int64_t)py * vy;
                    bool inside;
                    if (dot <= 0 || !length)
                        inside = (int64_t)px * px + (int64_t)py * py <= radius * radius;
                    else if (dot >= length) {
                        px -= vx;
                        py -= vy;
                        inside = (int64_t)px * px + (int64_t)py * py <= radius * radius;
                    } else {
                        int64_t cross = (int64_t)px * vy - (int64_t)py * vx;
                        inside = cross * cross <= (int64_t)radius * radius * length;
                    }
                    hits += inside;
                }
            if (hits)
                over(r, x, y, rgb, (hits * alpha + 8) / 16);
        }
}
static void polygon(Raster *r, const Point *points, int count, uint32_t rgb, unsigned alpha) {
    int x0 = r->width, y0 = r->height, x1 = 0, y1 = 0;
    for (int i = 0; i < count; i++) {
        x0 = lower(x0, points[i].x);
        y0 = lower(y0, points[i].y);
        x1 = higher(x1, points[i].x);
        y1 = higher(y1, points[i].y);
    }
    x0 = higher(0, x0 - 1);
    y0 = higher(0, y0 - 1);
    x1 = lower(r->width, x1 + 1);
    y1 = lower(r->height, y1 + 1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            unsigned hits = 0;
            for (int sy = 1; sy < 8; sy += 2)
                for (int sx = 1; sx < 8; sx += 2) {
                    int px = x * 8 + sx, py = y * 8 + sy;
                    bool inside = false;
                    for (int i = 0, j = count - 1; i < count; j = i++) {
                        int ax = points[i].x * 8, ay = points[i].y * 8, bx = points[j].x * 8,
                            by = points[j].y * 8;
                        if ((ay > py) != (by > py)) {
                            int64_t cross =
                                (int64_t)(px - ax) * (by - ay) - (int64_t)(bx - ax) * (py - ay);
                            if ((by > ay && cross < 0) || (by < ay && cross > 0))
                                inside = !inside;
                        }
                    }
                    hits += inside;
                }
            if (hits)
                over(r, x, y, rgb, (hits * alpha + 8) / 16);
        }
}
static void paper(Raster *r, uint32_t ink) {
    box(r, 23, 19, 52, 63, 8, 0x081a38, 35);
    rounded_gradient(r, 21, 16, 52, 63, 7, 0xffffff, 0xe7eef7, 255);
    Point fold[] = {{58, 16}, {73, 31}, {58, 31}};
    polygon(r, fold, 3, 0xc4d6ed, 255);
    segment(r, 31, 40, 61, 40, 3, ink, 110);
    segment(r, 31, 50, 61, 50, 3, ink, 110);
    segment(r, 31, 60, 51, 60, 3, ink, 110);
}
static void gear(Raster *r, int cx, int cy, int radius, uint32_t color, uint32_t hole) {
    static const int directions[8][2] = {{1024, 0},  {724, 724},   {0, 1024},  {-724, 724},
                                         {-1024, 0}, {-724, -724}, {0, -1024}, {724, -724}};
    for (int i = 0; i < 8; i++)
        segment(r, cx + directions[i][0] * (radius - 6) / 1024,
                cy + directions[i][1] * (radius - 6) / 1024, cx + directions[i][0] * radius / 1024,
                cy + directions[i][1] * radius / 1024, higher(3, radius / 3), color, 255);
    disc(r, cx, cy, radius - 5, color, 255);
    disc(r, cx, cy, radius / 2, hole, 255);
    ring(r, cx, cy, radius / 2 + 2, 2, 0xffffff, 140);
}
static void make_app(ArkUIAppIcon icon) {
    if (app_ready[icon])
        return;
    Raster r = {app_pixels[icon], APP_SIZE, APP_SIZE};
    static const uint32_t top[ARKUI_APP_COUNT] = {0x425571, 0x69d8ff, 0xffdb7a, 0xd4dde7, 0x54dbcf,
                                                  0xffc66c, 0x4ccff1, 0xf5e3cb, 0xc5a3ff, 0x657eed,
                                                  0x6ce2c1, 0xff9fc5, 0x86bcff, 0xbdabff, 0xffd18b,
                                                  0x9e90ff, 0xeac898, 0xf78c96, 0x8bc5fb, 0x91c4ef};
    static const uint32_t bottom[ARKUI_APP_COUNT] = {
        0x142134, 0x1784e4, 0xf4a935, 0x8395aa, 0x168f98, 0xe27727, 0x2775db,
        0xd4b48e, 0x8661d7, 0x354da8, 0x177773, 0xa63e82, 0x4360a9, 0x694bb5,
        0xd47a39, 0x5541c8, 0xaa7851, 0xcf475f, 0x3d7ebc, 0x3e6aaf};
    box(&r, 5, 8, 86, 85, 22, 0x04182f, 34);
    rounded_gradient(&r, 4, 3, 88, 88, 22, top[icon], bottom[icon], 255);
    outline(&r, 4, 3, 88, 88, 22, 1, 0xffffff, 98);
    if (icon == ARKUI_APP_TERMINAL) {
        rounded_gradient(&r, 16, 20, 64, 57, 10, 0x16293c, 0x0c1727, 255);
        outline(&r, 16, 20, 64, 57, 10, 1, 0xabc1d9, 110);
        disc(&r, 25, 28, 2, 0x7e94ab, 255);
        disc(&r, 32, 28, 2, 0x7e94ab, 255);
        disc(&r, 39, 28, 2, 0x7e94ab, 255);
        segment(&r, 29, 40, 42, 50, 5, 0xf1faff, 255);
        segment(&r, 42, 50, 29, 61, 5, 0xf1faff, 255);
        segment(&r, 49, 62, 66, 62, 4, 0x87e7d5, 255);
    } else if (icon == ARKUI_APP_FILES) {
        box(&r, 17, 25, 27, 17, 5, 0xd6f7ff, 255);
        rounded_gradient(&r, 16, 31, 65, 43, 6, 0xeafaff, 0xace9ff, 255);
        box(&r, 16, 42, 65, 34, 6, 0x0871ba, 25);
        rounded_gradient(&r, 15, 39, 67, 35, 6, 0xffffff, 0xd0f2ff, 255);
        segment(&r, 24, 45, 72, 45, 2, 0xffffff, 220);
        segment(&r, 27, 62, 44, 62, 3, 0x5badd9, 170);
    } else if (icon == ARKUI_APP_NOTES) {
        paper(&r, 0x7d96ad);
        rounded_gradient(&r, 21, 16, 52, 16, 6, 0xffeea7, 0xf9cb5a, 255);
        segment(&r, 55, 71, 77, 36, 7, 0x805b32, 45);
        segment(&r, 53, 69, 75, 34, 7, 0xf5a23d, 255);
        segment(&r, 56, 64, 73, 37, 2, 0xffdf87, 255);
        Point tip[] = {{49, 76}, {50, 66}, {57, 71}};
        polygon(&r, tip, 3, 0x47516a, 255);
    } else if (icon == ARKUI_APP_SETTINGS) {
        disc(&r, 49, 51, 31, 0x3d5068, 70);
        gear(&r, 48, 47, 30, 0xf3f6fa, 0x6e8299);
        ring(&r, 48, 47, 21, 2, 0xb7c9d9, 210);
    } else if (icon == ARKUI_APP_ABOUT) {
        segment(&r, 27, 71, 47, 24, 8, 0x0a626b, 45);
        segment(&r, 47, 24, 71, 71, 8, 0x0a626b, 45);
        segment(&r, 26, 68, 47, 23, 7, 0xf4ffff, 255);
        segment(&r, 47, 23, 70, 68, 7, 0xf4ffff, 255);
        segment(&r, 36, 52, 59, 52, 6, 0xb5fff0, 255);
        disc(&r, 72, 24, 4, 0xd1fff4, 230);
    } else if (icon == ARKUI_APP_CALCULATOR) {
        box(&r, 23, 18, 51, 66, 9, 0x704322, 55);
        rounded_gradient(&r, 22, 15, 51, 66, 9, 0x35445b, 0x1c293e, 255);
        rounded_gradient(&r, 28, 22, 39, 16, 3, 0xe4f3e4, 0xb6d6c4, 255);
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 3; x++)
                box(&r, 29 + x * 13, 45 + y * 10, 9, 7, 2, x == 2 ? 0xffb750 : 0xb8c8d7, 255);
        segment(&r, 58, 30, 62, 30, 2, 0x547569, 230);
    } else if (icon == ARKUI_APP_BROWSER) {
        disc(&r, 49, 51, 32, 0x0b4e9b, 50);
        rounded_gradient(&r, 16, 15, 64, 64, 32, 0xf4fdff, 0xd7f5ff, 255);
        ring(&r, 48, 47, 26, 2, 0x63b2dc, 255);
        segment(&r, 48, 23, 48, 28, 2, 0x68a8c8, 255);
        segment(&r, 48, 66, 48, 71, 2, 0x68a8c8, 255);
        segment(&r, 24, 47, 29, 47, 2, 0x68a8c8, 255);
        segment(&r, 67, 47, 72, 47, 2, 0x68a8c8, 255);
        Point north[] = {{58, 27}, {53, 52}, {43, 42}};
        polygon(&r, north, 3, 0xed675d, 255);
        Point south[] = {{38, 67}, {43, 42}, {53, 52}};
        polygon(&r, south, 3, 0x316cc1, 255);
        disc(&r, 48, 47, 3, 0xffffff, 255);
    } else if (icon == ARKUI_APP_CLOCK) {
        disc(&r, 49, 51, 32, 0x755736, 45);
        disc(&r, 48, 47, 32, 0xfffdf7, 255);
        ring(&r, 48, 47, 29, 1, 0xe5d8c5, 255);
        segment(&r, 48, 22, 48, 26, 2, 0x5e6770, 255);
        segment(&r, 48, 68, 48, 72, 2, 0x5e6770, 255);
        segment(&r, 23, 47, 27, 47, 2, 0x5e6770, 255);
        segment(&r, 69, 47, 73, 47, 2, 0x5e6770, 255);
        segment(&r, 48, 47, 34, 36, 4, 0x384657, 255);
        segment(&r, 48, 47, 61, 29, 3, 0x384657, 255);
        segment(&r, 48, 54, 48, 29, 2, 0xe78054, 255);
        disc(&r, 48, 47, 3, 0x384657, 255);
    } else if (icon == ARKUI_APP_TASKS) {
        rounded_gradient(&r, 15, 23, 66, 53, 9, 0x184b55, 0x103039, 255);
        segment(&r, 24, 64, 72, 64, 2, 0xa5decb, 170);
        box(&r, 26, 46, 9, 15, 2, 0x9befca, 255);
        box(&r, 43, 35, 9, 26, 2, 0x70e3c4, 255);
        box(&r, 60, 40, 9, 21, 2, 0x49b5cf, 255);
    } else if (icon == ARKUI_APP_PAINT) {
        box(&r, 19, 24, 56, 53, 24, 0x60479e, 45);
        rounded_gradient(&r, 17, 20, 59, 53, 25, 0xffffff, 0xeee4ff, 255);
        disc(&r, 35, 32, 5, 0xe88091, 255);
        disc(&r, 51, 30, 5, 0xe6bb53, 255);
        disc(&r, 64, 40, 5, 0x70b89f, 255);
        disc(&r, 34, 55, 5, 0x729bd3, 255);
        disc(&r, 52, 56, 7, 0xa98be4, 255);
        segment(&r, 57, 74, 78, 42, 6, 0x895cb9, 45);
        segment(&r, 54, 72, 76, 40, 5, 0x533c83, 255);
        segment(&r, 57, 68, 66, 55, 5, 0xf3cc83, 255);
        disc(&r, 77, 39, 5, 0xf3ebff, 255);
    } else if (icon == ARKUI_APP_CAPTURE) {
        box(&r, 18, 30, 60, 42, 9, 0xffffff, 255);
        box(&r, 30, 22, 23, 13, 4, 0xffffff, 255);
        ring(&r, 48, 51, 14, 4, 0xa63e82, 255);
        disc(&r, 48, 51, 8, 0xeda5ca, 255);
        disc(&r, 70, 38, 3, 0xee6581, 255);
    } else if (icon == ARKUI_APP_INSTALLER) {
        box(&r, 18, 44, 60, 29, 7, 0xe6f0ff, 255);
        segment(&r, 48, 21, 48, 52, 6, 0xffffff, 255);
        segment(&r, 37, 42, 48, 53, 5, 0xffffff, 255);
        segment(&r, 48, 53, 59, 42, 5, 0xffffff, 255);
        disc(&r, 67, 63, 3, 0x4360a9, 255);
    } else if (icon == ARKUI_APP_TODO) {
        paper(&r, 0x8665be);
        for (int y = 0; y < 3; y++) {
            box(&r, 27, 36 + y * 13, 9, 9, 2, 0x8e72bf, 255);
            segment(&r, 29, 40 + y * 13, 31, 42 + y * 13, 2, 0xffffff, 255);
            segment(&r, 31, 42 + y * 13, 35, 37 + y * 13, 2, 0xffffff, 255);
        }
    } else if (icon == ARKUI_APP_WASM) {
        segment(&r, 26, 30, 17, 48, 5, 0xffffff, 255);
        segment(&r, 17, 48, 26, 66, 5, 0xffffff, 255);
        segment(&r, 70, 30, 79, 48, 5, 0xffffff, 255);
        segment(&r, 79, 48, 70, 66, 5, 0xffffff, 255);
        segment(&r, 54, 30, 42, 66, 5, 0xffffff, 255);
    } else if (icon == ARKUI_APP_CALENDAR) {
        box(&r, 17, 19, 62, 58, 10, 0xffffff, 255);
        box(&r, 17, 19, 62, 17, 7, 0xe3465c, 255);
        segment(&r, 32, 14, 32, 27, 4, 0xffffff, 255);
        segment(&r, 65, 14, 65, 27, 4, 0xffffff, 255);
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 4; col++)
                disc(&r, 28 + col * 13, 45 + row * 10, 3,
                     row == 1 && col == 2 ? 0xe3465c : 0x7c8ea5, 255);
    } else if (icon == ARKUI_APP_REMINDERS) {
        paper(&r, 0x6a8dae);
        for (int i = 0; i < 3; i++) {
            ring(&r, 28, 36 + i * 15, 5, 2, i == 0 ? 0xefaf47 : i == 1 ? 0x83c68b : 0x72b3ef, 255);
            segment(&r, 41, 36 + i * 15, 68, 36 + i * 15, 3, 0x9baec1, 255);
        }
    } else if (icon == ARKUI_APP_PACKAGES || icon == ARKUI_APP_PACKAGE_APP) {
        Point lid[] = {{48, 22}, {77, 35}, {48, 49}, {19, 35}};
        polygon(&r, lid, 4, 0xffe2b5, 255);
        Point left[] = {{19, 38}, {45, 52}, {45, 78}, {19, 63}};
        polygon(&r, left, 4, 0xd29b63, 255);
        Point right[] = {{51, 52}, {77, 38}, {77, 63}, {51, 78}};
        polygon(&r, right, 4, 0xb17a4a, 255);
        segment(&r, 34, 29, 63, 42, 3, 0xffffff, 190);
        if (icon == ARKUI_APP_PACKAGES) {
            disc(&r, 70, 69, 13, 0x287dc2, 255);
            segment(&r, 64, 69, 76, 69, 3, 0xffffff, 255);
            segment(&r, 70, 63, 70, 75, 3, 0xffffff, 255);
        }
    } else if (icon == ARKUI_APP_TIMER) {
        ring(&r, 48, 52, 26, 5, 0xffffff, 255);
        segment(&r, 40, 16, 56, 16, 5, 0xffffff, 255);
        segment(&r, 48, 18, 48, 25, 4, 0xffffff, 255);
        segment(&r, 48, 52, 58, 39, 4, 0xffffff, 255);
        disc(&r, 48, 52, 4, 0xffffff, 255);
    } else {
        box(&r, 21, 18, 55, 64, 8, 0x152966, 45);
        rounded_gradient(&r, 20, 15, 55, 64, 7, 0xffffff, 0xe8edff, 255);
        Point fold[] = {{59, 15}, {75, 31}, {59, 31}};
        polygon(&r, fold, 3, 0xbdc9f4, 255);
        segment(&r, 29, 58, 29, 38, 4, 0x4561ac, 255);
        segment(&r, 29, 38, 39, 50, 4, 0x4561ac, 255);
        segment(&r, 39, 50, 49, 38, 4, 0x4561ac, 255);
        segment(&r, 49, 38, 49, 58, 4, 0x4561ac, 255);
        segment(&r, 62, 39, 62, 58, 4, 0x4561ac, 255);
        segment(&r, 57, 53, 62, 59, 3, 0x4561ac, 255);
        segment(&r, 62, 59, 67, 53, 3, 0x4561ac, 255);
        segment(&r, 29, 69, 66, 69, 2, 0xa6b6df, 255);
    }
    app_ready[icon] = true;
}

static void make_symbol(ArkUISymbol symbol) {
    if (symbol_ready[symbol])
        return;
    uint32_t pixels[SYMBOL_SIZE * SYMBOL_SIZE] = {0};
    Raster r = {pixels, SYMBOL_SIZE, SYMBOL_SIZE};
    const uint32_t c = 0xffffff;
#define L(a, b, d, e) segment(&r, a, b, d, e, 3, c, 255)
#define R(a, b, d, e, f) outline(&r, a, b, d, e, f, 2, c, 255)
    switch (symbol) {
    case ARKUI_SYMBOL_BACK:
        L(20, 7, 11, 16);
        L(11, 16, 20, 25);
        break;
    case ARKUI_SYMBOL_FORWARD:
        L(12, 7, 21, 16);
        L(21, 16, 12, 25);
        break;
    case ARKUI_SYMBOL_NEW_DOCUMENT:
        R(7, 5, 17, 23, 3);
        L(16, 11, 16, 22);
        L(11, 16, 21, 16);
        break;
    case ARKUI_SYMBOL_FOLDER:
        R(4, 10, 24, 17, 3);
        L(5, 10, 5, 6);
        L(5, 6, 13, 6);
        L(13, 6, 17, 10);
        break;
    case ARKUI_SYMBOL_SHARE:
        L(8, 15, 8, 27);
        L(8, 27, 25, 27);
        L(25, 27, 25, 15);
        L(16, 20, 16, 4);
        L(10, 10, 16, 4);
        L(16, 4, 22, 10);
        break;
    case ARKUI_SYMBOL_TRASH:
        R(9, 10, 15, 18, 2);
        L(6, 8, 27, 8);
        L(12, 5, 21, 5);
        L(14, 14, 14, 23);
        L(19, 14, 19, 23);
        break;
    case ARKUI_SYMBOL_RENAME:
        R(5, 17, 21, 11, 3);
        L(11, 20, 25, 6);
        L(22, 5, 27, 10);
        break;
    case ARKUI_SYMBOL_COPY:
        R(10, 10, 16, 18, 3);
        L(6, 22, 6, 5);
        L(6, 5, 20, 5);
        break;
    case ARKUI_SYMBOL_SEARCH:
        ring(&r, 14, 13, 9, 3, c, 255);
        L(21, 20, 28, 27);
        break;
    case ARKUI_SYMBOL_GRID:
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++)
                R(5 + x * 13, 5 + y * 13, 9, 9, 2);
        break;
    case ARKUI_SYMBOL_LIST:
        for (int y = 0; y < 3; y++) {
            disc(&r, 6, 8 + y * 8, 2, c, 255);
            L(12, 8 + y * 8, 27, 8 + y * 8);
        }
        break;
    case ARKUI_SYMBOL_LOCK:
        R(6, 14, 21, 15, 3);
        R(11, 4, 11, 17, 6);
        box(&r, 9, 15, 15, 9, 1, c, 255);
        disc(&r, 16, 20, 2, 0, 255);
        break;
    case ARKUI_SYMBOL_USER:
        ring(&r, 16, 10, 6, 3, c, 255);
        R(6, 20, 21, 10, 6);
        break;
    case ARKUI_SYMBOL_NETWORK:
        ring(&r, 16, 16, 12, 2, c, 255);
        R(11, 4, 10, 24, 5);
        L(5, 12, 27, 12);
        L(5, 20, 27, 20);
        break;
    case ARKUI_SYMBOL_MINIMIZE:
        L(7, 17, 25, 17);
        break;
    case ARKUI_SYMBOL_MAXIMIZE:
        R(6, 6, 21, 21, 3);
        break;
    case ARKUI_SYMBOL_CLOSE:
        L(8, 8, 24, 24);
        L(24, 8, 8, 24);
        break;
    case ARKUI_SYMBOL_DROPDOWN:
        L(8, 12, 16, 20);
        L(16, 20, 24, 12);
        break;
    case ARKUI_SYMBOL_REFRESH:
        ring(&r, 16, 17, 10, 3, c, 255);
        box(&r, 19, 2, 11, 10, 0, 0, 255);
        L(22, 5, 25, 12);
        L(25, 12, 18, 13);
        break;
    case ARKUI_SYMBOL_CHECK:
        L(6, 17, 13, 24);
        L(13, 24, 27, 8);
        break;
    case ARKUI_SYMBOL_MORE:
        for (int x = 0; x < 3; x++)
            disc(&r, 7 + x * 9, 16, 2, c, 255);
        break;
    case ARKUI_SYMBOL_HOME:
        L(4, 14, 16, 4);
        L(16, 4, 28, 14);
        L(8, 13, 8, 28);
        L(8, 28, 25, 28);
        L(25, 28, 25, 13);
        L(14, 28, 14, 19);
        L(14, 19, 19, 19);
        L(19, 19, 19, 28);
        break;
    case ARKUI_SYMBOL_POWER:
        ring(&r, 16, 18, 11, 3, c, 255);
        box(&r, 11, 3, 10, 12, 0, 0, 255);
        L(16, 4, 16, 17);
        break;
    case ARKUI_SYMBOL_SETTINGS:
        gear(&r, 16, 16, 13, c, 0);
        break;
    case ARKUI_SYMBOL_DOCUMENT:
        R(7, 4, 19, 25, 3);
        L(12, 11, 20, 11);
        L(12, 17, 21, 17);
        L(12, 23, 18, 23);
        break;
    case ARKUI_SYMBOL_POINTER: {
        Point p[] = {{7, 4}, {8, 27}, {14, 20}, {20, 29}, {24, 26}, {18, 18}, {28, 17}};
        polygon(&r, p, 7, c, 255);
        break;
    }
    case ARKUI_SYMBOL_COUNT:
        break;
    }
#undef L
#undef R
    /* For masks, black geometry is a knockout. This also produces clean
     * negative space inside locks, gears, power and refresh symbols. */
    for (int i = 0; i < SYMBOL_SIZE * SYMBOL_SIZE; i++)
        symbol_alpha[symbol][i] = (uint8_t)pixels[i];
    symbol_ready[symbol] = true;
}

ArkUISurface arkui_surface(uint32_t *pixels, int stride, int width, int height) {
    return (ArkUISurface){pixels, width, height, stride, {0, 0, width, height}};
}
static bool destination(ArkUISurface *s, int x, int y, int size, ArkUIRect *r) {
    if (!s || !s->pixels || s->width <= 0 || s->height <= 0 || s->width > 16384 ||
        s->height > 16384 || s->stride < s->width || s->stride > 65536 || size < 1 || size > 512 ||
        s->clip.w <= 0 || s->clip.h <= 0)
        return false;
    int64_t left = higher(0, higher(x, s->clip.x)), top = higher(0, higher(y, s->clip.y));
    int64_t right = (int64_t)x + size, bottom = (int64_t)y + size;
    if (right > s->width)
        right = s->width;
    if (bottom > s->height)
        bottom = s->height;
    if (right > (int64_t)s->clip.x + s->clip.w)
        right = (int64_t)s->clip.x + s->clip.w;
    if (bottom > (int64_t)s->clip.y + s->clip.h)
        bottom = (int64_t)s->clip.y + s->clip.h;
    if (right <= left || bottom <= top)
        return false;
    *r = (ArkUIRect){(int)left, (int)top, (int)(right - left), (int)(bottom - top)};
    return true;
}
static uint32_t rgba_mix(uint32_t a, uint32_t b, unsigned weight) {
    return rgb_mix(a, b, weight) |
           (((((a >> 24) * (256 - weight) + (b >> 24) * weight) >> 8)) << 24);
}
static void composite(ArkUISurface *s, int x, int y, uint32_t premult) {
    unsigned alpha = premult >> 24;
    if (!alpha)
        return;
    uint32_t *out = &s->pixels[(size_t)y * (size_t)s->stride + (size_t)x], d = *out;
    unsigned inv = 255 - alpha;
    unsigned red = ((premult >> 16) & 255) + (((d >> 16) & 255) * inv + 127) / 255;
    unsigned green = ((premult >> 8) & 255) + (((d >> 8) & 255) * inv + 127) / 255;
    unsigned blue = (premult & 255) + ((d & 255) * inv + 127) / 255;
    *out = (red << 16) | (green << 8) | blue;
}
static int sample_coordinate(int local, int size, int resolution, unsigned *fraction) {
    int q = ((local * 2 + 1) * resolution * 128) / size - 128;
    if (q <= 0) {
        *fraction = 0;
        return 0;
    }
    if (q >= (resolution - 1) * 256) {
        *fraction = 0;
        return resolution - 1;
    }
    *fraction = (unsigned)q & 255u;
    return q >> 8;
}
void arkui_app_icon(ArkUISurface *s, int x, int y, int size, ArkUIAppIcon icon) {
    ArkUIRect region;
    if ((unsigned)icon >= ARKUI_APP_COUNT || !destination(s, x, y, size, &region))
        return;
    make_app(icon);
    const uint32_t *src = app_pixels[icon];
    for (int yy = region.y; yy < region.y + region.h; yy++) {
        unsigned fy;
        int sy = sample_coordinate(yy - y, size, APP_SIZE, &fy), sy1 = lower(APP_SIZE - 1, sy + 1);
        for (int xx = region.x; xx < region.x + region.w; xx++) {
            unsigned fx;
            int sx = sample_coordinate(xx - x, size, APP_SIZE, &fx),
                sx1 = lower(APP_SIZE - 1, sx + 1);
            uint32_t a = rgba_mix(src[sy * APP_SIZE + sx], src[sy * APP_SIZE + sx1], fx);
            uint32_t b = rgba_mix(src[sy1 * APP_SIZE + sx], src[sy1 * APP_SIZE + sx1], fx);
            composite(s, xx, yy, rgba_mix(a, b, fy));
        }
    }
}
void arkui_symbol(ArkUISurface *s, int x, int y, int size, ArkUISymbol symbol, uint32_t rgb) {
    ArkUIRect region;
    if ((unsigned)symbol >= ARKUI_SYMBOL_COUNT || !destination(s, x, y, size, &region))
        return;
    make_symbol(symbol);
    const uint8_t *src = symbol_alpha[symbol];
    for (int yy = region.y; yy < region.y + region.h; yy++) {
        unsigned fy;
        int sy = sample_coordinate(yy - y, size, SYMBOL_SIZE, &fy),
            sy1 = lower(SYMBOL_SIZE - 1, sy + 1);
        for (int xx = region.x; xx < region.x + region.w; xx++) {
            unsigned fx;
            int sx = sample_coordinate(xx - x, size, SYMBOL_SIZE, &fx),
                sx1 = lower(SYMBOL_SIZE - 1, sx + 1);
            unsigned a =
                (src[sy * SYMBOL_SIZE + sx] * (256 - fx) + src[sy * SYMBOL_SIZE + sx1] * fx) >> 8;
            unsigned b =
                (src[sy1 * SYMBOL_SIZE + sx] * (256 - fx) + src[sy1 * SYMBOL_SIZE + sx1] * fx) >> 8;
            unsigned alpha = (a * (256 - fy) + b * fy) >> 8;
            uint32_t premult = (alpha << 24) | ((((rgb >> 16) & 255) * alpha / 255) << 16) |
                               ((((rgb >> 8) & 255) * alpha / 255) << 8) |
                               ((rgb & 255) * alpha / 255);
            composite(s, xx, yy, premult);
        }
    }
}
const char *arkui_symbol_name(ArkUISymbol symbol) {
    static const char *names[] = {
        "back",     "forward",  "new document", "folder",   "share",   "trash", "rename",
        "copy",     "search",   "grid",         "list",     "lock",    "user",  "network",
        "minimize", "maximize", "close",        "dropdown", "refresh", "check", "more",
        "home",     "power",    "settings",     "document", "pointer"};
    return (unsigned)symbol < ARKUI_SYMBOL_COUNT ? names[symbol] : "unknown";
}
