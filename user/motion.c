/* Native fixed-point motion and cached-layer scaling. Original code; MIT. */
#include "motion.h"
#include "raster.h"
#include <stddef.h>
#include <limits.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

void motion_frame_init(MotionFrameClock *clock, uint64_t now, unsigned hz) {
    *clock = (MotionFrameClock){now, now, hz < 1 ? 1 : hz > 240 ? 240 : hz};
}
void motion_frame_advance(MotionFrameClock *clock, uint64_t now) {
    if (now < clock->origin)
        clock->origin = now;
    unsigned phase = (unsigned)((now - clock->origin) % 1000) * clock->hz % 1000;
    clock->deadline = now + (1000 - phase + clock->hz - 1) / clock->hz;
}

static int progress_clamp(int value) {
    return value < 0 ? 0 : value > MOTION_ONE ? MOTION_ONE : value;
}

int motion_lerp(int a, int b, int progress) {
    int p = progress_clamp(progress);
    if (!p)
        return a;
    if (p == MOTION_ONE)
        return b;
    /* The largest signed-int difference times 65536 fits in int64_t. */
    return (int)((int64_t)a + ((int64_t)b - a) * p / MOTION_ONE);
}

/* x(t) = 3/5*t*(1-t) + t^3. One common denominator keeps its quantized
 * evaluation monotonic; rounding the separate terms can introduce reversals. */
static unsigned bezier_x(unsigned t) {
    uint64_t a = t, u = MOTION_ONE - t;
    uint64_t numerator = 3 * a * u * MOTION_ONE + 5 * a * a * a;
    return (unsigned)(numerator / (5ULL * MOTION_ONE * MOTION_ONE));
}

int motion_ease(int progress) {
    unsigned p = (unsigned)progress_clamp(progress);
    if (!p || p == MOTION_ONE)
        return (int)p;
    unsigned low = 0, high = MOTION_ONE;
    while (high - low > 1) {
        unsigned middle = low + (high - low) / 2;
        if (bezier_x(middle) < p)
            low = middle;
        else
            high = middle;
    }
    uint64_t t = high;
    /* y(t) = 3*t^2 - 2*t^3; no overshoot or floating point. */
    return (int)(t * t * (3 * MOTION_ONE - 2 * t) / ((uint64_t)MOTION_ONE * MOTION_ONE));
}

bool motion_update(MotionTrack *track, uint64_t now) {
    if (!track || !track->active)
        return false;
    int previous = track->value;
    if (!track->duration) {
        track->value = track->to;
        track->active = false;
    } else if (now < track->start) {
        track->value = track->from;
    } else {
        uint64_t elapsed = now - track->start;
        if (elapsed >= track->duration) {
            track->value = track->to;
            track->active = false;
        } else {
            /* elapsed < duration <= UINT_MAX, so this cannot overflow. */
            int progress = (int)(elapsed * MOTION_ONE / track->duration);
            track->value = motion_lerp(track->from, track->to, motion_ease(progress));
        }
    }
    return track->value != previous;
}

void motion_start(MotionTrack *track, int target, uint64_t now, unsigned duration) {
    if (!track)
        return;
    motion_update(track, now);
    track->from = track->value;
    track->to = target;
    track->start = now;
    track->duration = duration;
    track->active = duration != 0 && track->value != target;
    if (!track->active)
        track->value = target;
}

static int floor_q16(int64_t value) {
    int64_t quotient = value / MOTION_ONE;
    if (value < 0 && value % MOTION_ONE)
        --quotient;
    return (int)quotient;
}

static int ceil_q16(int64_t value) {
    int64_t quotient = value / MOTION_ONE;
    if (value > 0 && value % MOTION_ONE)
        ++quotient;
    return (int)quotient;
}

/* 256-based interpolation of red/blue lanes and green, using only 32-bit
 * arithmetic. The lane products together remain below UINT32_MAX. */
static inline uint32_t mix_rgb(uint32_t a, uint32_t b, unsigned fraction) {
    unsigned inverse = 256 - fraction;
    uint32_t rb = (((a & 0x00ff00ffu) * inverse + (b & 0x00ff00ffu) * fraction) >> 8) & 0x00ff00ffu;
    uint32_t g = (((a & 0x0000ff00u) * inverse + (b & 0x0000ff00u) * fraction) >> 8) & 0x0000ff00u;
    return rb | g;
}

