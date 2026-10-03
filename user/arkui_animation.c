#include "arkui_animation.h"
#include "ribbon.h"
#include <limits.h>

static int progress(int value) {
    return value < 0 ? 0 : value > MOTION_ONE ? MOTION_ONE : value;
}
unsigned arkui_animation_duration(ArkUITransition transition, bool presenting) {
    switch (transition) {
    case ARKUI_TRANSITION_SHEET:
        return presenting ? 230 : 330;
    case ARKUI_TRANSITION_LAUNCHER:
        return presenting ? 280 : 230;
    case ARKUI_TRANSITION_HOVER:
        return presenting ? 120 : 100;
    case ARKUI_TRANSITION_ISLAND:
        return presenting ? 300 : 240;
    }
    return 0;
}
void arkui_animation_init(ArkUIAnimation *animation, ArkUITransition transition, bool presented) {
    if (!animation)
        return;
    *animation = (ArkUIAnimation){.transition = transition,
                                  .presented = presented,
                                  .track = {.from = presented ? MOTION_ONE : 0,
                                            .to = presented ? MOTION_ONE : 0,
                                            .value = presented ? MOTION_ONE : 0}};
}
void arkui_animation_set(ArkUIAnimation *animation, bool presented, uint64_t now_ms,
                         bool reduced_motion) {
    if (!animation)
        return;
    motion_update(&animation->track, now_ms);
    int target = presented ? MOTION_ONE : 0;
    animation->presented = presented;
    if (reduced_motion) {
        motion_start(&animation->track, target, now_ms, 0);
        return;
    }
    if (animation->track.to == target &&
        (animation->track.active || animation->track.value == target))
        return;
    int distance = target - progress(animation->track.value);
    if (distance < 0)
        distance = -distance;
    unsigned full = arkui_animation_duration(animation->transition, presented);
    unsigned duration =
        (unsigned)(((uint64_t)full * (unsigned)distance + MOTION_ONE - 1) / MOTION_ONE);
    motion_start(&animation->track, target, now_ms, duration);
}
bool arkui_animation_update(ArkUIAnimation *animation, uint64_t now_ms) {
    return animation && motion_update(&animation->track, now_ms);
}
bool arkui_animation_active(const ArkUIAnimation *animation) {
    return animation && animation->track.active;
}
int arkui_animation_progress(const ArkUIAnimation *animation) {
    return animation ? progress(animation->track.value) : 0;
}
void arkui_animation_finish(ArkUIAnimation *animation) {
    if (!animation)
        return;
    animation->track.value = animation->track.to;
    animation->track.active = false;
}
static bool transition_geometry(const ArkUIAnimation *animation, ArkUIRect target, int anchor_x,
                                int anchor_y, int *xx, int *yy, int *ww, int *hh) {
    if (!animation || target.w <= 0 || target.h <= 0 || target.w > MOTION_MAX_DIM ||
        target.h > MOTION_MAX_DIM || target.x < -16384 || target.x > 16384 || target.y < -16384 ||
        target.y > 16384 || anchor_x < -16384 || anchor_x > 16384 || anchor_y < -16384 ||
        anchor_y > 16384)
        return false;
    int p = arkui_animation_progress(animation);
    if (!p)
        return false;
    int scale = animation->transition == ARKUI_TRANSITION_LAUNCHER
                    ? MOTION_ONE + (MOTION_ONE - p) / 5
                    : MOTION_ONE - (MOTION_ONE - p) * 6 / 100;
    int64_t w = (int64_t)target.w * scale, h = (int64_t)target.h * scale;
    int64_t x = (int64_t)target.x * MOTION_ONE + ((int64_t)target.w * MOTION_ONE - w) / 2;
    int64_t y = (int64_t)target.y * MOTION_ONE + ((int64_t)target.h * MOTION_ONE - h) / 2;
    if (animation->transition == ARKUI_TRANSITION_LAUNCHER) {
        /* Scale around the supplied content center, allowing a stationary Dock. */
        x = (int64_t)anchor_x * MOTION_ONE + ((int64_t)target.x - anchor_x) * scale;
        y = (int64_t)anchor_y * MOTION_ONE + ((int64_t)target.y - anchor_y) * scale;
    }
    if (animation->transition == ARKUI_TRANSITION_HOVER)
        y += ((int64_t)target.h * MOTION_ONE - h) / 2 + 12 * (MOTION_ONE - p);
    if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX || w > INT_MAX || h > INT_MAX)
        return false;
    *xx = (int)x;
    *yy = (int)y;
    *ww = (int)w;
    *hh = (int)h;
    return true;
}
ArkUIRect arkui_transition_bounds(const ArkUIAnimation *animation, ArkUIRect target, int anchor_x,
                                  int anchor_y) {
    int x, y, w, h;
    if (!transition_geometry(animation, target, anchor_x, anchor_y, &x, &y, &w, &h))
        return (ArkUIRect){0};
    if (animation->transition == ARKUI_TRANSITION_SHEET) {
        int left = target.x < anchor_x ? target.x : anchor_x,
            top = target.y < anchor_y ? target.y : anchor_y;
        int right = target.x + target.w > anchor_x ? target.x + target.w : anchor_x;
        int bottom = target.y + target.h > anchor_y ? target.y + target.h : anchor_y;
        return (ArkUIRect){left - 2, top - 2, right - left + 4, bottom - top + 4};
    }
    int left = x >> 16, top = y >> 16;
    int right = (int)(((int64_t)x + w + MOTION_ONE - 1) >> 16),
        bottom = (int)(((int64_t)y + h + MOTION_ONE - 1) >> 16);
    return (ArkUIRect){left - 1, top - 1, right - left + 2, bottom - top + 2};
}
void arkui_transition_draw_cached(const ArkUIAnimation *animation, uint32_t *dst, int dw, int dh,
                                  const uint32_t *src, int sw, int sh, ArkUIRect target,
                                  int anchor_x, int anchor_y, const uint32_t *colors) {
    int x, y, w, h;
    if (!transition_geometry(animation, target, anchor_x, anchor_y, &x, &y, &w, &h))
        return;
    int p = arkui_animation_progress(animation);
    if (animation->transition == ARKUI_TRANSITION_SHEET) {
        ribbon_draw_cached(dst, dw, dh, src, sw, sh, target.x, target.y, target.w, target.h,
                           anchor_x, anchor_y, p, colors);
        return;
    }
    motion_blit_cached(dst, dw, dh, src, sw, sh, x, y, w, h,
                       (unsigned)((uint64_t)p * 255 / MOTION_ONE),
                       animation->transition == ARKUI_TRANSITION_HOVER ? 14 : 0, colors);
}
void arkui_transition_draw(const ArkUIAnimation *animation, uint32_t *dst, int dw, int dh,
                           const uint32_t *src, int sw, int sh, ArkUIRect target, int anchor_x,
                           int anchor_y) {
    arkui_transition_draw_cached(animation, dst, dw, dh, src, sw, sh, target, anchor_x, anchor_y,
                                 0);
}
