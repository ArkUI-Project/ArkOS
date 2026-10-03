#ifndef ARK_CAPTURE_H
#define ARK_CAPTURE_H
#include <stdint.h>
#include <stddef.h>
size_t capture_bmp(uint8_t *out, size_t cap, const uint32_t *p, unsigned w, unsigned h);
size_t capture_gif_begin(uint8_t *out, size_t cap, unsigned w, unsigned h);
size_t capture_gif_frame(uint8_t *out, size_t cap, const uint32_t *p, unsigned sw, unsigned sh,
                         unsigned w, unsigned h, unsigned delay);
#endif