static inline uint32_t opacity_rgb(uint32_t below, uint32_t above, unsigned alpha) {
    /* Map exact endpoint 255 to weight 256 without per-channel division. */
    unsigned weight = alpha + (alpha >> 7);
    return mix_rgb(below, above, weight);
}

#if defined(__SSE2__)
/* Four XRGB pixels, with 16-bit channel products. Every sum is <=65280;
 * pack/shift reproduces the scalar rounding and ignores the high byte. */
static inline __m128i mix_four(__m128i a, __m128i b, __m128i lo, __m128i hi) {
    __m128i zero = _mm_setzero_si128();
    __m128i al = _mm_unpacklo_epi8(a, zero), ah = _mm_unpackhi_epi8(a, zero);
    /* a*256 + (b-a)*weight is identical modulo 65536. The final sum is
     * nonnegative and <=65280, so one multiply per lane preserves rounding. */
    __m128i low = _mm_add_epi16(_mm_slli_epi16(al, 8),
                                _mm_mullo_epi16(_mm_sub_epi16(_mm_unpacklo_epi8(b, zero), al), lo));
    __m128i high = _mm_add_epi16(
        _mm_slli_epi16(ah, 8), _mm_mullo_epi16(_mm_sub_epi16(_mm_unpackhi_epi8(b, zero), ah), hi));
    return _mm_and_si128(_mm_packus_epi16(_mm_srli_epi16(low, 8), _mm_srli_epi16(high, 8)),
                         _mm_set1_epi32(0xffffff));
}
#endif

static void mix_span(uint32_t *out, const uint32_t *top, const uint32_t *bottom, int count,
                     unsigned fraction, unsigned opacity) {
    int x = 0;
#if defined(__SSE2__)
    __m128i vertical = _mm_set1_epi16((short)fraction);
    __m128i alpha = _mm_set1_epi16((short)(opacity + (opacity >> 7)));
    for (; x + 4 <= count; x += 4) {
        __m128i value = _mm_loadu_si128((const __m128i *)(top + x));
        if (fraction) {
            __m128i low = _mm_loadu_si128((const __m128i *)(bottom + x));
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(value, low)) != 65535)
                value = mix_four(value, low, vertical, vertical);
        }
        if (opacity != 255)
            value = mix_four(_mm_loadu_si128((const __m128i *)(out + x)), value, alpha, alpha);
        _mm_storeu_si128((__m128i *)(out + x), value);
    }
#endif
    for (; x < count; x++) {
        uint32_t value = fraction ? mix_rgb(top[x], bottom[x], fraction) : top[x];
        out[x] = opacity == 255 ? value : opacity_rgb(out[x], value, opacity);
    }
}

void motion_mix_pixels(uint32_t *out, const uint32_t *below, const uint32_t *above, size_t count,
                       unsigned alpha) {
    size_t x = 0;
#if defined(__SSE2__)
    __m128i weight = _mm_set1_epi16((short)alpha), inverse = _mm_set1_epi16((short)(255 - alpha));
    __m128i zero = _mm_setzero_si128(), one = _mm_set1_epi16(1);
    for (; x + 4 <= count; x += 4) {
        __m128i a = _mm_loadu_si128((const __m128i *)(below + x)),
                b = _mm_loadu_si128((const __m128i *)(above + x));
        __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(a, zero), inverse),
                                   _mm_mullo_epi16(_mm_unpacklo_epi8(b, zero), weight));
        __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(a, zero), inverse),
                                   _mm_mullo_epi16(_mm_unpackhi_epi8(b, zero), weight));
        lo = _mm_add_epi16(lo, one);
        hi = _mm_add_epi16(hi, one);
        lo = _mm_srli_epi16(_mm_add_epi16(lo, _mm_srli_epi16(lo, 8)), 8);
        hi = _mm_srli_epi16(_mm_add_epi16(hi, _mm_srli_epi16(hi, 8)), 8);
        _mm_storeu_si128((__m128i *)(out + x),
                         _mm_and_si128(_mm_packus_epi16(lo, hi), _mm_set1_epi32(0xffffff)));
    }
