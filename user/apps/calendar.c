#include "app.h"
#include "drag.h"
#include "text.h"
#include "config.h"
#include "datetime.h"
#define EVENT_LIMIT 256
typedef struct {
    int year, month, day, minutes;
    char title[128];
} CalendarEvent;
static ArkApp app;
static uint32_t pixels[800 * 500];
static CalendarEvent events[EVENT_LIMIT];
static char keys[EVENT_LIMIT][128];
static unsigned count;
static uint64_t revision, next_poll;
static int year, month, day, scroll, selection = -1;
static bool held, editing;
static char input[128], status[128];
static size_t length, cursor;
static bool terminated(const char *s) {
    for (unsigned i = 0; i < 128; i++)
        if (!s[i])
            return true;
    return false;
}
static int min(int a, int b) {
    return a < b ? a : b;
}
static int max(int a, int b) {
    return a > b ? a : b;
}
static int days(int y, int m) {
    static const int n[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return n[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}
static int weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    y -= m < 3;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static void load(void) {
    count = 0;
    for (unsigned i = 0;; i++) {
        ArkRegistryRequest q = {.op = ARK_REG_LIST, .index = i};
        strcopy(q.key, "/apps/calendar/events/", 128);
        int64_t r = ark_registry(&q);
        revision = q.generation;
        if (r == -2)
            break;
        if (r < 0) {
            strcopy(status, "无法读取日程，请检查系统磁盘", 128);
            break;
        }
        if (q.type != ARK_REG_BINARY || q.length != sizeof(CalendarEvent))
            continue;
        CalendarEvent e;
        memcpy(&e, q.value, sizeof e);
        if (e.month < 1 || e.month > 12 || e.day < 1 || e.day > days(e.year, e.month) ||
            e.minutes < 0 || e.minutes >= 1440 || !terminated(e.title))
            continue;
        if (count == EVENT_LIMIT) {
            strcopy(status, "当前窗口最多加载 256 条日程", 128);
            break;
        }
        events[count] = e;
        strcopy(keys[count++], q.key, 128);
    }
}
static void add(void) {
    if (!length)
        return;
    CalendarEvent e = {.year = year, .month = month, .day = day, .minutes = 9 * 60};
    strcopy(e.title, input, 128);
    char key[128] = "/apps/calendar/events/", n[24];
    ArkDateTime now = {0};
    ark_datetime(&now);
    uint64_t stamp = (uint64_t)now.year * 40000000000ull + (uint64_t)now.month * 3000000000ull +
                     (uint64_t)now.day * 86400000 + (uint64_t)now.hour * 3600000 +
                     (uint64_t)now.minute * 60000 + (uint64_t)now.second * 1000 +
                     ark_millis() % 1000;
    uint_to_str(stamp, n);
    strcopy(key + strlen(key), n, 128 - strlen(key));
    strcopy(key + strlen(key), "-", 128 - strlen(key));
    uint_to_str(ark_pid(), n);
    strcopy(key + strlen(key), n, 128 - strlen(key));
    if (app_config_set(key, &e, sizeof e)) {
        input[0] = 0;
        length = cursor = 0;
        editing = false;
        strcopy(status, "日程已保存", 128);
        app_log("[calendar] Native event saved\n");
        load();
    } else
        strcopy(status, "保存失败，请检查系统磁盘", 128);
}
static void draw(void) {
    int w = app_width(&app), h = app_height(&app);
    app_clear(&app, 0xf4f7fb);
    char title[64], n[24];
    uint_to_str(year, title);
    strcopy(title + strlen(title), " 年 ", 64 - strlen(title));
    uint_to_str(month, n);
    strcopy(title + strlen(title), n, 64 - strlen(title));
    strcopy(title + strlen(title), " 月", 64 - strlen(title));
    app_clip(&app, 24, 24, title, w - 262, 0x283d57);
    app_button(&app, w - 222, 14, 52, 36, "‹", false);
    app_button(&app, w - 162, 14, 52, 36, "›", false);
    app_button(&app, w - 102, 14, 78, 36, "今天", false);
    int gridw = w >= 720 ? w - 296 : w * 2 / 3 - 40, cw = max(24, gridw / 7),
        ch = max(38, min(62, (h - 160) / 6)), top = weekday(year, month, 1);
    static const char *names[] = {"日", "一", "二", "三", "四", "五", "六"};
    for (int i = 0; i < 7; i++)
        app_text(&app, 28 + i * cw, 68, names[i], 0x6b8199, 1);
    app_cursor(&app, 20, 98, cw * 7, ch * 6, ARK_CURSOR_POINTER);
    for (int d = 1; d <= days(year, month); d++) {
        int at = top + d - 1, x = 20 + (at % 7) * cw, y = 98 + (at / 7) * ch;
        app_round(&app, x + 2, y, cw - 4, ch - 4, 9, d == day ? 0xd5e5ff : 0xffffff);
        uint_to_str(d, n);
        app_text(&app, x + 10, y + 9, n, 0x283d57, 1);
        for (unsigned i = 0; i < count; i++)
            if (events[i].year == year && events[i].month == month && events[i].day == d) {
                app_round(&app, x + 10, y + ch - 15, 5, 5, 2, 0x2376d6);
                break;
            }
    }
    int x = w >= 720 ? w - 256 : 20 + gridw + 16, y = 74, width = w - x - 24;
    app_text(&app, x, y, "当日日程", 0x283d57, 1);
    app_button(&app, x, y + 28, width, 36, "新建日程", true);
    int row = 0;
    for (unsigned i = 0; i < count; i++)
        if (events[i].year == year && events[i].month == month && events[i].day == day) {
            if (row++ < scroll)
                continue;
            int yy = y + 80 + (row - scroll - 1) * 48;
            if (yy > h - 70)
                break;
            app_round(&app, x, yy, width, 43, 9, (int)i == selection ? 0xd5e5ff : 0xffffff);
            app_clip(&app, x + 9, yy + 5, events[i].title, width - 18, 0x314660);
            app_text(&app, x + 9, yy + 24, "09:00", 0x6b8199, 1);
        }
    if (editing) {
        app_cursor(&app, 20, h - 103, w - 40, 53, ARK_CURSOR_TEXT);
        app_round(&app, 20, h - 103, w - 40, 53, 12, 0xffffff);
        app_clip(&app, 31, h - 89, input[0] ? input : "输入日程名称，Enter 保存", w - 62, 0x314660);
    }
    app_clip(&app, 24, h - 29, status[0] ? status : "选择日期 · Ctrl+N 新建 · Delete 删除选中日程",
             w - 48, 0x6b8199);
    app_text_input(&app, editing, 31, app_height(&app) - 89, 20);
    app_present(&app);
}
int main(void) {
    if (!app_open(&app, "Calendar", pixels, 800, 500))
        return 1;
    ArkDateTime date = {0};
    app_local_datetime(&date);
    year = date.valid ? date.year : 2026;
    month = date.valid ? date.month : 1;
    day = date.valid ? date.day : 1;
    load();
    draw();
    app_log("[app] Calendar ring3 ready\n");
    for (;;) {
        ArkEvent e;
        bool dirty = false;
        while (app_event(&app, &e)) {
            if (e.type == ARK_EV_DROP) {
                ArkDragRequest drop;
                if (app_drop_read(&app, &e, &drop)) {
                    bool ok = app_insert(input, &length, &cursor, 128, drop.data);
                    editing = true;
                    app_drop_accept(&app, &drop, ok);
                    dirty = true;
                }
            }
            if (e.type == ARK_EV_NEW) {
                editing = true;
                input[0] = 0;
                length = cursor = 0;
                dirty = true;
            } else if (e.type == ARK_EV_TEXT && editing) {
                app_codepoint(input, &length, &cursor, 128, (uint32_t)e.key);
                dirty = true;
            } else if (e.type == ARK_EV_KEY) {
                if (editing) {
                    if (e.key == KEY_ENTER)
                        add();
                    else if (e.key == KEY_ESCAPE)
                        editing = false;
                    else
                        app_edit(input, &length, &cursor, 128, e.key);
                } else if (e.key == KEY_DELETE && selection >= 0) {
                    ArkRegistryRequest q = {.op = ARK_REG_DELETE};
                    strcopy(q.key, keys[selection], 128);
                    if (ark_registry(&q) >= 0) {
                        selection = -1;
                        load();
                    } else
                        strcopy(status, "无法删除日程", 128);
                }
                dirty = true;
            } else if (e.type == ARK_EV_SCROLL) {
                scroll = max(0, scroll + e.y);
                dirty = true;
            } else if (e.type == ARK_EV_POINTER) {
                bool down = (e.buttons & 1) != 0;
                if (app_released(&app, &e)) {
                    int w = app_width(&app), h = app_height(&app);
                    if (app_click_hit(&app, &e, w - 222, 14, 52, 36) ||
                        app_click_hit(&app, &e, w - 162, 14, 52, 36)) {
                        month += e.x < w - 162 ? -1 : 1;
                        if (month == 0) {
                            month = 12;
                            year--;
                        }
                        if (month == 13) {
                            month = 1;
                            year++;
                        }
                        day = min(day, days(year, month));
                        scroll = 0;
                        selection = -1;
                    } else if (app_click_hit(&app, &e, w - 102, 14, 78, 36)) {
                        app_local_datetime(&date);
                        if (date.valid) {
                            year = date.year;
                            month = date.month;
                            day = date.day;
                        }
                    } else {
                        int gridw = w >= 720 ? w - 296 : w * 2 / 3 - 40, cw = max(24, gridw / 7),
                            ch = max(38, min(62, (h - 160) / 6)),
                            x = w >= 720 ? w - 256 : 20 + gridw + 16, y = 74, width = w - x - 24;
                        if (app_click_hit(&app, &e, x, y + 28, width, 36)) {
                            editing = true;
                            input[0] = 0;
                            length = cursor = 0;
                        } else if (app_click_hit(&app, &e, 20, 98, cw * 7, ch * 6)) {
                            int d =
                                (e.y - 98) / ch * 7 + (e.x - 20) / cw - weekday(year, month, 1) + 1;
                            int cell = (e.y - 98) / ch * 7 + (e.x - 20) / cw,
                                pressed_cell =
                                    (app.press_y - 98) / ch * 7 + (app.press_x - 20) / cw;
                            if (cell == pressed_cell && d >= 1 && d <= days(year, month)) {
                                day = d;
                                scroll = 0;
                                selection = -1;
                                load();
                            }
                        } else {
                            int row = 0;
                            for (unsigned i = 0; i < count; i++)
                                if (events[i].year == year && events[i].month == month &&
                                    events[i].day == day) {
                                    if (row++ < scroll)
                                        continue;
                                    if (app_click_hit(&app, &e, x, y + 80 + (row - scroll - 1) * 48,
                                                      width, 43))
                                        selection = (int)i;
                                }
                        }
                    }
                    dirty = true;
                }
                held = down;
            }
        }
        uint64_t now = ark_ticks();
        if (now >= next_poll) {
            next_poll = now + 100;
            ArkRegistryRequest q = {.op = ARK_REG_REVISION};
            strcopy(q.key, "/apps/calendar/events/", 128);
            if (ark_registry(&q) >= 0 && q.generation != revision) {
                selection = -1;
                load();
                dirty = true;
            }
        }
        if (dirty || app.dirty)
            draw();
        app_wait(100);
    }
}
