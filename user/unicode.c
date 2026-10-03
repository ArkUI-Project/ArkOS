#include "unicode.h"
#include "ark.h"
#include "unicode_font.h"

/* Only the immutable bundled, hash-pinned font is parsed. No user font input. */
static unsigned char outline_scratch[256 * 1024];
static size_t outline_used;
static void *outline_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (n > sizeof outline_scratch - outline_used)
        return 0;
    void *p = outline_scratch + outline_used;
    outline_used += n;
    return p;
}
static int outline_floor(double x) {
    int n = (int)x;
    return n - (x < n);
}
static int outline_ceil(double x) {
    int n = (int)x;
    return n + (x > n);
}
/* SDF functions are unused and eliminated from this translation unit. */
extern double pow(double, double), fmod(double, double), cos(double), acos(double);
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_ifloor outline_floor
#define STBTT_iceil outline_ceil
#define STBTT_sqrt __builtin_sqrt
#define STBTT_pow pow
#define STBTT_fmod fmod
#define STBTT_cos cos
#define STBTT_acos acos
#define STBTT_fabs __builtin_fabs
#define STBTT_malloc(n, u) ((void)(u), outline_alloc(n))
#define STBTT_free(p, u) ((void)(p), (void)(u))
#define STBTT_assert(x) ((void)sizeof(x))
#define STBTT_strlen strlen
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../third_party/stb/stb_truetype.h"
#pragma GCC diagnostic pop

/* The blob is read-only application data, with no runtime filesystem dependency.
 * Build from the project root, as the supplied Makefile does. */
__asm__(".pushsection .arkfont,\"a\",@progbits\n"
        ".balign 16\n"
        ".global ark_unicode_font_start\n"
        ".type ark_unicode_font_start,@object\n"
        "ark_unicode_font_start:\n"
        ".incbin \"assets/fonts/wqy-microhei.ttc\"\n"
        ".size ark_unicode_font_start, .-ark_unicode_font_start\n"
        ".popsection\n");
extern const uint8_t ark_unicode_font_start[];

int utf8_decode(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t scalar, minimum;
    int length;
    if (!s[0])
        return 0;
    if (s[0] < 0x80) {
        (*p)++;
        return s[0];
    }
    if (s[0] >= 0xc2 && s[0] <= 0xdf) {
        length = 2;
        scalar = s[0] & 0x1f;
        minimum = 0x80;
    } else if (s[0] >= 0xe0 && s[0] <= 0xef) {
        length = 3;
        scalar = s[0] & 0x0f;
        minimum = 0x800;
    } else if (s[0] >= 0xf0 && s[0] <= 0xf4) {
        length = 4;
        scalar = s[0] & 0x07;
        minimum = 0x10000;
    } else {
        (*p)++;
        return 0xfffd;
    }
    for (int i = 1; i < length; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            (*p)++;
            return 0xfffd;
        }
        scalar = (scalar << 6) | (s[i] & 0x3f);
    }
    if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) {
        (*p)++;
        return 0xfffd;
    }
    *p += length;
    return (int)scalar;
}

static stbtt_fontinfo outline_font;
static bool outline_ready;
static int outline_glyph(uint32_t cp) {
    if (!outline_ready) {
        int offset = stbtt_GetFontOffsetForIndex(ark_unicode_font_start, 1);
        if (offset < 0 || !stbtt_InitFont(&outline_font, ark_unicode_font_start, offset))
            return 0;
        outline_ready = true;
    }
    return stbtt_FindGlyphIndex(&outline_font, (int)cp);
}
int unicode_has_glyph(uint32_t cp) {
    return outline_glyph(cp) != 0;
}
static struct {
    uint32_t cp;
    int width;
} width_cache[512];
int unicode_codepoint_width(uint32_t cp) {
    if (cp < 0x20 || cp == 0x7f)
        return 0;
    unsigned slot = (cp * 33u) & 511;
    if (width_cache[slot].cp == cp)
        return width_cache[slot].width;
    int glyph = outline_glyph(cp), advance, bearing;
    if (!glyph) {
        width_cache[slot].cp = cp;
        width_cache[slot].width = UNICODE_CJK_WIDTH;
        return UNICODE_CJK_WIDTH;
    }
    stbtt_GetGlyphHMetrics(&outline_font, glyph, &advance, &bearing);
    float scale = stbtt_ScaleForMappingEmToPixels(&outline_font, 16);
    int width = advance * scale <= 12 ? UNICODE_ASCII_WIDTH : UNICODE_CJK_WIDTH;
    width_cache[slot].cp = cp;
    width_cache[slot].width = width;
    return width;
}
#define GLYPH_CACHE 256
#define GLYPH_SCALE_MAX 6
#define GLYPH_PIXELS (16 * 20 * GLYPH_SCALE_MAX * GLYPH_SCALE_MAX)
typedef struct {
    uint32_t cp;
    int scale, advance;
    unsigned char alpha[GLYPH_PIXELS];
} OutlineGlyph;
static OutlineGlyph outline_cache[GLYPH_CACHE];
static unsigned outline_replace[64];
static const OutlineGlyph *outline_render(uint32_t cp, int scale) {
    unsigned set = (cp * 33u + (unsigned)scale) % 64;
    for (unsigned i = 0; i < 4; i++) {
        OutlineGlyph *g = &outline_cache[set * 4 + i];
        if (g->cp == cp && g->scale == scale)
            return g;
    }
    OutlineGlyph *g = &outline_cache[set * 4 + outline_replace[set]++ % 4];
    g->cp = cp;
    g->scale = scale;
    g->advance = unicode_codepoint_width(cp);
    int width = g->advance * scale, height = 20 * scale, index = outline_glyph(cp);
    memset(g->alpha, 0, (size_t)width * height);
    if (!index) {
        for (int y = 3 * scale; y < 17 * scale; y++)
            for (int x = 2 * scale; x < 14 * scale; x++)
                if (x < 3 * scale || x >= 13 * scale || y < 4 * scale || y >= 16 * scale)
                    g->alpha[y * width + x] = 255;
        return g;
    }
    float sy = stbtt_ScaleForMappingEmToPixels(&outline_font, 16.0f * scale),
          sx = sy * (g->advance == 8 ? 0.8f : 1.0f);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&outline_font, index, sx, sy, &x0, &y0, &x1, &y1);
    int gw = x1 - x0, gh = y1 - y0;
    unsigned char bitmap[GLYPH_PIXELS];
    if (gw <= 0 || gh <= 0 || gw > 16 * GLYPH_SCALE_MAX || gh > 20 * GLYPH_SCALE_MAX)
        return g;
    outline_used = 0;
    stbtt_MakeGlyphBitmap(&outline_font, bitmap, gw, gh, gw, sx, sy, index);
    int baseline = 15 * scale;
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            int px = x + x0, py = y + y0 + baseline;
            if (px >= 0 && px < width && py >= 0 && py < height)
                g->alpha[py * width + px] = bitmap[y * gw + x];
        }
    return g;
}

