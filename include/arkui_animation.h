#ifndef ARKUI_ANIMATION_H
#define ARKUI_ANIMATION_H
#include "arkui_types.h"
#include "motion.h"

typedef enum {
    ARKUI_TRANSITION_SHEET,
    ARKUI_TRANSITION_LAUNCHER,
    ARKUI_TRANSITION_HOVER,
    ARKUI_TRANSITION_ISLAND
} ArkUITransition;
typedef struct {
    MotionTrack track;
    ArkUITransition transition;
    bool presented;
} ArkUIAnimation;

/* Timestamps and durations in this public API are MILLISECONDS.
 * Use ark_millis() for fine-grained animation timing on ArkOS. */
void arkui_animation_init(ArkUIAnimation *, ArkUITransition, bool presented);
/* Binding-style state transition. Calling with an unchanged target is a no-op.
 * Reversal samples current position first and scales time by remaining travel. */
void arkui_animation_set(ArkUIAnimation *, bool presented, uint64_t now_ms, bool reduced_motion);
bool arkui_animation_update(ArkUIAnimation *, uint64_t now_ms);
bool arkui_animation_active(const ArkUIAnimation *);
int arkui_animation_progress(const ArkUIAnimation *); /* 0..65536 */
unsigned arkui_animation_duration(ArkUITransition, bool presenting);
void arkui_animation_finish(ArkUIAnimation *);
/* Conservative pixel bounds of the affine transitions, including AA edges.
 * Sheet bounds include its target and anchor; empty progress has empty bounds. */
ArkUIRect arkui_transition_bounds(const ArkUIAnimation *, ArkUIRect target, int anchor_x,
                                  int anchor_y);
/* Draw cached content through the selected ArkUI transition. The caller owns
 * backdrop/caching. Source and destination are tightly packed XRGB8888 and may
 * not overlap; target and anchor are pixels. Sheet uses the Dock anchor;
 * launcher uses the supplied zoom center; hover ignores the anchor.
 * Pass a smaller dst_height when a
 * launcher must leave a persistent Dock outside its animated content region. */
void arkui_transition_draw(const ArkUIAnimation *, uint32_t *dst, int dst_width, int dst_height,
                           const uint32_t *src, int src_width, int src_height, ArkUIRect target,
                           int anchor_x, int anchor_y);
/* Same transition with metadata from motion_texture_tiles(). Keep both the
 * texture and its metadata immutable until every rendering worker completes. */
void arkui_transition_draw_cached(const ArkUIAnimation *, uint32_t *dst, int dst_width,
                                  int dst_height, const uint32_t *src, int src_width,
                                  int src_height, ArkUIRect target, int anchor_x, int anchor_y,
                                  const uint32_t *colors);
#endif
