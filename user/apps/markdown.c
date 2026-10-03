#include "app.h"
#include "drag.h"
#include "text.h"
static uint32_t pixels[800 * 500];
static ArkApp app;
static const char example[] =
    "# 欢迎\n\n在这里阅读和整理你的文档。\n\n## 试一试\n- 把这份示例保存为 Welcome.md。\n- "
    "在文本编辑中修改文档。\n- 回到这里打开文件。\n\n## 常用操作\n方向键或 PageUp / PageDown "
    "滚动。\n点击路径输入框，输入文件位置后按 Enter 打开。\n\n## "
    "格式\n支持标题、段落、列表、分隔线和代码块。\n";
static char document[16384],
    path[128] = "Welcome.md",
    status[128] = "Sample document · save it, edit it, then open it again.";
static int scroll;
static bool editing, pressed;
static bool open_path(const char *target) {
    char loaded[sizeof document];
    int n = app_read_text(target, loaded, sizeof loaded, status);
    if (n < 0)
        return false;
    memcpy(document, loaded, (size_t)n + 1);
    strcopy(path, target, sizeof path);
    scroll = 0;
    strcopy(status, "文档已打开。", sizeof status);
    return true;
}
static void opened(void) {
    (void)open_path(path);
}
static void saved(void) {
    if (app_write_text(path, document, status))
        strcopy(status, "Saved. Use the system text editor, then choose Open.", sizeof(status));
}
static void draw(void) {
    int w = app_width(&app), h = app_height(&app);
    app_clear(&app, 0xf2f5f9);
    app_text(&app, 24, 15, "Markdown", 0x273c58, 2);
    app_round(&app, 26, 62, app_width(&app) - 303, 37, 8, editing ? 0xe3eeff : 0xffffff);
    char display[128];
    strcopy(display, path, sizeof(display));
    app_clip(&app, 36, 70, display, w - 325, 0x3b526e);
    app_button(&app, app_width(&app) - 263, 62, 96, 37, "打开", true);
    app_button(&app, app_width(&app) - 154, 62, 128, 37, "保存", false);
    app_round(&app, 24, 111, w - 48, h - 150, 14, 0xffffff);
    const char *p = document;
    int line = 0, y = 126;
    bool code = false;
    while (*p && y < h - 55) {
        const char *begin = p;
        while (*p && *p != '\n')
            p++;
        size_t n = (size_t)(p - begin);
        if (*p)
            p++;
        if (n >= 3 && !strncmp(begin, "```", 3)) {
            code = !code;
            line++;
            continue;
        }
        if (line++ < scroll)
            continue;
        int scale = 1, x = 42;
        uint32_t color = 0x455971;
        size_t skip = 0;
        if (!code && n && begin[0] == '#') {
            while (skip < n && begin[skip] == '#')
                skip++;
            if (skip < n && begin[skip] == ' ') {
                scale = skip == 1 ? 2 : 1;
                color = 0x245a9d;
                skip++;
            } else
                skip = 0;
        }
        if (!code && n >= 2 && begin[0] == '-' && begin[1] == ' ') {
            app_round(&app, 44, y + 7, 5, 5, 2, 0x4383bc);
            x = 59;
            skip = 2;
        }
        if (!code && n >= 3 && begin[0] == '-' && begin[1] == '-' && begin[2] == '-') {
            app_rect(&app, 42, y + 9, w - 88, 1, 0xdce4ef);
            y += 23;
            continue;
        }
        if (!n) {
            y += 12;
            continue;
        }
        size_t at = skip;
        while (at < n && y < h - 55) {
            char text[256];
            size_t length = 0;
            int width = 0;
            while (at + length < n && length + 4 < sizeof(text)) {
                const char *q = begin + at + length;
                int cp = utf8_decode(&q), advance = unicode_codepoint_width((uint32_t)cp) * scale;
                size_t bytes = (size_t)(q - (begin + at + length));
                if (width + advance > w - 90 - (x - 42))
                    break;
                memcpy(text + length, begin + at + length, bytes);
                length += bytes;
                width += advance;
            }
            if (!length) {
                at++;
                continue;
            }
            text[length] = 0;
            if (code)
                app_round(&app, 37, y - 2, w - 79, 24, 4, 0xedf2f8);
            app_text(&app, x, y, text, code ? 0x36684e : color, scale);
            at += length;
            y += scale == 2 ? 39 : 24;
        }
    }
    app_text(&app, 28, h - 25, status, 0x73849b, 1);
    app_text_input(&app, editing, 32, 70, 20);
    app_present(&app);
}
int main(void) {
    strcopy(document, example, sizeof(document));
    if (!app_open(&app, "Markdown", pixels, 800, 500))
        return 1;
    draw();
    app_log("[app] Markdown ring3 ready\n");
    for (;;) {
        ArkEvent e;
        bool dirty = false;
        while (app_event(&app, &e)) {
            if (e.type == ARK_EV_DROP) {
                ArkDragRequest drop;
                if (app_drop_read(&app, &e, &drop)) {
                    bool ok = drop.kind == ARK_DRAG_FILE && open_path(drop.data);
                    if (ok)
                        editing = false;
                    app_drop_accept(&app, &drop, ok);
                    dirty = true;
                }
            } else if (e.type == ARK_EV_SCROLL) {
                scroll += e.y;
                if (scroll < 0)
                    scroll = 0;
                dirty = true;
            } else if (e.type == ARK_EV_TEXT && editing) {
                size_t n = strlen(path), cursor = n;
                app_codepoint(path, &n, &cursor, 128, (uint32_t)e.key);
                dirty = true;
            } else if (e.type == ARK_EV_KEY) {
                if (editing) {
                    size_t n = strlen(path);
                    if (e.key == KEY_BACKSPACE && n)
                        path[utf8_prev(path, n)] = 0;
                    else if (e.key == KEY_ENTER) {
                        editing = false;
                        opened();
                    } else if (e.key >= 32 && e.key < 127 && n + 1 < sizeof(path)) {
                        path[n] = (char)e.key;
                        path[n + 1] = 0;
                    }
                } else {
                    if (e.key == KEY_DOWN)
                        scroll++;
                    else if (e.key == KEY_UP && scroll)
                        scroll--;
                    else if (e.key == KEY_PAGE_DOWN)
                        scroll += 8;
                    else if (e.key == KEY_PAGE_UP)
                        scroll = scroll > 8 ? scroll - 8 : 0;
                    else if (e.key == 19)
                        saved();
                    else if (e.key == 'o' || e.key == 'O')
                        opened();
                }
                dirty = true;
            } else if (e.type == ARK_EV_POINTER || e.type == ARK_EV_MOUSE ||
                       e.type == ARK_EV_TOUCH) {
                bool down = (e.buttons & 1) != 0;
                if (app_released(&app, &e)) {
                    if (app_click_hit(&app, &e, 26, 62, app_width(&app) - 303, 37)) {
                        editing = true;
                        path[0] = 0;
                    } else if (app_click_hit(&app, &e, app_width(&app) - 263, 62, 96, 37)) {
                        editing = false;
                        opened();
                    } else if (app_click_hit(&app, &e, app_width(&app) - 154, 62, 128, 37)) {
                        editing = false;
                        saved();
                    } else
                        editing = false;
                    dirty = true;
                }
                pressed = down;
            }
        }
        if (dirty || app.dirty)
            draw();
        app_wait(100);
    }
}
