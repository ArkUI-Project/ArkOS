/* Compile into a native ArkOS application module. No Swift runtime is needed.
 * The host supplies clipped rect/text painters; text uses unicode_draw(). */
#include "arkui.h"

typedef struct {
    ArkUI views;
    bool dark;
    int tint;
    unsigned saves;
} PreferencesDemo;

void preferences_demo_init(PreferencesDemo *demo) {
    arkui_init(&demo->views);
    demo->dark = false;
    demo->tint = 170;
    demo->saves = 0;
    int card = arkui_card(&demo->views, 0, 16, 12);
    arkui_text(&demo->views, card, "外观设置");
    arkui_toggle(&demo->views, card, "深色模式", &demo->dark, 1);
    arkui_slider(&demo->views, card, "玻璃染色", &demo->tint, 70, 235, 2);
    int actions = arkui_hstack(&demo->views, card, 0, 10);
    arkui_spacer(&demo->views, actions, 1);
    arkui_button(&demo->views, actions, "保存", 3);
}

void preferences_demo_draw(PreferencesDemo *demo, ArkUIRect bounds, const ArkUIPainter *painter,
                           const ArkUITheme *theme) {
    arkui_layout(&demo->views, bounds);
    arkui_draw(&demo->views, painter, theme);
}

bool preferences_demo_pointer(PreferencesDemo *demo, int x, int y, bool down) {
    ArkUIAction action;
    bool consumed = arkui_pointer(&demo->views, x, y, down, &action);
    if (action.changed && action.id == 3)
        demo->saves++;
    /* dark/tint bindings are already updated. The host must request a frame
     * when arkui_take_dirty(&demo->views) returns true and persist if desired. */
    return consumed;
}