#endif
    for (; x < count; x++) {
        uint32_t a = below[x], b = above[x];
        out[x] = ((((a >> 16) & 255) * (255 - alpha) + ((b >> 16) & 255) * alpha) / 255 << 16) |
                 ((((a >> 8) & 255) * (255 - alpha) + ((b >> 8) & 255) * alpha) / 255 << 8) |
                 (((a & 255) * (255 - alpha) + (b & 255) * alpha) / 255);
    }
}

typedef struct {
    int64_t coordinate, step;
    uint32_t remainder, step_remainder, denominator;
} Sampler;

static Sampler sampler(int first_pixel, int origin_q16, int extent_q16, int source_size) {
    Sampler result;
    int64_t numerator = ((int64_t)first_pixel * MOTION_ONE + MOTION_ONE / 2 - origin_q16) *
                        source_size * MOTION_ONE;
    int64_t quotient = numerator / extent_q16;
    int64_t remainder = numerator % extent_q16;
    if (remainder < 0) {
        --quotient;
        remainder += extent_q16;
    }
    uint64_t advance = (uint64_t)source_size * MOTION_ONE * MOTION_ONE;
    result.coordinate = quotient - MOTION_ONE / 2;
    result.remainder = (uint32_t)remainder;
    result.step = (int64_t)(advance / (unsigned)extent_q16);
    result.step_remainder = (uint32_t)(advance % (unsigned)extent_q16);
    result.denominator = (unsigned)extent_q16;
    return result;
}

static inline void sampler_next(Sampler *s) {
    s->coordinate += s->step;
    /* Each remainder is < INT_MAX, hence their sum fits uint32_t. */
    s->remainder += s->step_remainder;
    if (s->remainder >= s->denominator) {
        s->remainder -= s->denominator;
        ++s->coordinate;
    }
}

static inline int sample_index(int64_t coordinate, int dimension, unsigned *fraction) {
    if (coordinate <= 0) {
        *fraction = 0;
        return 0;
    }
    if (coordinate >= (int64_t)(dimension - 1) * MOTION_ONE) {
        *fraction = 0;
        return dimension - 1;
    }
    *fraction = (unsigned)((uint64_t)coordinate >> 8) & 255;
    return (int)(coordinate >> 16);
}

static unsigned edge_coverage(int pixel, int64_t begin, int64_t end) {
    int64_t lo = (int64_t)pixel * MOTION_ONE;
    int64_t hi = lo + MOTION_ONE;
    if (lo < begin)
        lo = begin;
    if (hi > end)
        hi = end;
    return hi <= lo ? 0 : (unsigned)(hi - lo);
}

static bool valid_surface(const void *buffer, int w, int h) {
    return buffer && w > 0 && h > 0 && w <= MOTION_MAX_DIM && h <= MOTION_MAX_DIM;
}

/* The public contract ignores source XRGB's high byte. A zero-high-byte span
 * can use the native rep-movsq memcpy; arbitrary ARGB input remains identical. */
