#ifndef ARK_UNICODE_H
#define ARK_UNICODE_H

#include <stddef.h>
#include <stdint.h>

/* All positions and widths are pixels at scale 1. Pitch is uint32_t pixels. */
#define UNICODE_FONT_HEIGHT 20
#define UNICODE_LINE_HEIGHT 22
#define UNICODE_ASCII_WIDTH 8
#define UNICODE_CJK_WIDTH 16

/* Decode one scalar and advance. Invalid input consumes one byte as U+FFFD.
 * A terminating NUL returns 0 without advancing. */
int utf8_decode(const char **p);
/* Maximum line width; tabs are four ASCII cells and CR is ignored. */
int utf8_width(const char *s);
/* Byte offset of the character before byte_len (usually strlen(s)). */
size_t utf8_prev(const char *s, size_t byte_len);
/* Bytes fitting the first line, always ending on a complete scalar. */
size_t utf8_fit(const char *s, int maxwidth);
int unicode_codepoint_width(uint32_t codepoint);
int unicode_has_glyph(uint32_t codepoint);
void unicode_draw(uint32_t *buf, int pitch, int width, int height, int x, int y, const char *s,
                  uint32_t rgb, int scale);

#endif
