#include "arkui.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define GUARD 16
#define WIDTH 61
#define HEIGHT 49
#define STRIDE 67
#define CANARY 0x7ac159e3u
static uint32_t guarded[GUARD + STRIDE * HEIGHT + GUARD];
static ArkUI ui;
static void clear_canvas(void) {
    for (unsigned i = 0; i < sizeof guarded / sizeof guarded[0]; i++)
        guarded[i] = CANARY;
}
static unsigned changed_in(ArkUIRect clip) {
    unsigned changed = 0;
    for (int i = 0; i < GUARD; i++)
        assert(guarded[i] == CANARY && guarded[GUARD + STRIDE * HEIGHT + i] == CANARY);
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < STRIDE; x++) {
            uint32_t value = guarded[GUARD + y * STRIDE + x];
            bool inside = x >= clip.x && x < clip.x + clip.w && y >= clip.y && y < clip.y + clip.h;
            if (!inside)
                assert(value == CANARY);
            else
                changed += value != CANARY;
        }
    return changed;
}
static void icons(void) {
    ArkUISurface surface = arkui_surface(guarded + GUARD, STRIDE, WIDTH, HEIGHT);
    surface.clip = (ArkUIRect){10, 7, 30, 28};
    uint32_t previous[STRIDE * HEIGHT];
    for (int app = 0; app < ARKUI_APP_COUNT; app++) {
        clear_canvas();
        arkui_app_icon(&surface, -9, -8, 70, (ArkUIAppIcon)app);
        assert(changed_in(surface.clip) > 400);
        memcpy(previous, guarded + GUARD, sizeof previous);
        clear_canvas();
        arkui_app_icon(&surface, -9, -8, 70, (ArkUIAppIcon)app);
        assert(!memcmp(previous, guarded + GUARD, sizeof previous));
    }
    for (int symbol = 0; symbol < ARKUI_SYMBOL_COUNT; symbol++) {
        clear_canvas();
        arkui_symbol(&surface, 9, 6, 32, (ArkUISymbol)symbol, 0xf9fcff);
        assert(changed_in(surface.clip) > 3);
        assert(strcmp(arkui_symbol_name((ArkUISymbol)symbol), "unknown"));
    }
    clear_canvas();
    arkui_app_icon(&surface, INT_MIN, INT_MAX, 512, ARKUI_APP_TERMINAL);
    arkui_app_icon(&surface, 0, 0, 513, ARKUI_APP_TERMINAL);
    arkui_app_icon(&surface, 0, 0, 32, (ArkUIAppIcon)-1);
    arkui_symbol(&surface, INT_MAX, INT_MIN, 20, ARKUI_SYMBOL_CLOSE, 0xffffff);
    arkui_symbol(&surface, 0, 0, 20, ARKUI_SYMBOL_COUNT, 0xffffff);
    assert(changed_in(surface.clip) == 0);
    surface.stride = WIDTH - 1;
    arkui_app_icon(&surface, 0, 0, 40, ARKUI_APP_CLOCK);
    assert(changed_in(surface.clip) == 0);
}
static void transitions(void) {
    const unsigned enter[] = {230, 280, 120}, leave[] = {330, 230, 100};
    for (int kind = 0; kind < 3; kind++) {
        ArkUIAnimation animation;
        arkui_animation_init(&animation, (ArkUITransition)kind, false);
        assert(arkui_animation_duration((ArkUITransition)kind, true) == enter[kind]);
        assert(arkui_animation_duration((ArkUITransition)kind, false) == leave[kind]);
        assert(!arkui_animation_progress(&animation) && !arkui_animation_active(&animation));
        arkui_animation_set(&animation, true, 1000, false);
        assert(animation.track.duration == enter[kind] && arkui_animation_active(&animation));
        arkui_animation_update(&animation, 1050);
        int before = arkui_animation_progress(&animation);
        assert(before > 0 && before < MOTION_ONE);
        arkui_animation_set(&animation, true, 1050, false);
        assert(animation.track.start == 1000);
        arkui_animation_set(&animation, false, 1050, false);
        assert(animation.track.from == before && arkui_animation_progress(&animation) == before);
        assert(animation.track.duration > 0 && animation.track.duration <= leave[kind]);
        arkui_animation_update(&animation, 2000);
        assert(!arkui_animation_progress(&animation));
        assert(!arkui_animation_active(&animation));
        arkui_animation_set(&animation, true, 2000, true);
        assert(arkui_animation_progress(&animation) == MOTION_ONE);
        assert(!arkui_animation_active(&animation));
        arkui_animation_set(&animation, false, 2100, false);
        arkui_animation_finish(&animation);
        assert(!arkui_animation_progress(&animation) && !arkui_animation_active(&animation));
    }
    enum { W = 64, H = 48, SW = 20, SH = 16 };
    uint32_t dest[GUARD + W * H + GUARD], source[SW * SH];
    for (int i = 0; i < SW * SH; i++)
        source[i] = 0x1877dd;
    for (int kind = 0; kind < 3; kind++)
        for (int phase = 0; phase < 3; phase++) {
            for (unsigned i = 0; i < sizeof dest / sizeof dest[0]; i++)
                dest[i] = CANARY;
            ArkUIAnimation animation;
            arkui_animation_init(&animation, (ArkUITransition)kind, false);
            animation.track.value = phase * (MOTION_ONE / 2);
            arkui_transition_draw(&animation, dest + GUARD, W, H, source, SW, SH,
                                  (ArkUIRect){20, 15, 20, 16}, 32, 39);
            for (int i = 0; i < GUARD; i++)
                assert(dest[i] == CANARY && dest[GUARD + W * H + i] == CANARY);
            unsigned changed = 0;
            for (int i = 0; i < W * H; i++)
                changed += dest[GUARD + i] != CANARY;
            if (!phase)
                assert(!changed);
            else
                assert(changed);
            if (phase == 2)
                assert(dest[GUARD + 23 * W + 30] == source[8 * SW + 10]);
            arkui_transition_draw(&animation, dest + GUARD, W, H, source, SW, SH,
                                  (ArkUIRect){-12, -7, 30, 24}, -5, -5);
            for (int i = 0; i < GUARD; i++)
                assert(dest[i] == CANARY && dest[GUARD + W * H + i] == CANARY);
        }
}
static void high_refresh(void) {
    const unsigned rates[] = {60, 120, 144, 240};
    for (unsigned rate = 0; rate < 4; rate++) {
        MotionFrameClock clock;
        motion_frame_init(&clock, 123, rates[rate]);
        unsigned frames = 0;
        uint64_t previous = 123;
        for (uint64_t now = 123; now < 10123; now++)
            if (now >= clock.deadline) {
                if (frames)
                    assert(now - previous >= 1000 / rates[rate] &&
                           now - previous <= 1000 / rates[rate] + 1);
                previous = now;
                frames++;
                motion_frame_advance(&clock, now);
            }
        assert(frames == rates[rate] * 10);
        motion_frame_advance(&clock, 10999);
        assert(clock.deadline > 10999);
    }
    enum { W = 83, H = 65, SW = 31, SH = 27 };
    uint32_t source[SW * SH], dest[W * H], tiled[W * H];
    for (int i = 0; i < SW * SH; i++)
        source[i] = 0x345678;
    ArkUIAnimation animation;
    const ArkUITransition kinds[] = {ARKUI_TRANSITION_SHEET, ARKUI_TRANSITION_LAUNCHER};
    for (unsigned kind = 0; kind < 2; kind++)
        for (int phase = 0; phase <= 16; phase++) {
            arkui_animation_init(&animation, kinds[kind], false);
            memset(dest, 0, sizeof dest);
            animation.track.value = phase * MOTION_ONE / 16;
            ArkUIRect bounds =
                arkui_transition_bounds(&animation, (ArkUIRect){7, 9, SW, SH}, 72, 61);
            arkui_transition_draw(&animation, dest, W, H, source, SW, SH, (ArkUIRect){7, 9, SW, SH},
                                  72, 61);
            memset(tiled, 0, sizeof tiled);
            for (int tile = 0; tile < 4; tile++) {
                int first = H * tile / 4, last = H * (tile + 1) / 4;
                arkui_transition_draw(&animation, tiled + first * W, W, last - first, source, SW,
                                      SH, (ArkUIRect){7, 9 - first, SW, SH}, 72, 61 - first);
            }
            assert(!memcmp(dest, tiled, sizeof dest));
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    if (dest[y * W + x])
                        assert(x >= bounds.x && y >= bounds.y && x < bounds.x + bounds.w &&
                               y < bounds.y + bounds.h);
            if (phase == 16)
                assert(dest[22 * W + 22] == 0x345678);
        }
}
static void toolbar(void) {
    arkui_init(&ui);
    int bar = arkui_toolbar(&ui, 0, 0, 4);
    arkui_size(&ui, bar, ARKUI_FILL, 36);
    int ids[7];
    for (int i = 0; i < 7; i++) {
        if (i == 3)
            arkui_spacer(&ui, bar, 1);
        ids[i] = arkui_icon_button(&ui, bar, (ArkUISymbol)i, i == 0 ? "上一级" : "操作", 100 + i);
    }
    arkui_layout(&ui, (ArkUIRect){198, 68, 520, 36});
    for (int i = 0; i < 7; i++) {
        ArkUIRect frame = ui.nodes[ids[i]].frame;
        assert(frame.x == (i < 3 ? 198 + i * 40 : 562 + (i - 3) * 40));
        assert(frame.y == 68 && frame.w == 36 && frame.h == 36);
        assert(arkui_hit_test(&ui, frame.x + 1, frame.y + 1) == 100 + i);
    }
    ArkUIAction action;
    arkui_pointer(&ui, 200, 70, true, &action);
    assert(!action.changed);
    assert(!strcmp(arkui_hover_label(&ui), "上一级"));
    arkui_pointer(&ui, 200, 70, false, &action);
    assert(action.changed && action.id == 100);
    ui.nodes[bar].enabled = false;
    arkui_layout(&ui, ui.bounds);
    assert(!arkui_hit_test(&ui, 200, 70));
    assert(!arkui_hit_test(&ui, INT_MIN, INT_MAX));
    /* Real clipped CPU painter, Unicode, toolbar material and vector symbol. */
    arkui_init(&ui);
    bar = arkui_toolbar(&ui, 0, 1, 3);
    arkui_icon_button(&ui, bar, ARKUI_SYMBOL_SEARCH, "搜索", 1);
    arkui_layout(&ui, (ArkUIRect){8, 5, 40, 40});
    ArkUISurface surface = arkui_surface(guarded + GUARD, STRIDE, WIDTH, HEIGHT);
    surface.clip = (ArkUIRect){10, 7, 30, 28};
    ArkUIPainter painter = arkui_canvas_painter(&surface);
    ArkUITheme theme = arkui_theme(false);
    clear_canvas();
    arkui_draw(&ui, &painter, &theme);
    assert(changed_in(surface.clip) > 300);
    assert(!arkui_take_dirty(&ui));
    arkui_reset(&ui);
    int content = arkui_card(&ui, 0, 0, 0);
    arkui_material(&ui, content, ARKUI_MATERIAL_CONTENT);
    arkui_text(&ui, content, "你好");
    arkui_layout(&ui, (ArkUIRect){8, 5, 40, 40});
    clear_canvas();
    arkui_draw(&ui, &painter, &theme);
    assert(changed_in(surface.clip) > 300);
}
static void damage(void) {
    ArkUIDamage batch;
    ArkUIRect out[ARKUI_DAMAGE_CAP];
    arkui_damage_reset(&batch, (ArkUIRect){0, 0, 100, 100});
    arkui_damage_add(&batch, (ArkUIRect){-5, -5, 10, 10});
    arkui_damage_add(&batch, (ArkUIRect){10, 0, 5, 5});
    arkui_damage_add(&batch, (ArkUIRect){5, 0, 5, 5}); /* bridge merges both previous */
    assert(arkui_damage_take(&batch, out) == 1);
    assert(out[0].x == 0 && out[0].y == 0 && out[0].w == 15 && out[0].h == 5);
    assert(!arkui_damage_take(&batch, out));
    for (int i = 0; i < 9; i++)
        arkui_damage_add(&batch, (ArkUIRect){i * 10, 40, 2, 2});
    assert(batch.full && batch.count == 1);
    assert(arkui_damage_take(&batch, out) == 1 && out[0].w == 100 && out[0].h == 100);
    arkui_damage_add(&batch, (ArkUIRect){INT_MIN, INT_MIN, INT_MAX, INT_MAX});
    assert(!batch.count);
    ArkUIRect clip = arkui_intersection((ArkUIRect){INT_MAX - 3, INT_MAX - 3, INT_MAX, INT_MAX},
                                        (ArkUIRect){0, 0, 100, 100});
    assert(!clip.w && !clip.h);
}
int main(void) {
    icons();
    transitions();
    high_refresh();
    toolbar();
    damage();
    puts("ArkUI v5 PASS: 15 icons, 26 symbols, clipped stride, presets/reversal, transition "
         "guards, toolbar actions/materials, damage batching");
    return 0;
}
