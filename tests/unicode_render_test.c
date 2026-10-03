#include "unicode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t pixels[196 * 64], large[196 * 64];
int main(void) {
    assert(utf8_width("ArkOS 中国") == 80 && unicode_has_glyph(0x4e2d) &&
           unicode_has_glyph(0x56fd));
    for (unsigned i = 0; i < 196 * 64; i++)
        pixels[i] = large[i] = 0x12765432;
    unicode_draw(pixels, 196, 192, 64, 12, 4, "ArkOS 中国", 0xffffff, 1);
    unsigned shades[256] = {0}, count = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 196; x++) {
            uint32_t p = pixels[y * 196 + x];
            if (x >= 192)
                assert(p == 0x12765432);
            if (p != 0x12765432) {
                assert((p & 0xff000000) == 0x12000000);
                shades[p & 255] = 1;
            }
        }
    for (unsigned i = 0; i < 256; i++)
        count += shades[i];
    assert(count > 30);
    unicode_draw(large, 196, 192, 64, 12, 4, "ArkOS 中国", 0xffffff, 2);
    unsigned interpolated = 0;
    for (int y = 0; y < 40; y += 2)
        for (int x = 0; x < 160; x += 2)
            if (large[(y + 4) * 196 + x + 12] != large[(y + 4) * 196 + x + 13] ||
                large[(y + 4) * 196 + x + 12] != large[(y + 5) * 196 + x + 12])
                interpolated++;
    assert(interpolated > 100);
    unicode_draw(pixels, 196, 192, 64, -8, -8, "中文", 0xffffff, 2);
    for (int y = 0; y < 64; y++)
        for (int x = 192; x < 196; x++)
            assert(pixels[y * 196 + x] == 0x12765432);
    printf("PASS Unicode: unchanged metrics, %u coverage shades, outline rasterized enlarged "
           "glyphs and clipped stride\n",
           count);
}
