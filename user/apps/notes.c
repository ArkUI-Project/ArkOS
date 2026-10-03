#include "app.h"
#include "drag.h"
#include "text.h"
#include "app_permissions.h"
static uint32_t pixels[800 * 500];
static ArkApp app;
static char text[VFS_FILE_CAP], path[128] = "notes.txt", status[128] = "正在请求文件权限",
                                clipboard[VFS_FILE_CAP];
static int drag_x, drag_y;
static bool drag_pending;
static int first_row;
static bool reveal = true;
static size_t length, cursor;
static bool loaded, changed, held;
static void load(void) {
    char error[128];
    int n = app_read_text(path, text, sizeof text, error);
    if (n < 0) {
        text[0] = 0;
        n = 0;
        strcopy(status, "新文档；Ctrl+S 保存。文件不存在或无法读取。", sizeof status);
    } else
        strcopy(status, "文件已打开", sizeof status);
    length = cursor = (size_t)n;
    loaded = true;
    changed = false;
}
static void save(void) {
    int64_t allowed = app_request_permission(ARK_CAP_FILES);
    if (allowed < 0) {
        strcopy(status,
                allowed == -11 ? "请在系统授权窗口允许文件访问" : "文件权限已拒绝；可在设置中修改",
                sizeof status);
        return;
    }
    char error[128];
    if (app_write_text(path, text, error)) {
        changed = false;
        strcopy(status, "已保存", sizeof status);
        app_log("[notes] File saved through private user API\n");
    } else
        strcopy(status, error[0] ? error : "保存失败", sizeof status);
}
static void open_argument(void) {
    ArkLaunchInfo q = {0};
    if (app_launch_info(&q) >= 0 && q.argument[0]) {
        if (!strcmp(q.argument, "new:")) {
            char number[24];
            uint_to_str(ark_pid(), number);
            strcopy(path, "未命名-", 128);
            strcopy(path + strlen(path), number, 128 - strlen(path));
            strcopy(path + strlen(path), ".txt", 128 - strlen(path));
            text[0] = 0;
            length = cursor = 0;
            first_row = 0;
            reveal = true;
            loaded = true;
            changed = false;
            strcopy(status, "新文档 · Ctrl+S 保存", 128);
            return;
        }
        strcopy(path, q.argument, sizeof path);
    }
    loaded = false;
}
static void draw(void) {
    int w = app_width(&app), h = app_height(&app), wrap = w - 64, rows = (h - 164) / 24;
    if (rows < 1)
        rows = 1;
    app_clear(&app, 0xf4f7fb);
    app_clip(&app, 22, 18, path, w - 160, 0x2469ce);
    app_button(&app, w - 126, 12, 104, 36, "保存", true);
    app_round(&app, 16, 64, w - 32, h - 129, 12, 0xffffff);
    app_cursor(&app, 16, 64, w - 32, h - 129, ARK_CURSOR_TEXT);
    int row = 0, col = 0, caret_row = 0, caret_col = 0;
    const char *p = text;
    for (size_t i = 0; i <= length;) {
        if (i == cursor) {
            caret_row = row;
            caret_col = col;
        }
        if (i == length)
            break;
        p = text + i;
        int cp = utf8_decode(&p);
        i = (size_t)(p - text);
        if (cp == '\n') {
            row++;
            col = 0;
        } else {
            int adv = cp < 128 ? 8 : 16;
            if (col + adv > wrap) {
                row++;
                col = 0;
            }
            col += adv;
        }
    }
    if (first_row > row)
        first_row = row;
    if (reveal) {
        if (caret_row < first_row)
            first_row = caret_row;
        if (caret_row >= first_row + rows)
            first_row = caret_row - rows + 1;
        reveal = false;
    }
    row = col = 0;
    p = text;
    while (*p) {
        const char *begin = p;
        int cp = utf8_decode(&p);
        if (cp == '\n') {
            row++;
            col = 0;
            continue;
        }
        int adv = cp < 128 ? 8 : 16;
        if (col + adv > wrap) {
            row++;
            col = 0;
        }
        if (row >= first_row && row < first_row + rows) {
            char b[8];
            size_t n = (size_t)(p - begin);
            memcpy(b, begin, n);
            b[n] = 0;
            app_text(&app, 30 + col, 80 + (row - first_row) * 24, b, 0x273b55, 1);
        }
        col += adv;
    }
    bool caret = caret_row >= first_row && caret_row < first_row + rows;
    if (caret)
        app_rect(&app, 30 + caret_col, 80 + (caret_row - first_row) * 24, 2, 20, 0x297be0);
    app_clip(&app, 22, h - 49, status, w - 44, 0x6b7e97);
    app_clip(&app, 22, h - 24,
             changed ? "尚未保存 · Ctrl+S 保存 · Ctrl+I 拼音"
                     : "Ctrl+S 保存 · Ctrl+I 拼音 · 滚轮浏览",
             w - 44, 0x6b7e97);
    app_text_input(&app, true, caret ? 30 + caret_col : 30,
                   caret ? 80 + (caret_row - first_row) * 24 : 80, 20);
    app_present(&app);
}
int main(void) {
    if (!app_open(&app, "Notes", pixels, 800, 500))
        return 1;
    open_argument();
    app_log("[app] Notes ring3 ready\n");
    draw();
    for (;;) {
        bool dirty = false;
        ArkEvent e;
        while (app_poll_event(&app, &e)) {
            if (e.type == ARK_EV_CLOSE) {
                if (changed)
                    save();
                if (!changed) {
                    app_close(&app);
                    return 0;
                }
                dirty = true;
            }
            if (e.type == ARK_EV_DROP) {
                ArkDragRequest drop;
                if (app_drop_read(&app, &e, &drop)) {
                    bool accepted = false;
                    if (drop.kind == ARK_DRAG_FILE) {
                        if (changed)
                            save();
                        if (!changed) {
                            char incoming[VFS_FILE_CAP], error[128];
                            int n = app_read_text(drop.data, incoming, sizeof incoming, error);
                            if (n >= 0) {
                                strcopy(path, drop.data, 128);
                                memcpy(text, incoming, (size_t)n + 1);
                                length = cursor = (size_t)n;
                                loaded = true;
                                first_row = 0;
                                reveal = true;
                                accepted = true;
                                strcopy(status, "文件已打开", 128);
                            } else
                                strcopy(status, error, 128);
                        }
                    } else if (drop.kind == ARK_DRAG_TEXT) {
                        accepted = app_insert(text, &length, &cursor, sizeof text, drop.data);
                        changed |= accepted;
                    }
                    app_drop_accept(&app, &drop, accepted);
                    dirty = true;
                }
            }
            if (e.type == ARK_EV_SCROLL) {
                first_row += e.y;
                if (first_row < 0)
                    first_row = 0;
                dirty = true;
            }
            if (e.type == ARK_EV_DRAG_END) {
                held = drag_pending = false;
            }
            if (e.type == ARK_EV_TEXT) {
                reveal = true;
                changed |= app_codepoint(text, &length, &cursor, sizeof text, (uint32_t)e.key);
                dirty = true;
            }
            if (e.type == ARK_EV_OPEN) {
                if (changed)
                    save();
                if (!changed)
                    open_argument();
                else
                    strcopy(status, "尚未保存，已保留当前文档；保存后再打开其他文件",
                            sizeof status);
                dirty = true;
            }
            if (e.type == ARK_EV_KEY) {
                int k = e.key;
                reveal = true;
                if (k == 19)
                    save();
                else if (k == 3 || k == 24) {
                    strcopy(clipboard, text, sizeof clipboard);
                    if (k == 24) {
                        text[0] = 0;
                        length = cursor = 0;
                        changed = true;
                    }
                } else if (k == 22) {
                    changed |= app_insert(text, &length, &cursor, sizeof text, clipboard);
                } else {
                    bool edit = app_edit(text, &length, &cursor, sizeof text, k);
                    if (edit && (k == KEY_BACKSPACE || k == KEY_DELETE || k == KEY_ENTER ||
                                 (k >= 32 && k < 127)))
                        changed = true;
                }
                dirty = true;
            }
            if (e.type == ARK_EV_POINTER) {
                if (app_click_hit(&app, &e, app_width(&app) - 126, 12, 104, 36)) {
                    save();
                    dirty = true;
                }
                bool down = (e.buttons & 1) != 0;
                if (down && !held) {
                    if (e.y >= 64 && e.y < app_height(&app) - 65 && length) {
                        drag_pending = true;
                        drag_x = e.x;
                        drag_y = e.y;
                    }
                } else if (down && drag_pending) {
                    int dx = e.x - drag_x, dy = e.y - drag_y;
                    if (dx * dx + dy * dy > 144) {
                        char data[512];
                        size_t bytes = length < 511 ? length : 511;
                        while (bytes && ((unsigned char)text[bytes] & 0xc0) == 0x80)
                            bytes--;
                        memcpy(data, text, bytes);
                        data[bytes] = 0;
                        (void)app_drag_begin(&app, ARK_DRAG_TEXT, data, "text/plain");
                        drag_pending = false;
                    }
                }
                if (!down)
                    drag_pending = false;
                held = down;
            }
        }
        if (!loaded) {
            int64_t r = app_request_permission(ARK_CAP_FILES);
            if (r == 0) {
                if (changed)
                    loaded = true;
                else
                    load();
                dirty = true;
            } else if (r == -1 && strcmp(status, "文件访问已拒绝；仍可编辑，保存需授权")) {
                strcopy(status, "文件访问已拒绝；仍可编辑，保存需授权", sizeof status);
                dirty = true;
            }
        }
        if (dirty || app.dirty)
            draw();
        app_wait(100);
    }
}