static void copy_xrgb(uint32_t *out, const uint32_t *source, int count) {
    uint32_t bits = 0;
    int i = 0;
    for (; i + 4 <= count; i += 4)
        bits |= source[i] | source[i + 1] | source[i + 2] | source[i + 3];
    for (; i < count; i++)
        bits |= source[i];
    if (!(bits & 0xff000000u)) {
        memcpy(out, source, (size_t)count * sizeof(*out));
        return;
    }
    i = 0;
    for (; i + 4 <= count; i += 4) {
        out[i] = source[i] & 0xffffffu;
        out[i + 1] = source[i + 1] & 0xffffffu;
        out[i + 2] = source[i + 2] & 0xffffffu;
        out[i + 3] = source[i + 3] & 0xffffffu;
    }
    for (; i < count; i++)
        out[i] = source[i] & 0xffffffu;
}
static void opaque_native(uint32_t *dst, int dw, const uint32_t *src, int sw, int sh, int ox,
                          int oy, int x0, int y0, int x1, int y1, int radius) {
    for (int y = y0; y < y1; y++) {
        uint32_t *out = dst + (size_t)y * dw;
        const uint32_t *source = src + (size_t)(y - oy) * sw;
        int first = x0, last = x1;
        if (radius && (y - oy < radius || y - oy >= sh - radius)) {
            first = ox + radius;
            if (first < x0)
                first = x0;
            if (first > x1)
                first = x1;
            last = ox + sw - radius;
            if (last < x0)
                last = x0;
            if (last > x1)
                last = x1;
        }
        for (int x = x0; x < first; x++) {
            unsigned alpha = raster_round_coverage(x - ox, y - oy, sw, sh, radius);
            uint32_t color = source[x - ox] & 0xffffffu;
            if (alpha)
                out[x] = alpha == 255 ? color : opacity_rgb(out[x], color, alpha);
        }
        copy_xrgb(out + first, source + first - ox, last - first);
        for (int x = last; x < x1; x++) {
            unsigned alpha = raster_round_coverage(x - ox, y - oy, sw, sh, radius);
            uint32_t color = source[x - ox] & 0xffffffu;
            if (alpha)
                out[x] = alpha == 255 ? color : opacity_rgb(out[x], color, alpha);
        }
    }
}
static void filter_horizontal(uint32_t *out, const uint32_t *source, const uint32_t *columns,
                              const uint64_t *weights, const uint8_t *patterns,
                              const uint32_t *tiles, int count) {
    int x = 0;
#if defined(__SSE2__)
    for (; x + 4 <= count; x += 4) {
        unsigned a = columns[x], b = columns[x + 1], c = columns[x + 2], d = columns[x + 3];
        unsigned ia = a & 65535u, ib = b & 65535u, ic = c & 65535u, id = d & 65535u;
        if (tiles && (ia >> 3) == ((id + (d >> 16 != 0)) >> 3)) {
            uint32_t color = tiles[ia >> 3];
            if (color < 0x01000000u) {
                _mm_storeu_si128((__m128i *)(out + x), _mm_set1_epi32((int)color));
                continue;
            }
        }
        __m128i left, right;
        /* Enlargement visits adjacent source columns. A precomputed shuffle
         * replaces eight scalar gathers with two bounded vector loads. */
        if (patterns[x / 4]) {
            left = _mm_loadu_si128((const __m128i *)(source + ia));
            right = _mm_loadu_si128((const __m128i *)(source + ia + 1));
            switch (patterns[x / 4]) {
            case 0x90:
                left = _mm_shuffle_epi32(left, 0x90);
                right = _mm_shuffle_epi32(right, 0x90);
                break;
            case 0x94:
                left = _mm_shuffle_epi32(left, 0x94);
                right = _mm_shuffle_epi32(right, 0x94);
                break;
            case 0xa4:
                left = _mm_shuffle_epi32(left, 0xa4);
                right = _mm_shuffle_epi32(right, 0xa4);
                break;
            }
        } else {
            left =
                _mm_set_epi32((int)source[id], (int)source[ic], (int)source[ib], (int)source[ia]);
            right =
                _mm_set_epi32((int)source[id + (d >> 16 != 0)], (int)source[ic + (c >> 16 != 0)],
                              (int)source[ib + (b >> 16 != 0)], (int)source[ia + (a >> 16 != 0)]);
        }
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(left, right)) == 65535) {
            _mm_storeu_si128((__m128i *)(out + x), _mm_and_si128(left, _mm_set1_epi32(0xffffff)));
            continue;
        }
        __m128i lo = _mm_loadu_si128((const __m128i *)(weights + x));
        __m128i hi = _mm_loadu_si128((const __m128i *)(weights + x + 2));
        _mm_storeu_si128((__m128i *)(out + x), mix_four(left, right, lo, hi));
    }
#endif
    (void)weights;
    (void)patterns;
    (void)tiles;
    for (; x < count; x++) {
        unsigned entry = columns[x], sx = entry & 65535u, fx = entry >> 16;
        uint32_t color = source[sx] & 0xffffffu;
        /* A clamped last-source column always has fraction zero. */
        if (fx)
            color = mix_rgb(color, source[sx + 1], fx);
        out[x] = color;
    }
}
/* Fully covered spans need no fractional-edge or corner work. Horizontal
 * sampling is invariant across rows; each distinct
 * source row is filtered at most once while vertically enlarged. The two-row
 * cache preserves the original horizontal-then-vertical rounding exactly.
 * Scratch uses at most 81 KiB of the 128 KiB user stack. Plans are immutable
 * during row filtering and the scalar path uses the same quantized weights. */
