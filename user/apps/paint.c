#include "app.h"
#include "drag.h"
#define LIMIT 180
static uint32_t pixels[800 * 500];
static ArkApp app;
typedef struct {
    int x, y, ex, ey;
    unsigned color;
} Segment;
static Segment segments[LIMIT];
static unsigned count, color;
static const uint32_t palette[] = {0x244666, 0x277bd8, 0xd95270, 0xe4a429, 0x369c87, 0x8f65c6};
static bool pressed, drawing;
static int previous_x, previous_y;
static char status[128] = "用鼠标或触控绘画，完成后可导出 SVG。";
static char svg[16384];
static bool add(const char *s) {
    size_t n = strlen(svg), m = strlen(s);
    if (n + m >= sizeof(svg))
        return false;
    memcpy(svg + n, s, m + 1);
    return true;
}
static bool number(unsigned n) {
    char b[24];
    uint_to_str(n, b);
    return add(b);
}
static void save(void) {
    strcopy(svg,
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"750\" height=\"300\" viewBox=\"0 0 "
            "750 300\">\n<rect width=\"750\" height=\"300\" fill=\"white\"/>\n<g fill=\"none\" "
            "stroke-width=\"4\" stroke-linecap=\"round\">\n",
            sizeof(svg));
    bool ok = true;
    const char *hex = "0123456789abcdef";
    for (unsigned i = 0; i < count && ok; i++) {
        Segment *s = &segments[i];
        char rgb[8] = "#000000";
        for (unsigned j = 0; j < 6; j++)
            rgb[j + 1] = hex[(palette[s->color] >> (20 - j * 4)) & 15];
        ok = add("<path stroke=\"") && add(rgb) && add("\" d=\"M") && number((unsigned)s->x) &&
             add(" ") && number((unsigned)s->y) && add(" L") && number((unsigned)s->ex) &&
             add(" ") && number((unsigned)s->ey) && add("\"/>\n");
    }
    if (!ok || !add("</g></svg>\n")) {
        strcopy(status, "Drawing exceeds the 16383-byte document limit.", sizeof(status));
        return;
    }
    if (app_write_text("Paint.svg", svg, status))
        strcopy(status, "Saved Paint.svg in your permitted files. Open it on another system.",
                sizeof(status));
}
static void draw(void) {
    int w = app_width(&app), h = app_height(&app), cw = w - 50, ch = h - 200;
    app_clear(&app, 0xf0f5fb);
    app_cursor(&app, 25, 98, cw, ch, ARK_CURSOR_CROSSHAIR);
    app_text(&app, 25, 18, "画板", 0x253b58, 2);
    app_text(&app, 27, 58, "记录你的灵感", 0x73869e, 1);
    for (unsigned i = 0; i < 6; i++) {
        app_cursor(&app, w - 279 + (int)i * 43, 27, 29, 29, ARK_CURSOR_POINTER);
        if (i == color)
            app_round(&app, app_width(&app) - 284 + (int)i * 43, 22, 39, 39, 12, 0xc3d2e8);
        app_round(&app, app_width(&app) - 279 + (int)i * 43, 27, 29, 29, 9, palette[i]);
    }
    app_round(&app, 23, 95, cw + 4, ch + 6, 14, 0xc6d4e3);
    app_rect_raw(&app, 25, 98, cw, ch, 0xffffff);
    for (unsigned i = 0; i < count; i++) {
        Segment *s = &segments[i];
        app_line(&app, 25 + s->x * cw / 750, 98 + s->y * ch / 300, 25 + s->ex * cw / 750,
                 98 + s->ey * ch / 300, palette[s->color], 4);
    }
    app_button(&app, 26, app_height(&app) - 83, 92, 39, "撤销", false);
    app_button(&app, 130, app_height(&app) - 83, 92, 39, "清空", false);
    app_button(&app, app_width(&app) - 155, app_height(&app) - 83, 128, 39, "导出 SVG", true);
    char n[24];
    uint_to_str(count, n);
    app_text(&app, 235, h - 72, n, 0x6c829b, 1);
    app_text(&app, 268, h - 72, "/ 180", 0x6c829b, 1);
    app_text(&app, 26, h - 26, status, 0x73869e, 1);
    app_present(&app);
}
static bool inside(int x, int y) {
    return app_hit(x, y, 28, 101, app_width(&app) - 56, app_height(&app) - 206);
}
int main(void) {
    if (!app_open(&app, "画板", pixels, 800, 500))
        return 1;
    draw();
    app_log("[app] Paint ring3 ready\n");
    for (;;) {
        ArkEvent e;
        bool dirty = false;
        unsigned batch = 0;
        while (batch++ < 16 && app_event(&app, &e)) {
            if (e.type == ARK_EV_DROP) {
                ArkDragRequest drop;
                if (app_drop_read(&app, &e, &drop))
                    app_drop_accept(&app, &drop, false);
            }
            if (e.type == ARK_EV_NEW) {
                count = 0;
                dirty = true;
            }
            if (e.type == ARK_EV_KEY) {
                if (e.key == 'c' || e.key == 'C')
                    count = 0;
                else if ((e.key == 'z' || e.key == 'Z') && count)
                    count--;
                else if (e.key == 's' || e.key == 'S' || e.key == 19)
                    save();
                dirty = true;
                continue;
            }
            if (e.type != ARK_EV_POINTER && e.type != ARK_EV_MOUSE && e.type != ARK_EV_TOUCH)
                continue;
            bool down = (e.buttons & 1) != 0;
            if (app_released(&app, &e)) {
                for (unsigned i = 0; i < 6; i++)
                    if (app_click_hit(&app, &e, app_width(&app) - 284 + (int)i * 43, 22, 39, 39)) {
                        color = i;
                        dirty = true;
                    }
                if (app_click_hit(&app, &e, 26, app_height(&app) - 83, 92, 39) && count) {
                    count--;
                    dirty = true;
                }
                if (app_click_hit(&app, &e, 130, app_height(&app) - 83, 92, 39)) {
                    count = 0;
                    dirty = true;
                }
                if (app_click_hit(&app, &e, app_width(&app) - 155, app_height(&app) - 83, 128,
                                  39)) {
                    save();
                    dirty = true;
                }
            }
            if (down && !pressed) {
                drawing = inside(e.x, e.y);
                previous_x = (e.x - 25) * 750 / (app_width(&app) - 50);
                previous_y = (e.y - 98) * 300 / (app_height(&app) - 200);
            }
            if (down && drawing && inside(e.x, e.y)) {
                int x = (e.x - 25) * 750 / (app_width(&app) - 50),
                    y = (e.y - 98) * 300 / (app_height(&app) - 200), dx = x - previous_x,
                    dy = y - previous_y;
                if (dx * dx + dy * dy >= 9) {
                    if (count < LIMIT) {
                        segments[count++] = (Segment){previous_x, previous_y, x, y, color};
                        dirty = true;
                    } else {
                        strcopy(status, "Segment limit reached. Export, Undo or Clear to continue.",
                                sizeof(status));
                        dirty = true;
                    }
                    previous_x = x;
                    previous_y = y;
                }
            }
            if (!down)
                drawing = false;
            pressed = down;
        }
        if (dirty || app.dirty)
            draw();
        if (batch >= 16)
            ark_yield();
        else
            app_wait(100);
    }
}
