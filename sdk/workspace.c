/* Original ArkOS SDK example: independent surfaces, resize, theme, text input
 * and typed copy drag/drop. It uses no host service or POSIX runtime. */
#include "app.h"
#include "text.h"
#include "drag.h"
#define WINDOWS 4
static struct {
    ArkApp app;
    char text[512];
    size_t length, cursor;
    bool held, dragging;
    int x, y;
} views[WINDOWS];
static bool create(void) {
    for (unsigned i = 0; i < WINDOWS; i++)
        if (!views[i].app.pixels || views[i].app.closed) {
            uint32_t *pixels = ark_memory(800u * 500u * 4u);
            if (!pixels)
                return false;
            memset(&views[i], 0, sizeof views[i]);
            if (!app_open(&views[i].app, "Workspace", pixels, 800, 500))
                return false;
            app_log("[workspace] surface opened\n");
            return true;
        }
    return false;
}
static void draw(unsigned i) {
    ArkApp *a = &views[i].app;
    int w = app_width(a), h = app_height(a);
    app_clear(a, 0xf4f7fb);
    app_text(a, 24, 20, "拖拽工作区", 0x283d57, 2);
    app_button(a, w - 140, 18, 116, 38, "新建窗口", true);
    app_round(a, 20, 82, w - 40, h - 145, 12, 0xffffff);
    app_clip(a, 32, 98, views[i].text[0] ? views[i].text : "输入文字，或把文字、文件拖到这里",
             w - 64, 0x314660);
    app_clip(a, 24, h - 36, "拖动文字可以交给其他窗口或顶部暂存区", w - 48, 0x6b8199);
    app_text_input(a, true, 32, 98, 20);
    app_present(a);
}
int main(void) {
    if (!create())
        return 1;
    for (;;) {
        unsigned live = 0;
        for (unsigned i = 0; i < WINDOWS; i++) {
            ArkApp *a = &views[i].app;
            if (!a->pixels || a->closed)
                continue;
            live++;
            ArkEvent e;
            unsigned batch = 0;
            while (batch++ < 16 && app_event(a, &e)) {
                if (a->closed)
                    break;
                if (e.type == ARK_EV_TEXT)
                    app_codepoint(views[i].text, &views[i].length, &views[i].cursor, 512,
                                  (uint32_t)e.key);
                if (e.type == ARK_EV_KEY) {
                    if (e.key == 14)
                        create();
                    else
                        app_edit(views[i].text, &views[i].length, &views[i].cursor, 512, e.key);
                }
                if (e.type == ARK_EV_DROP) {
                    ArkDragRequest drop;
                    if (app_drop_read(a, &e, &drop)) {
                        bool accepted = app_insert(views[i].text, &views[i].length,
                                                   &views[i].cursor, 512, drop.data);
                        app_drop_accept(a, &drop, accepted);
                        if (accepted)
                            app_log("[workspace] drop accepted\n");
                    }
                }
                if (e.type == ARK_EV_DRAG_END)
                    views[i].held = views[i].dragging = false;
                if (e.type == ARK_EV_POINTER) {
                    bool down = e.buttons & 1;
                    if (app_click_hit(a, &e, app_width(a) - 140, 18, 116, 38))
                        create();
                    if (down && !views[i].held) {
                        if (!app_hit(e.x, e.y, app_width(a) - 140, 18, 116, 38)) {
                            views[i].dragging = true;
                            views[i].x = e.x;
                            views[i].y = e.y;
                        }
                    } else if (down && views[i].dragging) {
                        int dx = e.x - views[i].x, dy = e.y - views[i].y;
                        if (dx * dx + dy * dy > 144) {
                            if (views[i].length)
                                app_drag_begin(a, ARK_DRAG_TEXT, views[i].text, "text/plain");
                            views[i].dragging = false;
                        }
                    }
                    if (!down)
                        views[i].dragging = false;
                    views[i].held = down;
                }
                a->dirty = true;
            }
            if (!a->closed && a->dirty)
                draw(i);
        }
        if (!live)
            return 0;
        app_wait(100);
    }
}
