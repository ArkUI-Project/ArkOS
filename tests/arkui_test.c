#include "arkui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ArkUI ui;
static unsigned rect_calls, text_calls;
static bool inside(ArkUIRect outer, ArkUIRect inner) {
    return inner.w >= 0 && inner.h >= 0 && inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;
}
static void verify_layout(void) {
    for (int i = 0; i < ui.count; i++) {
        ArkUINode *n = &ui.nodes[i];
        assert(inside(ui.bounds, n->frame));
        assert(inside(n->frame, n->clip));
        if (n->parent >= 0)
            assert(inside(ui.nodes[n->parent].frame, n->frame));
        int previous = -1;
        for (int c = n->first_child; c >= 0; c = ui.nodes[c].next_sibling) {
            ArkUIRect r = ui.nodes[c].frame;
            int start = n->kind == ARKUI_HSTACK ? r.x : r.y;
            if (previous >= 0)
                assert(start >= previous);
            previous = start + (n->kind == ARKUI_HSTACK ? r.w : r.h);
        }
    }
}
static void rect_paint(void *context, ArkUIRect r, int radius, uint32_t color, int alpha,
                       ArkUIRect clip) {
    (void)context;
    (void)radius;
    (void)color;
    assert(alpha >= 0 && alpha <= 255);
    assert(inside(ui.bounds, clip));
    assert(inside(r, clip));
    rect_calls++;
}
static void text_paint(void *context, int x, int y, const char *text, uint32_t color,
                       ArkUIRect clip) {
    (void)context;
    (void)x;
    (void)y;
    (void)color;
    assert(text != NULL);
    assert(inside(ui.bounds, clip));
    text_calls++;
}
static void paint(void) {
    ArkUIPainter painter = {.context = 0, .rect = rect_paint, .text = text_paint};
    ArkUITheme theme = {0x123456, 0x456789, 0xffffff, 0xdddddd, 0x3399dd,
                        0xffffff, 0x999999, 180,      180};
    arkui_draw(&ui, &painter, &theme);
}
static void interactions(void) {
    bool state = false;
    int level = 50;
    char label[32] = "中文界面";
    arkui_init(&ui);
    int card = arkui_card(&ui, 0, 12, 8);
    int toggle = arkui_toggle(&ui, card, label, &state, 10);
    int slider = arkui_slider(&ui, card, "强度", &level, 0, 100, 20);
    int button = arkui_button(&ui, card, "保存", 30);
    strcpy(label, "changed");
    assert(strcmp(ui.nodes[toggle].text, "中文界面") == 0);
    arkui_layout(&ui, (ArkUIRect){17, 23, 340, 260});
    verify_layout();
    paint();
    assert(rect_calls > 5 && text_calls >= 3);
    assert(!arkui_take_dirty(&ui));
    ArkUIRect r = ui.nodes[toggle].frame;
    ArkUIAction action;
    assert(arkui_hit_test(&ui, r.x + 3, r.y + 3) == 10);
    assert(arkui_pointer(&ui, r.x + 3, r.y + 3, true, &action));
    assert(!state && !action.changed);
    arkui_pointer(&ui, r.x + 3, r.y + 3, false, &action);
    assert(state && action.id == 10 && action.value == 1);
    assert(arkui_take_dirty(&ui));
    r = ui.nodes[slider].frame;
    arkui_pointer(&ui, r.x + 10, r.y + 42, true, &action);
    assert(level == 0 && action.changed);
    arkui_pointer(&ui, r.x + r.w + 100, r.y - 200, true, &action);
    assert(level == 100 && action.changed);
    arkui_pointer(&ui, r.x + r.w + 100, r.y - 200, false, &action);
    assert(ui.capture_action == 0);
    r = ui.nodes[button].frame;
    arkui_pointer(&ui, r.x + 8, r.y + 8, true, &action);
    arkui_pointer(&ui, 0, 0, false, &action);
    assert(!action.changed); /* cancel outside */
    ui.nodes[card].enabled = false;
    arkui_layout(&ui, ui.bounds);
    assert(!arkui_hit_test(&ui, r.x + 8, r.y + 8));
    ui.nodes[card].enabled = true;
    arkui_layout(&ui, ui.bounds);
    assert(arkui_hit_test(&ui, r.x + 8, r.y + 8) == 30);
    arkui_pointer(&ui, r.x + 8, r.y + 8, true, &action);
    arkui_reset(&ui); /* copied text and node indices change; capture identity persists */
    card = arkui_card(&ui, 0, 12, 8);
    arkui_toggle(&ui, card, "中文界面", &state, 10);
    arkui_slider(&ui, card, "强度", &level, 0, 100, 20);
    arkui_button(&ui, card, "保存", 30);
    arkui_layout(&ui, (ArkUIRect){17, 23, 340, 260});
    arkui_pointer(&ui, r.x + 8, r.y + 8, false, &action);
    assert(action.changed && action.id == 30);
    assert(!arkui_pointer(&ui, 0, 0, true, &action));
    arkui_pointer(&ui, r.x + 8, r.y + 8, true, &action);
    assert(!action.changed); /* no press-through */
    arkui_cancel_pointer(&ui);
    assert(!ui.pointer_down && !ui.capture_action);
}
static void constrained_layout(void) {
    arkui_init(&ui);
    ui.nodes[0].style.padding = 9;
    ui.nodes[0].style.gap = 5;
    int top = arkui_hstack(&ui, 0, 2, 8);
    int a = arkui_button(&ui, top, "甲", 1);
    arkui_size(&ui, a, 40, 36);
    arkui_spacer(&ui, top, 1);
    int b = arkui_button(&ui, top, "乙", 2);
    arkui_size(&ui, b, 80, 36);
    int c = arkui_button(&ui, 0, "有限宽度", 3);
    ui.nodes[c].style.max_width = 70;
    arkui_layout(&ui, (ArkUIRect){0, 0, 360, 220});
    verify_layout();
    assert(ui.nodes[a].frame.w == 40 && ui.nodes[b].frame.w == 80 && ui.nodes[c].frame.w == 70);
    assert(ui.nodes[b].frame.x > ui.nodes[a].frame.x + 150);
    /* Constraint conflicts compress within the parent's finite viewport. */
    for (int h = 0; h < 180; h += 3)
        for (int w = 0; w < 300; w += 7) {
            arkui_layout(&ui, (ArkUIRect){-7, 11, w, h});
            verify_layout();
            paint();
        }
}
static void capacity(void) {
    arkui_init(&ui);
    for (int i = 1; i < ARKUI_MAX_NODES; i++)
        assert(arkui_text(&ui, 0, "中") == i);
    assert(arkui_text(&ui, 0, "overflow") == -1 && ui.overflow && ui.count == ARKUI_MAX_NODES);
    arkui_layout(&ui, (ArkUIRect){0, 0, 320, 200});
    verify_layout();
    arkui_reset(&ui);
    assert(!ui.overflow);
    int button = arkui_button(&ui, 0, "A", 99);
    assert(arkui_button(&ui, 0, "duplicate action", 99) == -1);
    assert(arkui_text(&ui, button, "not a container") == -1);
    static char huge[ARKUI_TEXT_CAP];
    memset(huge, 'A', sizeof(huge) - 1);
    arkui_reset(&ui);
    assert(arkui_text(&ui, 0, huge) == 1);
    assert(arkui_text(&ui, 0, "") == -1 && ui.count == 2);
    arkui_layout(&ui, (ArkUIRect){0, 0, 200, 100});
    verify_layout();
}
int main(void) {
    interactions();
    constrained_layout();
    capacity();
    puts("ArkUI PASS: Unicode layout, binding, pointer capture/rebuild, clipping, constraints, "
         "capacity");
    return 0;
}
