#ifndef ARK_RASTER_H
#define ARK_RASTER_H
#include <stdint.h>
/* Continuous cubic superellipse corners, 8x8 subpixel coverage. Local pixels,
 * dimensions <=4096; joins have zero curvature at the straight edges. */
unsigned raster_round_coverage(int x, int y, int w, int h, int radius);
void raster_blend_span(uint32_t *pixels, int count, uint32_t color, unsigned alpha);
unsigned raster_cursor_coverage(int x, int y, int scale, int inner);
unsigned raster_cursor_shape_coverage(int x, int y, int scale, int inner, unsigned shape);
#endif
