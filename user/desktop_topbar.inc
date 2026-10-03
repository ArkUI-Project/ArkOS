/* Compact native menu bar. All status comes from ArkOS devices and session. */
static ArkDateTime bar_time;
static ArkNetworkRequest bar_network;
static char bar_user[64];
static int bar_menu = -1;
static uint64_t bar_refresh_at;
static void topbar_reset(void) {
    bar_refresh_at = 0;
    bar_user[0] = 0;
    bar_menu = -1;
}
static int bar_menu_start(void) {
    int app = focus();
    return 42 + min(160, utf8_width(app >= 0 ? titles[app] : "桌面")) + 18;
}
static int bar_menu_x(int menu) {
    return menu == 0 ? 10 : menu <= 5 ? bar_menu_start() + (menu - 1) * 56 : sw - 338;
}
static int bar_menu_rows(int menu) {
    return menu == 2 ? 3 : menu == 5 ? 3 : menu >= 6 ? 3 : 4;
}
static int bar_month_days(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}
static void bar_refresh(uint64_t now) {
    bar_refresh_at = now + 100;
    bar_time = (ArkDateTime){0};
    ark_call(ARK_SYS_DATETIME, &bar_time, sizeof bar_time);
    if (bar_time.valid) {
        int hour = bar_time.hour + utc_offset, day = hour < 0 ? -1 : hour >= 24 ? 1 : 0;
        bar_time.hour = (hour + 24) % 24;
        bar_time.weekday = (bar_time.weekday + day + 7) % 7;
        bar_time.day += day;
        if (bar_time.day < 1) {
            if (--bar_time.month < 1) {
                bar_time.month = 12;
                bar_time.year--;
            }
            bar_time.day = bar_month_days(bar_time.year, bar_time.month);
        } else if (bar_time.day > bar_month_days(bar_time.year, bar_time.month)) {
            bar_time.day = 1;
            if (++bar_time.month > 12) {
                bar_time.month = 1;
                bar_time.year++;
            }
        }
    }
    bar_network = (ArkNetworkRequest){0};
    ark_network(&bar_network);
    if (!bar_user[0]) {
        ArkAccountRequest q = {.op = ARK_ACCOUNT_STATUS};
        if (ark_account(&q) >= 0)
            strcopy(bar_user, q.display[0] ? q.display : q.user, sizeof bar_user);
    }
}
static void bar_two(char out[3], int n) {
    out[0] = (char)('0' + n / 10);
    out[1] = (char)('0' + n % 10);
    out[2] = 0;
}
static void topbar_draw(void) {
    if (!bar_refresh_at)
        bar_refresh(ark_ticks());
    glass_edge(0, 0, sw, 32, 0, 30, 0);
    rr(0, 0, sw, 32, 0, 0x1a2230, 170);
    uint32_t white = 0xf3f5f8, dim = 0xd0d7e0;
    int app = focus(), start = bar_menu_start();
    click_region(6, 0, 30, 32);
    icon(4, 12, 6, 20);
    clip(42, 6, app >= 0 ? titles[app] : "桌面", 160, white);
    static const char *menus[] = {"文件", "编辑", "视图", "窗口", "帮助"};
    for (int i = 0; i < 5; i++) {
        click_region(start + i * 56 - 8, 0, 49, 32);
        if (bar_menu == i + 1)
            rr(start + i * 56 - 8, 2, 49, 28, 7, white, 22);
        label(start + i * 56, 6, menus[i], white);
    }
    click_region(sw - 460, 0, 32, 32);
    click_region(sw - 417, 0, 32, 32);
    click_region(sw - 378, 0, 36, 32);
    click_region(sw - 341, 0, 116, 32);
    native_island_dot();
    symbol(sw - 454, 6, 20, ARKUI_SYMBOL_NETWORK, bar_network.configured ? white : 0x8190a4);
    symbol(sw - 411, 6, 20, ARKUI_SYMBOL_SETTINGS, white);
    /* Small keyboard geometry is an actual screen-keyboard toggle. */
    stroke(sw - 372, 8, 24, 16, 4, white, keyboard ? 245 : 180);
    for (int yy = 0; yy < 2; yy++)
        for (int xx = 0; xx < 4; xx++)
            rr(sw - 368 + xx * 5, 11 + yy * 5, 2, 2, 1, white, 230);
    symbol(sw - 337, 6, 20, ARKUI_SYMBOL_USER, white);
    clip(sw - 309, 6, bar_user, 78, dim);
    char date[80] = "", n[24], two[3];
    if (bar_time.valid) {
        uint_to_str((unsigned)bar_time.month, n);
        cat(date, sizeof date, n);
        cat(date, sizeof date, "月");
        bar_two(two, bar_time.day);
        cat(date, sizeof date, two);
        cat(date, sizeof date, "日 ");
        static const char *week[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        cat(date, sizeof date, week[bar_time.weekday]);
        cat(date, sizeof date, "  ");
        bar_two(two, bar_time.hour);
        cat(date, sizeof date, two);
        cat(date, sizeof date, ":");
        bar_two(two, bar_time.minute);
        cat(date, sizeof date, two);
        cat(date, sizeof date, ":");
        bar_two(two, bar_time.second);
        cat(date, sizeof date, two);
    } else
        strcopy(date, "时钟不可用", sizeof date);
    label(sw - 18 - utf8_width(date), 6, date, white);
}
static bool bar_enabled(int menu, int row) {
    if (menu == 2)
        return focused_app() == 2;
    if (menu == 4 && row < 3)
        return focus() >= 0;
    return true;
}
static void bar_action(int menu, int row) {
    bar_menu = -1;
    int app = focus();
    if (menu == 0) {
        if (row == 0)
            open_app(4);
        else if (row == 1)
            open_app(16);
        else if (row == 2)
            open_app(3);
        else
            session_lock();
    } else if (menu == 1) {
        if (row == 0)
            new_item();
        else if (row == 1)
            new_window(focused_app() < 0 ? 1 : focused_app());
        else if (row == 2)
            open_app(1);
        else
            save_now();
    } else if (menu == 2) {
        static const int keys[] = {3, 24, 22};
        thirdparty_key(app, keys[row]);
    } else if (menu == 3) {
        if (row == 0)
            native_key(4);
        else if (row == 1)
            launcher_toggle();
        else if (row == 2)
            keyboard_set(!keyboard);
        else {
            open_app(3);
            settings_navigate(1, 0, 0);
        }
    } else if (menu == 4) {
        if (row == 3)
            open_app(10);
        else if (app >= 0) {
            if (row == 0) {
                minimize_window(app);
            } else if (row == 1)
                maximize(app);
            else {
                close_window(app);
            }
        }
    } else if (menu == 5) {
        if (row == 0)
            open_app(4);
        else if (row == 1) {
            open_app(3);
            settings_navigate(8, 0, 0);
        } else {
            open_app(0);
            shell_execute("help");
        }
    } else if (menu == 6) {
        if (row == 0)
            session_open_users();
        else if (row == 1)
            session_lock();
        else
            open_app(3);
    } else if (menu == 7) {
        if (row == 0) {
            open_app(3);
            settings_navigate(5, 0, 0);
        } else if (row == 1)
            open_app(6);
        else
            open_app(10);
    } else {
        if (row == 0)
            open_app(3);
        else if (row == 1)
            open_app(10);
        else
            native_capture(false);
    }
    dirty = true;
}
static void topbar_overlay(void) {
    if (bar_menu < 0)
        return;
    static const char *items[9][4] = {{"关于 ArkOS", "包管理器", "系统设置", "锁定屏幕"},
                                      {"新建…   Ctrl+N", "新建窗口", "打开个人文件", "同步文件"},
                                      {"复制", "剪切", "粘贴"},
                                      {"显示 / 恢复桌面", "所有应用", "屏幕键盘", "显示设置"},
                                      {"最小化", "最大化 / 还原", "关闭窗口", "任务管理器"},
                                      {"关于 ArkOS", "键盘快捷键", "终端命令帮助"},
                                      {"用户与账号", "锁定屏幕", "系统设置"},
                                      {"网络设置", "浏览器", "任务管理器"},
                                      {"系统设置", "任务管理器", "截取屏幕"}};
    int x = clamp(bar_menu_x(bar_menu) - 10, 6, sw - 252), y = 36, rows = bar_menu_rows(bar_menu);
    shadow(x, y, 246, rows * 32 + 12);
    glass_edge(x, y, 246, rows * 32 + 12, 17, 225, 25);
    for (int row = 0; row < rows; row++) {
        click_region(x, 42 + row * 32, 246, 32);
        bool enabled = bar_enabled(bar_menu, row);
        if (enabled && in(mx, my, x + 4, y + 6 + row * 32, 238, 30))
            rr(x + 4, y + 6 + row * 32, 238, 30, 8, accent, 36);
        label(x + 16, y + 12 + row * 32, items[bar_menu][row], enabled ? ink : muted);
    }
}
static bool topbar_pointer(bool pressed) {
    if (session_overlay_visible() || dialog_kind || scene_kind == SCENE_LAUNCHER)
        return false;
    int next = -1, start = bar_menu_start();
    if (my < 32) {
        if (mx < 36)
            next = 0;
        else
            for (int i = 0; i < 5; i++)
                if (in(mx, my, start + i * 56 - 8, 0, 49, 32))
                    next = i + 1;
        if (in(mx, my, sw - 460, 0, 32, 32))
            next = 7;
        else if (in(mx, my, sw - 417, 0, 32, 32))
            next = 8;
        else if (in(mx, my, sw - 341, 0, 116, 32))
            next = 6;
    }
    if (bar_menu >= 0) {
        if (!pressed) {
            static int hover = -1;
            bool changed = false;
            if (next >= 0 && bar_menu != next) {
                bar_menu = next;
                changed = true;
            }
            int x = clamp(bar_menu_x(bar_menu) - 10, 6, sw - 252),
                row = in(mx, my, x, 42, 246, bar_menu_rows(bar_menu) * 32) ? (my - 42) / 32 : -1;
            if (row != hover) {
                hover = row;
                changed = true;
            }
            if (changed)
                dirty = true;
            return true;
        }
        int x = clamp(bar_menu_x(bar_menu) - 10, 6, sw - 252), rows = bar_menu_rows(bar_menu);
        if (in(mx, my, x, 42, 246, rows * 32)) {
            int row = (my - 42) / 32;
            if (bar_enabled(bar_menu, row))
                bar_action(bar_menu, row);
            return true;
        }
        if (my >= 32) {
            bar_menu = -1;
            dirty = true;
            return true;
        }
    }
    if (!pressed || my >= 32)
        return false;
    if (in(mx, my, sw - 378, 0, 36, 32)) {
        bar_menu = -1;
        keyboard_set(!keyboard);
        dirty = true;
        return true;
    }
    bar_menu = bar_menu == next ? -1 : next;
    dirty = true;
    return true;
}
static void topbar_background_region(int y, int x, int *next, int *covered) {
    if (bar_menu < 0)
        return;
    int left = max(0, clamp(bar_menu_x(bar_menu) - 10, 6, sw - 252) - 12);
    int right = min(sw, left + 270), bottom = 36 + bar_menu_rows(bar_menu) * 32 + 32;
    if (y < 32 || y >= bottom)
        return;
    if (x >= left && x < right)
        *covered = max(*covered, right);
    else if (left > x)
        *next = min(*next, left);
}
static bool topbar_key(int key) {
    if (bar_menu < 0)
        return false;
    if (key == KEY_ESCAPE) {
        bar_menu = -1;
        dirty = true;
        return true;
    }
    bar_menu = -1;
    dirty = true;
    return false;
}
static void topbar_tick(uint64_t now) {
    if (!session_active() || session_overlay_visible() || now < bar_refresh_at)
        return;
    bar_refresh(now);
    if (!dirty && scene_kind == SCENE_STILL && bar_menu < 0 && caption_menu < 0) {
        for (int y = 0; y < 32; y++)
            wallpaper_span(y, 0, sw);
        topbar_draw();
        GpuRect damage = {0, 0, sw, 32};
        gpu_present(canvas, (unsigned)sw, &damage);
    } else
        dirty = true;
}