static int tab_width(int width) {
    return UNICODE_ASCII_WIDTH * 4 - width % (UNICODE_ASCII_WIDTH * 4);
}

int utf8_width(const char *s) {
    int width = 0, maximum = 0, codepoint;
    while ((codepoint = utf8_decode(&s))) {
        if (codepoint == '\n') {
            if (width > maximum)
                maximum = width;
            width = 0;
        } else if (codepoint == '\t')
            width += tab_width(width);
        else
            width += unicode_codepoint_width((uint32_t)codepoint);
    }
    return width > maximum ? width : maximum;
}

size_t utf8_prev(const char *s, size_t byte_len) {
    const char *cursor = s;
    size_t previous = 0;
    while (*cursor && (size_t)(cursor - s) < byte_len) {
        previous = (size_t)(cursor - s);
        utf8_decode(&cursor);
    }
    return previous;
}

size_t utf8_fit(const char *s, int maxwidth) {
    const char *cursor = s;
    int width = 0;
    if (maxwidth < 0)
        return 0;
    while (*cursor) {
        const char *before = cursor;
        int codepoint = utf8_decode(&cursor);
        if (codepoint == '\n' || codepoint == '\r')
            return (size_t)(before - s);
        int advance =
            codepoint == '\t' ? tab_width(width) : unicode_codepoint_width((uint32_t)codepoint);
        if (advance > maxwidth - width)
            return (size_t)(before - s);
        width += advance;
    }
    return (size_t)(cursor - s);
}

static uint32_t blend(uint32_t dst, uint32_t rgb, unsigned alpha) {
    unsigned inverse = 255 - alpha;
    unsigned r = (((dst >> 16) & 255) * inverse + ((rgb >> 16) & 255) * alpha + 127) / 255;
    unsigned g = (((dst >> 8) & 255) * inverse + ((rgb >> 8) & 255) * alpha + 127) / 255;
    unsigned b = ((dst & 255) * inverse + (rgb & 255) * alpha + 127) / 255;
    return (dst & 0xff000000u) | (r << 16) | (g << 8) | b;
}

void unicode_draw(uint32_t *buf, int pitch, int width, int height, int x, int y, const char *s,
                  uint32_t rgb, int scale) {
    if (!buf || !s || pitch < width || width <= 0 || height <= 0 || scale < 1 ||
        scale > GLYPH_SCALE_MAX)
        return;
    int origin = x, cp;
    while ((cp = utf8_decode(&s))) {
        if (cp == '\n') {
            x = origin;
            y += UNICODE_LINE_HEIGHT * scale;
            continue;
        }
        if (cp == '\t') {
            x += tab_width((x - origin) / scale) * scale;
            continue;
        }
        if (cp < 0x20 || cp == 0x7f)
            continue;
        const OutlineGlyph *g = outline_render((uint32_t)cp, scale);
        int w = g->advance * scale, h = 20 * scale;
        int y0 = y < 0 ? -y : 0, y1 = y + h > height ? height - y : h, x0 = x < 0 ? -x : 0,
            x1 = x + w > width ? width - x : w;
        for (int row = y0; row < y1; row++)
            for (int col = x0; col < x1; col++) {
                unsigned a = g->alpha[row * w + col];
                if (!a)
                    continue;
                uint32_t *pixel = &buf[(size_t)(y + row) * pitch + x + col];
                *pixel =
                    a == 255 ? (*pixel & 0xff000000u) | (rgb & 0xffffffu) : blend(*pixel, rgb, a);
            }
        x += w;
    }
}
