#ifndef ARK_MOTION_H
#define ARK_MOTION_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MOTION_ONE 65536
#define MOTION_MAX_DIM 4096

typedef struct {
    int from, to, value;
    uint64_t start;
    unsigned duration;
    bool active;
} MotionTrack;
typedef struct {
    uint64_t origin, deadline;
    unsigned hz;
} MotionFrameClock;
/* Absolute phase avoids drift at 60/120/144/240 Hz. Advance skips late slots. */
void motion_frame_init(MotionFrameClock *, uint64_t now_ms, unsigned hz);
void motion_frame_advance(MotionFrameClock *, uint64_t now_ms);

/* Values and progress use signed Q16. Progress is clamped to [0, MOTION_ONE]. */
int motion_lerp(int a, int b, int progress);
int motion_ease(int progress); /* CSS cubic-bezier(.2, 0, .2, 1). */
/* update evaluates the timestamp before start retargets: no position jump.
 * Times earlier than start hold the initial value; duration zero completes now.
 * update returns whether the integer value changed, not whether it is active. */
void motion_start(MotionTrack *track, int target, uint64_t now, unsigned duration);
bool motion_update(MotionTrack *track, uint64_t now);
/* Exact floor((below*(255-alpha)+above*alpha)/255) on XRGB channels.
 * Buffers contain count pixels; output may alias either input. Alpha <=255. */
void motion_mix_pixels(uint32_t *out, const uint32_t *below, const uint32_t *above, size_t count,
                       unsigned alpha);

/* Packed, tightly-strided XRGB8888 surfaces; the high byte is ignored/cleared.
 * x/y/w/h are signed Q16 destination coordinates, radius is destination pixels.
 * Opacity is clamped to 0..255. Source alpha is not consumed: cached layers must
 * already be composited. Bilinear interpolation samples pixel centers; edges
 * receive fractional rectangle coverage and optional rounded AA coverage.
 * Both dimensions of each surface must be 1..MOTION_MAX_DIM. Buffers must each
 * contain width*height pixels; overlapping buffers are rejected. No allocation.
 * Radius is bounded to 2048 and half the destination rectangle's dimensions. */
void motion_blit(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh, int x_q16,
                 int y_q16, int w_q16, int h_q16, unsigned opacity, int corner_radius);
/* Optional 8x8 immutable texture metadata, shared with the curved renderer.
 * colors has ceil(sw/8)*ceil(sh/8) words; refresh it when the texture changes.
 * Uniform tiles preserve exact bilinear results and source XRGB semantics. */
void motion_texture_tiles(uint32_t *colors, const uint32_t *texture, int sw, int sh);
void motion_blit_cached(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh,
                        int x_q16, int y_q16, int w_q16, int h_q16, unsigned opacity,
                        int corner_radius, const uint32_t *colors);
#endif
