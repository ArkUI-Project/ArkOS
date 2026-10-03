/* Reusable native UI composition + state-driven animation. Build freestanding:
 * cc -std=c11 -ffreestanding -Iinclude -c examples/arkui_transition_demo.c
 * The host owns the cached pixels, restores the backdrop before each frame,
 * supplies monotonic milliseconds, presents damage, and routes user input. */
#include "arkui.h"

typedef struct {
    ArkUI views;
    ArkUIAnimation visibility;
    bool enabled;
} ArkUITransitionDemo;

void arkui_transition_demo_init(ArkUITransitionDemo *demo) {
    demo->enabled = true;
    arkui_init(&demo->views);
    arkui_animation_init(&demo->visibility, ARKUI_TRANSITION_SHEET, false);
    int toolbar = arkui_toolbar(&demo->views, 0, 4, 6);
    arkui_icon_button(&demo->views, toolbar, ARKUI_SYMBOL_BACK, "返回", 1);
    arkui_icon_button(&demo->views, toolbar, ARKUI_SYMBOL_NEW_DOCUMENT, "新建文稿", 2);
    arkui_spacer(&demo->views, toolbar, 1);
    arkui_icon_button(&demo->views, toolbar, ARKUI_SYMBOL_SHARE, "共享", 3);
    int card = arkui_card(&demo->views, 0, 16, 12);
    arkui_material(&demo->views, card, ARKUI_MATERIAL_CONTENT);
    arkui_text(&demo->views, card, "原生界面 · ArkUI");
    arkui_toggle(&demo->views, card, "启用功能", &demo->enabled, 4);
}

void arkui_transition_demo_cache(ArkUITransitionDemo *demo, ArkUISurface *cache, bool dark) {
    ArkUIPainter painter = arkui_canvas_painter(cache);
    ArkUITheme theme = arkui_theme(dark);
    arkui_layout(&demo->views, (ArkUIRect){0, 0, cache->width, cache->height});
    arkui_draw(&demo->views, &painter, &theme);
}

void arkui_transition_demo_presented(ArkUITransitionDemo *demo, bool presented, uint64_t now_ms,
                                     bool reduced_motion) {
    arkui_animation_set(&demo->visibility, presented, now_ms, reduced_motion);
}

bool arkui_transition_demo_frame(ArkUITransitionDemo *demo, uint64_t now_ms, uint32_t *destination,
                                 int width, int height, const uint32_t *cached, int cached_width,
                                 int cached_height, ArkUIRect target, int dock_x, int dock_y) {
    arkui_animation_update(&demo->visibility, now_ms);
    arkui_transition_draw(&demo->visibility, destination, width, height, cached, cached_width,
                          cached_height, target, dock_x, dock_y);
    return arkui_animation_active(&demo->visibility);
}
