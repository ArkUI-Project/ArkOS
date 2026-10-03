#include "app.h"
#include "drag.h"
#include "text.h"
#include "app_permissions.h"
static ArkApp app;
static uint32_t pixels[800 * 500];
static char todo[12][128], input[128], status[128] = "等待文件访问授权";
static int scroll;
static bool done[12], held, loaded;
static unsigned count;
static size_t length, cursor;
static bool allowed(void) {
    int64_t n = app_request_permission(ARK_CAP_FILES);
    if (n < 0) {
        strcopy(status,
                n == -11 ? "请在系统窗口中授权文件访问" : "文件权限已拒绝；可在系统设置中修改",
                sizeof status);
        return false;
    }
    return true;
}
static void load(void) {
    char b[2048], error[128];
    int n = app_read_text("todo.txt", b, sizeof b, error);
    count = 0;
    if (n >= 0) {
        char *p = b;
        while (*p && count < 12) {
            if ((p[0] == '0' || p[0] == '1') && p[1] == ' ') {
                done[count] = p[0] == '1';
                p += 2;
                unsigned at = 0;
                while (*p && *p != '\n') {
                    if (at < 127)
                        todo[count][at++] = *p;
                    p++;
                }
                todo[count++][at] = 0;
            } else
                while (*p && *p != '\n')
                    p++;
            if (*p)
                p++;
        }
    }
    loaded = true;
    strcopy(status, "每次修改均同步保存到 todo.txt", sizeof status);
}
static void save(void) {
    char b[2048], error[128];
    size_t at = 0;
    for (unsigned i = 0; i < count; i++) {
        b[at++] = done[i] ? '1' : '0';
        b[at++] = ' ';
        size_t n = strlen(todo[i]);
        memcpy(b + at, todo[i], n);
        at += n;
        b[at++] = '\n';
    }
    b[at] = 0;
    if (!app_write_text("todo.txt", b, error))
        strcopy(status, "保存失败：权限、容量或磁盘错误", sizeof status);
    else {
        strcopy(status, "已保存", sizeof status);
        app_log("[todo] Native file saved\n");
    }
}
static void add(void) {
    if (!loaded || !allowed() || !length)
        return;
    if (count == 12) {
        strcopy(status, "最多12条事项，请先删除已完成事项", sizeof status);
        return;
    }
    strcopy(todo[count], input, 128);
    done[count++] = false;
    input[0] = 0;
    length = cursor = 0;
    save();
}
static void draw(void) {
    int w = app_width(&app), h = app_height(&app), rows = (h - 160) / 30;
    if (rows < 1)
        rows = 1;
    if (scroll > (int)count - rows)
        scroll = (int)count - rows;
    if (scroll < 0)
        scroll = 0;
    app_clear(&app, 0xf4f7fb);
    app_cursor(&app, 20, 60, w - 164, 38, ARK_CURSOR_TEXT);
    app_text(&app, 24, 16, "待办事项", 0x283d57, 2);
    app_round(&app, 20, 60, w - 164, 38, 10, 0xffffff);
    app_clip(&app, 31, 70, input[0] ? input : "输入事项，Enter 添加", w - 188, 0x45617d);
    app_button(&app, w - 133, 60, 112, 38, "添加", true);
    for (unsigned i = (unsigned)scroll; i < count && i < (unsigned)(scroll + rows); i++) {
        int y = 112 + ((int)i - scroll) * 30;
        app_cursor(&app, 22, y, w - 44, 27, ARK_CURSOR_POINTER);
        app_round(&app, 22, y, w - 44, 27, 7, 0xffffff);
        app_text(&app, 31, y + 2, done[i] ? "✓" : "○", done[i] ? 0x278e81 : 0x8196ac, 1);
        app_clip(&app, 65, y + 2, todo[i], w - 124, done[i] ? 0x9aaaba : 0x314660);
        app_text(&app, w - 55, y + 2, "×", 0xb35563, 1);
    }
    app_clip(&app, 24, h - 35, status, w - 48, 0x6b8199);
    app_text_input(&app, true, 31, 70, 20);
    app_present(&app);
}
int main(void) {
    if (!app_open(&app, "Todo", pixels, 800, 500))
        return 1;
    app_log("[app] Todo ring3 ready\n");
    draw();
    for (;;) {
        bool dirty = false;
        ArkEvent e;
        while (app_event(&app, &e)) {
            if (e.type == ARK_EV_DROP) {
                ArkDragRequest drop;
                if (app_drop_read(&app, &e, &drop)) {
                    bool ok = app_insert(input, &length, &cursor, sizeof input, drop.data);
                    app_drop_accept(&app, &drop, ok);
                    dirty = true;
                }
            }
            if (e.type == ARK_EV_SCROLL) {
                scroll += e.y;
                if (scroll < 0)
                    scroll = 0;
                dirty = true;
            }
            if (e.type == ARK_EV_TEXT) {
                app_codepoint(input, &length, &cursor, sizeof input, (uint32_t)e.key);
                dirty = true;
            }
            if (e.type == ARK_EV_KEY) {
                if (e.key == KEY_ENTER)
                    add();
                else if (e.key == 19 && allowed())
                    save();
                else
                    app_edit(input, &length, &cursor, sizeof input, e.key);
                dirty = true;
            }
            if (e.type == ARK_EV_POINTER) {
                bool down = (e.buttons & 1) != 0;
                if (app_released(&app, &e)) {
                    if (app_click_hit(&app, &e, app_width(&app) - 133, 60, 112, 38))
                        add();
                    else
                        for (unsigned i = 0; i < count; i++)
                            if (app_click_hit(&app, &e, 22, 112 + ((int)i - scroll) * 30,
                                              app_width(&app) - 44, 27) &&
                                allowed()) {
                                if (e.x >= app_width(&app) - 75 &&
                                    app.press_x >= app_width(&app) - 75) {
                                    for (unsigned j = i; j + 1 < count; j++) {
                                        strcopy(todo[j], todo[j + 1], 128);
                                        done[j] = done[j + 1];
                                    }
                                    count--;
                                } else if (e.x < app_width(&app) - 75 &&
                                           app.press_x < app_width(&app) - 75)
                                    done[i] = !done[i];
                                else
                                    break;
                                save();
                                break;
                            }
                    dirty = true;
                }
                held = down;
            }
        }
        if (!loaded) {
            int64_t n = app_request_permission(ARK_CAP_FILES);
            if (!n) {
                load();
                dirty = true;
            } else if (n == -1 && strcmp(status, "文件访问已拒绝")) {
                strcopy(status, "文件访问已拒绝", sizeof status);
                dirty = true;
            }
        }
        if (dirty || app.dirty)
            draw();
        app_wait(100);
    }
}
