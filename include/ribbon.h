#ifndef ARK_RIBBON_H
#define ARK_RIBBON_H
#include <stdint.h>
/* Curved, textured sheet unfolding from a dock anchor. Integer geometry only.
 * All positions/sizes are pixels; progressQ16 is 0..65536 (clamped).
 * 0 leaves the destination untouched; 65536 draws the complete rounded window.
 * Packed XRGB8888 surfaces; source alpha is ignored. Source/destination must not
 * overlap and must each contain their stated number of pixels. Dimensions and
 * destination rectangle sizes are 1..4096; x/y/anchors are -16384..16384.
 * The caller owns time/easing (typically ~230ms opening / ~330ms closing), the
 * background, and snapshot lifetime. No allocation, libm, or hidden state. */
void ribbon_draw(uint32_t *dst, int width, int height, const uint32_t *texture, int tw, int th,
                 int x, int y, int w, int h, int anchor_x, int anchor_y, int progressQ16);
/* Optional immutable 8x8 texture metadata for repeated native snapshots.
 * colors contains ceil(tw/8)*ceil(th/8) words. Each tile records an exact
 * uniform XRGB color or 0x01000000 for a nonuniform tile; prepare again whenever
 * source pixels change. Sampling/geometry/lighting remain pixel-identical. */
void ribbon_draw_cached(uint32_t *dst, int width, int height, const uint32_t *texture, int tw,
                        int th, int x, int y, int w, int h, int anchor_x, int anchor_y,
                        int progressQ16, const uint32_t *colors);
#endif