static void scaled_interior(uint32_t *dst, int dw, const uint32_t *src, int sw, int sh, int x0,
                            int y0, int x1, int y1, Sampler horizontal, Sampler vertical,
                            unsigned opacity, const uint32_t *tiles) {
    uint32_t columns[MOTION_MAX_DIM], filtered[2][MOTION_MAX_DIM];
    uint64_t weights[MOTION_MAX_DIM];
    uint8_t patterns[MOTION_MAX_DIM / 4];
    int rows[2] = {-1, -1}, count = x1 - x0;
    for (int x = 0; x < count; x++) {
        unsigned fx;
        int sx = sample_index(horizontal.coordinate, sw, &fx);
        columns[x] = (unsigned)sx | (fx << 16);
        weights[x] = (uint64_t)fx * 0x0001000100010001ull;
        sampler_next(&horizontal);
    }
    for (int x = 0; x + 4 <= count; x += 4) {
        unsigned a = columns[x] & 65535u, b = columns[x + 1] & 65535u, c = columns[x + 2] & 65535u,
                 d = columns[x + 3] & 65535u;
        unsigned pattern = 0;
        if (sw >= 5 && a <= (unsigned)sw - 5) {
            if (b == a + 1 && c == a + 2 && d == a + 3)
                pattern = 0xe4;
            else if (b == a && c == a + 1 && d == a + 2)
                pattern = 0x90;
            else if (b == a + 1 && c == a + 1 && d == a + 2)
                pattern = 0x94;
            else if (b == a + 1 && c == a + 2 && d == a + 2)
                pattern = 0xa4;
        }
        patterns[x / 4] = (uint8_t)pattern;
    }
    for (int y = y0; y < y1; y++) {
        unsigned fy;
        int sy = sample_index(vertical.coordinate, sh, &fy);
        unsigned top_slot = rows[0] == sy ? 0 : rows[1] == sy ? 1 : 0;
        if (rows[top_slot] != sy) {
            filter_horizontal(filtered[top_slot], src + (size_t)sy * sw, columns, weights, patterns,
                              tiles ? tiles + (size_t)(sy / 8) * ((sw + 7) / 8) : 0, count);
            rows[top_slot] = sy;
        }
        uint32_t *out = dst + (size_t)y * dw + x0;
        const uint32_t *top = filtered[top_slot];
        if (!fy) {
            if (opacity == 255)
                memcpy(out, top, (size_t)count * sizeof(*out));
            else
                mix_span(out, top, top, count, 0, opacity);
        } else {
            unsigned bottom_slot = top_slot ^ 1u;
            if (rows[bottom_slot] != sy + 1) {
                filter_horizontal(
                    filtered[bottom_slot], src + (size_t)(sy + 1) * sw, columns, weights, patterns,
                    tiles ? tiles + (size_t)((sy + 1) / 8) * ((sw + 7) / 8) : 0, count);
                rows[bottom_slot] = sy + 1;
            }
            const uint32_t *bottom = filtered[bottom_slot];
            mix_span(out, top, bottom, count, fy, opacity);
        }
        sampler_next(&vertical);
    }
}

