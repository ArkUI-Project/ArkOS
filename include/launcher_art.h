#ifndef ARK_LAUNCHER_ART_H
#define ARK_LAUNCHER_ART_H
#include <stdbool.h>
#include <stdint.h>

/* Fill a contiguous width*height 0x00RRGGBB image. No text or app artwork.
 * Intended for a cached launcher backdrop: call on size/theme changes, then
 * composite that buffer during animation. The radial color table is cached.
 * Invalid pointers and dimensions (outside 1..1920 by 1..1200) are ignored. */
void launcher_art(uint32_t *out, int width, int height, bool night);
#endif
