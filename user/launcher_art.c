#include "launcher_art.h"

/* Original procedural atmosphere: broad blue limb over a quiet navy sky.
 * Distances use squared ellipse coordinates, so there are no square roots,
 * floating point operations, random grain, image decoders or runtime assets. */
#define COLOR_COUNT 8192
#define PLANET_RADIUS_SQUARED 4096
static uint32_t radial_colors[COLOR_COUNT];
static int column_distance[1920];
static unsigned column_light[1920];
static int cached_width;
static bool colors_ready, cached_night;

static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}
/* Fixed-point smoothstep with zero slope at each end, range 0..256. */
static int smooth(int value) {
    value = clamp(value, 0, 256);
    return (value * value * (768 - 2 * value) + 32768) >> 16;
}
static void make_colors(bool night) {
    if (colors_ready && cached_night == night)
        return;
    for (int i = 0; i < COLOR_COUNT; i++) {
        int difference = i - PLANET_RADIUS_SQUARED;
        int broad;
        if (difference < 0)
            broad = smooth((2800 + difference) * 256 / 2800);
        else {
            broad = smooth((1850 - difference) * 256 / 1850);
            broad = (broad * broad + 128) >> 8;
        }
        int haze = difference < 0 ? smooth((3900 + difference) * 256 / 3900)
                                  : smooth((3000 - difference) * 256 / 3000);
        int rim_distance = difference - 60;
        if (rim_distance < 0)
            rim_distance = -rim_distance;
        int rim = smooth((460 - rim_distance) * 256 / 460);
        int r = 4 + (11 * broad + 2 * haze + 5 * rim) / 256;
        int g = 8 + (36 * broad + 8 * haze + 13 * rim) / 256;
        int b = 22 + (133 * broad + 23 * haze + 30 * rim) / 256;
        if (night) {
            r = r * 224 / 256;
            g = g * 224 / 256;
            b = b * 232 / 256;
        }
        radial_colors[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    cached_night = night;
    colors_ready = true;
}

static void make_columns(int width) {
    if (cached_width == width)
        return;
    int center = width * 51 / 100;
    int radius = width * 54 / 100;
    if (radius < 1)
        radius = 1;
    for (int x = 0; x < width; x++) {
        int normalized = (x - center) * 1024 / radius;
        column_distance[x] = normalized * normalized;
        int lateral = x - width * 54 / 100;
        if (lateral < 0)
            lateral = -lateral;
        /* Subtle directional light, without artificial streaks or texture. */
        column_light[x] = (unsigned)clamp(256 - lateral * 56 / width, 208, 256);
    }
    cached_width = width;
}

void launcher_art(uint32_t *out, int width, int height, bool night) {
    if (!out || width < 1 || height < 1 || width > 1920 || height > 1200)
        return;
    make_colors(night);
    make_columns(width);
    int center = height * 113 / 100;
    int radius = height * 74 / 100;
    if (radius < 1)
        radius = 1;
    for (int y = 0; y < height; y++) {
        int normalized = (y - center) * 1024 / radius;
        int row_distance = normalized * normalized;
        /* The open sky remains dark enough for launcher labels and icons. */
        unsigned sky_shade = (unsigned)(y * 7 / height);
        uint32_t *row = out + (unsigned)y * (unsigned)width;
        for (int x = 0; x < width; x++) {
            int index = (column_distance[x] + row_distance) >> 8;
            if (index >= COLOR_COUNT)
                index = COLOR_COUNT - 1;
            uint32_t c = radial_colors[index];
            unsigned light = column_light[x];
            unsigned r = ((c >> 16) * light) >> 8;
            unsigned g = (((c >> 8) & 255u) * light) >> 8;
            unsigned b = ((c & 255u) * light) >> 8;
            /* Less light in the upper sky, a little blue depth near the base. */
            b += sky_shade;
            row[x] = (r << 16) | (g << 8) | b;
        }
    }
}