void motion_texture_tiles(uint32_t *colors, const uint32_t *texture, int sw, int sh) {
    if (!colors || !valid_surface(texture, sw, sh))
        return;
    int stride = (sw + 7) / 8;
    for (int y = 0; y < sh; y += 8)
        for (int x = 0; x < sw; x += 8) {
            uint32_t color = texture[(size_t)y * sw + x] & 0xffffffu;
            int end_y = y + 8 < sh ? y + 8 : sh, end_x = x + 8 < sw ? x + 8 : sw;
            for (int yy = y; yy < end_y && color < 0x01000000u; yy++)
                for (int xx = x; xx < end_x; xx++)
                    if ((texture[(size_t)yy * sw + xx] & 0xffffffu) != color) {
                        color = 0x01000000u;
                        break;
                    }
            colors[(size_t)(y / 8) * stride + x / 8] = color;
        }
}
void motion_blit(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh, int x, int y,
                 int w, int h, unsigned opacity, int radius) {
    motion_blit_cached(dst, dw, dh, src, sw, sh, x, y, w, h, opacity, radius, 0);
}
void motion_blit_cached(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh,
                        int x_q16, int y_q16, int w_q16, int h_q16, unsigned opacity,
                        int corner_radius, const uint32_t *colors) {
    if (!valid_surface(dst, dw, dh) || !valid_surface(src, sw, sh) || w_q16 <= 0 || h_q16 <= 0 ||
        !opacity)
        return;
    if (opacity > 255)
        opacity = 255;
    size_t dst_bytes = (size_t)dw * (unsigned)dh * sizeof(*dst);
    size_t src_bytes = (size_t)sw * (unsigned)sh * sizeof(*src);
    uintptr_t da = (uintptr_t)dst, sa = (uintptr_t)src;
    if (da > UINTPTR_MAX - dst_bytes || sa > UINTPTR_MAX - src_bytes ||
        (da < sa + src_bytes && sa < da + dst_bytes))
        return;

    int64_t right_q16 = (int64_t)x_q16 + w_q16;
    int64_t bottom_q16 = (int64_t)y_q16 + h_q16;
    int origin_x = floor_q16(x_q16), origin_y = floor_q16(y_q16);
    int finish_x = ceil_q16(right_q16), finish_y = ceil_q16(bottom_q16);
    int x0 = origin_x < 0 ? 0 : origin_x;
    int y0 = origin_y < 0 ? 0 : origin_y;
    int x1 = finish_x > dw ? dw : finish_x;
    int y1 = finish_y > dh ? dh : finish_y;
    if (x0 >= x1 || y0 >= y1)
        return;

    int rectangle_w = finish_x - origin_x, rectangle_h = finish_y - origin_y;
    int radius = corner_radius < 0 ? 0 : corner_radius;
    if (radius > rectangle_w / 2)
        radius = rectangle_w / 2;
    if (radius > rectangle_h / 2)
        radius = rectangle_h / 2;
    /* Keep corner work within the rasterizer's documented dimensions. */
    if (radius > MOTION_MAX_DIM / 2)
        radius = MOTION_MAX_DIM / 2;

    /* Exact-size native app surfaces need neither bilinear sampling nor DDA. */
    if (opacity == 255 && w_q16 == sw * MOTION_ONE && h_q16 == sh * MOTION_ONE &&
        x_q16 % MOTION_ONE == 0 && y_q16 % MOTION_ONE == 0) {
        opaque_native(dst, dw, src, sw, sh, origin_x, origin_y, x0, y0, x1, y1, radius);
        return;
    }
    if (!radius && w_q16 == sw * MOTION_ONE && h_q16 == sh * MOTION_ONE &&
        x_q16 % MOTION_ONE == 0 && y_q16 % MOTION_ONE == 0) {
        for (int y = y0; y < y1; y++)
            mix_span(dst + (size_t)y * dw + x0, src + (size_t)(y - origin_y) * sw + x0 - origin_x,
                     src + (size_t)(y - origin_y) * sw + x0 - origin_x, x1 - x0, 0, opacity);
        return;
    }
    if (!radius && (int64_t)x0 * MOTION_ONE >= x_q16 && (int64_t)y0 * MOTION_ONE >= y_q16 &&
        (int64_t)x1 * MOTION_ONE <= right_q16 && (int64_t)y1 * MOTION_ONE <= bottom_q16) {
        scaled_interior(dst, dw, src, sw, sh, x0, y0, x1, y1, sampler(x0, x_q16, w_q16, sw),
                        sampler(y0, y_q16, h_q16, sh), opacity, colors);
        return;
    }

    unsigned first_coverage = edge_coverage(x0, x_q16, right_q16);
    unsigned last_coverage = edge_coverage(x1 - 1, x_q16, right_q16);
    /* Rounded windows have the same SIMD interior as unrounded layers. Split
     * it into three bands; only fractional edges and corner squares use the
     * scalar coverage path below. Sampling/rounding stays identical. */
    int ix0 = ceil_q16(x_q16), ix1 = floor_q16(right_q16);
    int iy0 = ceil_q16(y_q16), iy1 = floor_q16(bottom_q16);
    if (ix0 < x0)
        ix0 = x0;
    if (ix1 > x1)
        ix1 = x1;
    if (iy0 < y0)
        iy0 = y0;
    if (iy1 > y1)
        iy1 = y1;
    if (ix0 < ix1 && iy0 < iy1) {
        int top = origin_y + radius, bottom = finish_y - radius;
        if (top < iy0)
            top = iy0;
        if (top > iy1)
            top = iy1;
        if (bottom < iy0)
            bottom = iy0;
        if (bottom > iy1)
            bottom = iy1;
        int cuts[4] = {iy0, top, bottom, iy1};
        for (int band = 0; band < 3; band++) {
            int left = ix0, right = ix1;
            if (band != 1) {
                if (left < origin_x + radius)
                    left = origin_x + radius;
                if (right > finish_x - radius)
                    right = finish_x - radius;
            }
            if (left < right && cuts[band] < cuts[band + 1])
                scaled_interior(dst, dw, src, sw, sh, left, cuts[band], right, cuts[band + 1],
                                sampler(left, x_q16, w_q16, sw),
                                sampler(cuts[band], y_q16, h_q16, sh), opacity, colors);
        }
    }
    Sampler initial_x = sampler(x0, x_q16, w_q16, sw);
    Sampler vertical = sampler(y0, y_q16, h_q16, sh);
    for (int y = y0; y < y1; ++y) {
        unsigned fy;
        int sy = sample_index(vertical.coordinate, sh, &fy);
        int sy_next = sy + (sy + 1 < sh);
        const uint32_t *top = src + (size_t)sy * (unsigned)sw;
        const uint32_t *bottom = src + (size_t)sy_next * (unsigned)sw;
        uint32_t *out = dst + (size_t)y * (unsigned)dw;
        unsigned row_alpha = (opacity * edge_coverage(y, y_q16, bottom_q16) + 32768u) >> 16;
        int local_y = y - origin_y;
        bool corner_row = radius && (local_y < radius || local_y >= rectangle_h - radius);
        int body_left = ix0, body_right = ix1;
        if (corner_row) {
            if (body_left < origin_x + radius)
                body_left = origin_x + radius;
            if (body_right > finish_x - radius)
                body_right = finish_x - radius;
        }
        Sampler horizontal = initial_x;
        for (int x = x0; x < x1; ++x) {
            if (y >= iy0 && y < iy1 && body_left < body_right && x == body_left) {
                x = body_right;
                if (x >= x1)
                    break;
                horizontal = sampler(x, x_q16, w_q16, sw);
            }
            unsigned alpha = row_alpha;
            if (x == x0)
                alpha = (alpha * first_coverage + 32768u) >> 16;
            else if (x == x1 - 1)
                alpha = (alpha * last_coverage + 32768u) >> 16;
            int local_x = x - origin_x;
            if (corner_row && (local_x < radius || local_x >= rectangle_w - radius)) {
                unsigned coverage =
                    raster_round_coverage(local_x, local_y, rectangle_w, rectangle_h, radius);
                alpha = (alpha * coverage + 127u) / 255u;
            }
            if (alpha) {
                unsigned fx;
                int sx = sample_index(horizontal.coordinate, sw, &fx);
                int sx_next = sx + (sx + 1 < sw);
                uint32_t color = top[sx] & 0x00ffffffu;
                if (fx)
                    color = mix_rgb(color, top[sx_next], fx);
                if (fy) {
                    uint32_t low = bottom[sx] & 0x00ffffffu;
                    if (fx)
                        low = mix_rgb(low, bottom[sx_next], fx);
                    color = mix_rgb(color, low, fy);
                }
                out[x] = alpha == 255 ? color : opacity_rgb(out[x], color, alpha);
            }
            sampler_next(&horizontal);
        }
        sampler_next(&vertical);
    }
}
