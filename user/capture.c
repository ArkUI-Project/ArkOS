/* Native encoders. GIF uses bounded 9-bit literal LZW batches (clear every
 * 200 pixels); legal GIF89a with no dictionary allocation or host encoder. */
#include "capture.h"
static void le(uint8_t *p, uint32_t x, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        p[i] = (uint8_t)(x >> (8 * i));
}
size_t capture_bmp(uint8_t *out, size_t cap, const uint32_t *p, unsigned w, unsigned h) {
    if (!out || !p || !w || !h || w > 1920 || h > 1200)
        return 0;
    unsigned row = (w * 3 + 3) & ~3u;
    size_t n = 54 + (size_t)row * h;
    if (n > cap)
        return 0;
    for (unsigned i = 0; i < 54; i++)
        out[i] = 0;
    out[0] = 'B';
    out[1] = 'M';
    le(out + 2, (uint32_t)n, 4);
    le(out + 10, 54, 4);
    le(out + 14, 40, 4);
    le(out + 18, w, 4);
    le(out + 22, h, 4);
    out[26] = 1;
    out[28] = 24;
    le(out + 34, row * h, 4);
    for (unsigned y = 0; y < h; y++) {
        uint8_t *d = out + 54 + y * row;
        for (unsigned x = 0; x < w; x++) {
            uint32_t c = p[(h - 1 - y) * w + x];
            d[x * 3] = (uint8_t)c;
            d[x * 3 + 1] = (uint8_t)(c >> 8);
            d[x * 3 + 2] = (uint8_t)(c >> 16);
        }
        for (unsigned x = w * 3; x < row; x++)
            d[x] = 0;
    }
    return n;
}
size_t capture_gif_begin(uint8_t *out, size_t cap, unsigned w, unsigned h) {
    if (cap < 800 || !w || !h || w > 1024 || h > 768)
        return 0;
    const char *s = "GIF89a";
    for (unsigned i = 0; i < 6; i++)
        out[i] = (uint8_t)s[i];
    le(out + 6, w, 2);
    le(out + 8, h, 2);
    out[10] = 0xf7;
    out[11] = out[12] = 0;
    for (unsigned i = 0; i < 256; i++) {
        out[13 + i * 3] = (uint8_t)(((i >> 5) & 7) * 255 / 7);
        out[14 + i * 3] = (uint8_t)(((i >> 2) & 7) * 255 / 7);
        out[15 + i * 3] = (uint8_t)((i & 3) * 255 / 3);
    }
    uint8_t loop[] = {0x21, 0xff, 11,  'N', 'E', 'T', 'S', 'C', 'A', 'P',
                      'E',  '2',  '.', '0', 3,   1,   0,   0,   0};
    for (unsigned i = 0; i < sizeof loop; i++)
        out[781 + i] = loop[i];
    return 781 + sizeof loop;
}
size_t capture_gif_frame(uint8_t *out, size_t cap, const uint32_t *p, unsigned sw, unsigned sh,
                         unsigned w, unsigned h, unsigned delay) {
    if (!p || !w || !h || w > 1024 || h > 768 || !sw || !sh || sw > 1920 || sh > 1200 ||
        cap < (size_t)w * h * 2 + 1024)
        return 0;
    uint8_t head[] = {0x21, 0xf9, 4, 4, 0, 0, 0, 0, 0x2c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8};
    for (unsigned i = 0; i < sizeof head; i++)
        out[i] = head[i];
    le(out + 4, delay ? delay : 1, 2);
    le(out + 13, w, 2);
    le(out + 15, h, 2);
    size_t at = 19, block = at++;
    unsigned used = 0, bits = 0, acc = 0, run = 0, total = w * h;
    for (unsigned i = 0; i <= total; i++) {
        unsigned codes[2], n = 0;
        if (!run && i < total)
            codes[n++] = 256;
        if (i == total)
            codes[n++] = 257;
        else {
            uint32_t c = p[(i / w * sh / h) * sw + (i % w * sw / w)];
            codes[n++] = ((c >> 16) & 0xe0) | ((c >> 11) & 0x1c) | ((c >> 6) & 3);
            run = (run + 1) % 200;
        }
        for (unsigned j = 0; j < n; j++) {
            acc |= codes[j] << bits;
            bits += 9;
            while (bits >= 8) {
                out[at++] = (uint8_t)acc;
                acc >>= 8;
                bits -= 8;
                if (++used == 255) {
                    out[block] = 255;
                    block = at++;
                    used = 0;
                }
            }
        }
    }
    if (bits) {
        out[at++] = (uint8_t)acc;
        used++;
    }
    out[block] = (uint8_t)used;
    if (used)
        out[at++] = 0;
    return at;
}
