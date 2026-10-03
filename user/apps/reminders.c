#include "app.h"
#include "drag.h"
#include "text.h"
#include "config.h"
#include "datetime.h"
#include "reminder.h"
static ArkApp app;
static uint32_t pixels[800 * 500];
static ArkReminder items[ARK_REMINDER_LIMIT];
static char keys[ARK_REMINDER_LIMIT][128];
static unsigned count;
static int scroll, field;
static bool held;
static char input[128], date[24], status[128];
static size_t length, cursor, date_length, date_cursor;
static uint64_t revision, next_poll;
static int max(int a, int b) {
    return a > b ? a : b;
}
static void date_default(void) {
    ArkDateTime d = {0};
    if (!app_local_datetime(&d))
        return;
    uint_to_str(d.year, date);
    date[4] = '-';
    date[5] = (char)('0' + d.month / 10);
    date[6] = (char)('0' + d.month % 10);
    date[7] = '-';
    date[8] = (char)('0' + d.day / 10);
    date[9] = (char)('0' + d.day % 10);
    date[10] = ' ';
    date[11] = (char)('0' + d.hour / 10);
    date[12] = (char)('0' + d.hour % 10);
    date[13] = ':';
    date[14] = (char)('0' + d.minute / 10);
    date[15] = (char)('0' + d.minute % 10);
    date[16] = 0;
    date_length = date_cursor = 16;
}
static bool date_parse(int64_t *due) {
    if (strlen(date) != 16 || date[4] != '-' || date[7] != '-' || date[10] != ' ' ||
        date[13] != ':')
        return false;
    for (unsigned i = 0; i < 16; i++)
        if (i != 4 && i != 7 && i != 10 && i != 13 && (date[i] < '0' || date[i] > '9'))
            return false;
    ArkDateTime d = {0};
    for (unsigned i = 0; i < 4; i++)
        d.year = d.year * 10 + date[i] - '0';
    d.month = (date[5] - '0') * 10 + date[6] - '0';
    d.day = (date[8] - '0') * 10 + date[9] - '0';
    d.hour = (date[11] - '0') * 10 + date[12] - '0';
    d.minute = (date[14] - '0') * 10 + date[15] - '0';
    if (d.year < 1970 || d.year > 9999 || d.month < 1 || d.month > 12 || d.day < 1 ||
        d.day > app_month_days(d.year, d.month) || d.hour > 23 || d.minute > 59)
        return false;
    *due = app_epoch(&d) - app_timezone() * 3600;
    return true;
}
static void load(void) {
    count = 0;
    for (unsigned i = 0;; i++) {
        ArkRegistryRequest q = {.op = ARK_REG_LIST, .index = i};
        strcopy(q.key, "/apps/reminders/items/", 128);
        int64_t r = ark_registry(&q);
        revision = q.generation;
        if (r == -2)
            break;
        if (r < 0) {
            strcopy(status, "无法读取提醒事项，请检查系统磁盘", 128);
            break;
        }
        if (q.type != ARK_REG_BINARY || q.length != sizeof(ArkReminder))
            continue;
        ArkReminder item;
        memcpy(&item, q.value, sizeof item);
        if (item.completed > 1 || item.notified > 1 || item.due < 0 || item.title[127] ||
            item.date[23])
            continue;
        if (count == ARK_REMINDER_LIMIT) {
            strcopy(status, "当前窗口最多加载 128 条提醒事项", 128);
            break;
        }
        items[count] = item;
        strcopy(keys[count++], q.key, 128);
    }
    if (scroll > (int)count)
        scroll = 0;
}
static void add(void) {
    if (!length)
        return;
    ArkReminder item = {0};
    if (!date_parse(&item.due)) {
        strcopy(status, "日期格式：年-月-日 时:分，例如 2026-10-03 09:30", 128);
        return;
    }
    strcopy(item.title, input, 128);
    strcopy(item.date, date, 24);
    char key[128] = "/apps/reminders/items/", n[24];
    uint_to_str((uint64_t)item.due, n);
    strcopy(key + strlen(key), n, 128 - strlen(key));
    strcopy(key + strlen(key), "-", 128 - strlen(key));
    uint_to_str((uint64_t)ark_pid() * 100000000 + ark_millis(), n);
    strcopy(key + strlen(key), n, 128 - strlen(key));
    if (app_config_set(key, &item, sizeof item)) {
        input[0] = 0;
        length = cursor = 0;
        strcopy(status, "提醒已保存；到期后会在桌面提示", 128);
        load();
        app_log("[reminders] saved native schedule\n");
    } else
        strcopy(status, "保存失败，请检查系统磁盘", 128);
}
static void draw(void) {
    int w = app_width(&app), h = app_height(&app);
    app_clear(&app, 0xf4f7fb);
    app_cursor(&app, 20, 64, w - 168, 38, ARK_CURSOR_TEXT);
    app_cursor(&app, 20, 112, 258, 34, ARK_CURSOR_TEXT);
    app_text(&app, 24, 18, "提醒事项", 0x283d57, 2);
    app_round(&app, 20, 64, w - 168, 38, 10, 0xffffff);
    app_clip(&app, 31, 74, input[0] ? input : "输入需要提醒的事情", w - 194, 0x314660);
    app_button(&app, w - 138, 64, 114, 38, "添加", true);
    app_round(&app, 20, 112, 258, 34, 9, field == 1 ? 0xd5e5ff : 0xffffff);
    app_text(&app, 31, 119, date, 0x314660, 1);
    app_text(&app, 296, 119, "年-月-日 时:分", 0x6b8199, 1);
    int rows = max(1, (h - 203) / 49);
    scroll = max(0, scroll);
    if (scroll > max(0, (int)count - rows))
        scroll = max(0, (int)count - rows);
    for (unsigned i = (unsigned)scroll; i < count && i < (unsigned)(scroll + rows); i++) {
        int y = 164 + ((int)i - scroll) * 49;
        app_cursor(&app, 20, y, w - 40, 44, ARK_CURSOR_POINTER);
        app_round(&app, 20, y, w - 40, 44, 10, 0xffffff);
        app_text(&app, 31, y + 10, items[i].completed ? "✓" : "○", 0x278e81, 1);
        app_clip(&app, 66, y + 4, items[i].title, w - 125, 0x314660);
        app_text(&app, 66, y + 25, items[i].date, 0x6b8199, 1);
        app_text(&app, w - 49, y + 11, "×", 0xb35563, 1);
    }
    app_clip(&app, 24, h - 29, status[0] ? status : "选择日期与时间 · 点击圆圈完成 · 滚轮查看列表",
             w - 48, 0x6b8199);
    app_text_input(&app, field == 0, 31, 74, 20);
    app_present(&app);
}
int main(void) {
    if (!app_open(&app, "Reminders", pixels, 800, 500))
        return 1;
    date_default();
    load();
    draw();
    app_log("[app] Reminders ring3 ready\n");
    for (;;) {
        ArkEvent e;
        bool dirty = false;
        while (app_event(&app, &e)) {
            if (e.type == ARK_EV_DROP) {
                ArkDragRequest drop;
                if (app_drop_read(&app, &e, &drop)) {
                    bool ok = app_insert(input, &length, &cursor, 128, drop.data);
                    field = 0;
                    app_drop_accept(&app, &drop, ok);
                    dirty = true;
                }
            }
            if (e.type == ARK_EV_NEW) {
                field = 0;
                input[0] = 0;
                length = cursor = 0;
                date_default();
                dirty = true;
            } else if (e.type == ARK_EV_TEXT && field == 0) {
                app_codepoint(input, &length, &cursor, 128, (uint32_t)e.key);
                dirty = true;
            } else if (e.type == ARK_EV_KEY) {
                if (e.key == KEY_ENTER)
                    add();
                else if (e.key == KEY_TAB)
                    field = !field;
                else if (field)
                    app_edit(date, &date_length, &date_cursor, 24, e.key);
                else
                    app_edit(input, &length, &cursor, 128, e.key);
                dirty = true;
            } else if (e.type == ARK_EV_SCROLL) {
                scroll = max(0, scroll + e.y);
                dirty = true;
            } else if (e.type == ARK_EV_POINTER) {
                bool down = (e.buttons & 1) != 0;
                if (app_released(&app, &e)) {
                    int w = app_width(&app);
                    if (app_click_hit(&app, &e, 20, 64, w - 168, 38))
                        field = 0;
                    else if (app_click_hit(&app, &e, 20, 112, 258, 34))
                        field = 1;
                    else if (app_click_hit(&app, &e, w - 138, 64, 114, 38))
                        add();
                    else {
                        int row = scroll + (e.y - 164) / 49;
                        if (e.y >= 164 && row >= 0 && row < (int)count &&
                            app_click_hit(&app, &e, 20, 164 + (row - scroll) * 49, w - 40, 44)) {
                            if (e.x > w - 65 && app.press_x > w - 65) {
                                ArkRegistryRequest q = {.op = ARK_REG_DELETE};
                                strcopy(q.key, keys[row], 128);
                                if (ark_registry(&q) < 0)
                                    strcopy(status, "无法删除提醒", 128);
                            } else {
                                items[row].completed = !items[row].completed;
                                if (!app_config_set(keys[row], &items[row], sizeof items[row]))
                                    strcopy(status, "无法保存更改", 128);
                            }
                            load();
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
            strcopy(q.key, "/apps/reminders/items/", 128);
            if (ark_registry(&q) >= 0 && q.generation != revision) {
                load();
                dirty = true;
            }
        }
        if (dirty || app.dirty)
            draw();
        app_wait(100);
    }
}
