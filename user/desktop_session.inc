/* Native userspace account screens. No password or verifier is put in a shell
 * command, event log, VFS client buffer or public profile. */
#include "ark_api.h"
static void desktop_session_changed(void);
static void session_lock(void);
enum {
    SESSION_SETUP = 0,
    SESSION_OUT = 1,
    SESSION_LOCKED = 2,
    SESSION_ACTIVE = 3,
    SESSION_ERROR = 4
};
typedef struct {
    uint32_t uid, flags;
    char user[24], display[64];
} SessionUser;
static ArkAccountRequest session_info;
static SessionUser session_users[8];
static unsigned session_user_count, session_choice;
static uint64_t session_boot_at, session_now, session_poll_at, session_blink_at;
static bool session_booting = true, session_manager, session_keyboard, session_shift,
            session_new_admin;
static int session_users_window = -1;
static unsigned session_user_scroll, session_form_first;
static unsigned session_form, session_field; /* manager: 0 list, 1 create, 2 password */
static char session_fields[4][96], session_message[128];

static void session_erase(void *data, size_t bytes) {
    volatile unsigned char *p = data;
    while (bytes--)
        *p++ = 0;
}
static void session_clear_fields(void) {
    session_erase(session_fields, sizeof session_fields);
    session_field = 0;
}
static void session_clear_private(void) {
    session_clear_fields();
    session_keyboard = false;
    session_shift = false;
    session_form = 0;
    session_manager = false;
    session_new_admin = false;
}
static void session_window_closed(int wid) {
    if (wid == session_users_window) {
        session_clear_private();
        session_users_window = -1;
    }
}
static bool session_active(void) {
    return session_info.status == SESSION_ACTIVE;
}
static bool session_boot_visible(void) {
    return session_booting;
}
static bool session_overlay_visible(void) {
    return session_booting || !session_active();
}
static void session_ready_log(void) {
    if (session_info.status == SESSION_SETUP)
        serial_write("[session] setup ready\n");
    else if (session_info.status == SESSION_LOCKED)
        serial_write("[session] locked\n");
    else if (session_info.status == SESSION_OUT)
        serial_write("[session] login ready\n");
    else if (session_info.status == SESSION_ERROR)
        serial_write("[session] account storage error\n");
}
static void session_users_refresh(void) {
    session_user_count = 0;
    for (unsigned i = 0; i < session_info.count && i < 8; i++) {
        ArkAccountRequest q;
        memset(&q, 0, sizeof q);
        q.op = ARK_ACCOUNT_LIST;
        q.index = i;
        if (ark_account(&q) < 0)
            break;
        SessionUser *u = &session_users[session_user_count++];
        u->uid = q.uid;
        u->flags = q.flags;
        strcopy(u->user, q.user, sizeof u->user);
        strcopy(u->display, q.display, sizeof u->display);
        session_erase(&q, sizeof q);
    }
    if (session_choice >= session_user_count)
        session_choice = 0;
}
static void session_status(void) {
    ArkAccountRequest q;
    memset(&q, 0, sizeof q);
    q.op = ARK_ACCOUNT_STATUS;
    if (ark_account(&q) < 0) {
        session_info.status = SESSION_ERROR;
        strcopy(session_message, "Account service unavailable", sizeof session_message);
        return;
    }
    unsigned before = session_info.status;
    session_info = q;
    if (before != q.status) {
        session_clear_private();
        session_users_refresh();
        desktop_session_changed();
        if (!session_booting)
            session_ready_log();
        dirty = true;
    }
}
static void session_init(void) {
    memset(&session_info, 0, sizeof session_info);
    session_info.status = SESSION_ERROR;
    memset(session_users, 0, sizeof session_users);
    session_clear_private();
    session_message[0] = 0;
    session_now = session_boot_at = ark_ticks();
    session_poll_at = session_blink_at = session_now;
    session_booting = true;
    session_status();
    session_users_refresh();
    dirty = true;
}
static bool session_call(ArkAccountRequest *q) {
    unsigned before = session_info.status;
    q->status = UINT32_MAX;
    int64_t result = ark_account(q);
    session_erase(q->secret, sizeof q->secret);
    session_erase(q->new_secret, sizeof q->new_secret);
    /* The dispatcher returns authoritative status even for rejected operations. */
    if (q->status <= SESSION_ERROR)
        session_info = *q;
    else {
        session_info.status = SESSION_ERROR;
        strcopy(q->error, "Invalid account service response", sizeof q->error);
    }
    strcopy(session_message, result < 0 ? q->error : "", sizeof session_message);
    session_clear_fields();
    session_users_refresh();
    if (before != session_info.status) {
        session_clear_private();
        desktop_session_changed();
        session_ready_log();
    }
    dirty = true;
    return result >= 0;
}
static bool session_flush(void) {
    if (!session_active())
        return true;
    if (!settings_flush(ark_millis(), true) || !vfs_sync()) {
        strcopy(session_message, "Save failed; try again before signing out.",
                sizeof session_message);
        notice("保存失败，请检查数据磁盘");
        dirty = true;
        return false;
    }
    return true;
}
static void session_lock(void) {
    if (!session_active())
        return;
    bool saved = session_flush();
    ArkAccountRequest q;
    memset(&q, 0, sizeof q);
    q.op = ARK_ACCOUNT_LOCK;
    if (session_call(&q) && !saved)
        strcopy(session_message, "Locked, but disk sync failed; some changes may not be saved.",
                sizeof session_message);
    session_erase(&q, sizeof q);
}
static void session_logout(void) {
    if (!session_flush())
        return;
    ArkAccountRequest q;
    memset(&q, 0, sizeof q);
    q.op = ARK_ACCOUNT_LOGOUT;
    (void)session_call(&q);
    session_erase(&q, sizeof q);
}
static void session_page_changed(int wid, int category, int subpage) {
    bool users = category == 4 && (subpage == 43 || subpage == 46 || subpage == 47);
    if (!users) {
        if (wid == session_users_window) {
            session_manager = false;
            session_keyboard = false;
            session_clear_fields();
        }
        return;
    }
    session_form_first = 0;
    session_manager = true;
    session_users_window = wid;
    session_form = subpage == 46 ? 1 : subpage == 47 ? 2 : 0;
    session_clear_fields();
    session_keyboard = false;
    session_message[0] = 0;
    if (!session_form) {
        session_users_refresh();
        for (unsigned i = 0; i < session_user_count; i++)
            if (session_users[i].uid == session_info.uid)
                session_choice = i;
    }
}
static void session_open_users(void) {
    if (!session_active())
        return;
    open_app(3);
    window_context(focus());
    settings_navigate(4, 43, 1);
    session_page_changed(focus(), 4, 43);
    dirty = true;
}
static void session_window_focused(int wid) {
    if (!session_active())
        return;
    if (wid != session_users_window) {
        session_clear_fields();
        session_keyboard = false;
    }
    if (window_app(wid) == 3 && settings_category == 4 &&
        (settings_subpage == 43 || settings_subpage == 46 || settings_subpage == 47) &&
        (!session_manager || wid != session_users_window))
        session_page_changed(wid, settings_category, settings_subpage);
}
static bool session_users_focused(void) {
    return session_active() && session_manager && focus() == session_users_window &&
           window_app(focus()) == 3 && settings_category == 4 &&
           (settings_subpage == 43 || settings_subpage == 46 || settings_subpage == 47);
}
static bool session_display_editing(void) {
    return session_users_focused() && session_form == 1 && session_field == 1;
}
static void session_display_insert(const char *s) {
    size_t n = strlen(session_fields[1]), bytes = strlen(s);
    if (n + bytes <= 63) {
        memcpy(session_fields[1] + n, s, bytes + 1);
        dirty = true;
    }
}
static void session_choose(int direction) {
    if (!session_user_count)
        return;
    for (unsigned i = 0; i < session_user_count; i++) {
        session_choice = (session_choice + session_user_count +
                          (unsigned)(direction > 0 ? 1 : session_user_count - 1)) %
                         session_user_count;
        if (session_manager || !(session_users[session_choice].flags & 2))
            break;
    }
    session_clear_fields();
    session_message[0] = 0;
    dirty = true;
}
static unsigned session_field_count(void) {
    if (session_manager)
        return session_form == 1 ? 4 : session_form == 2 ? 3 : 0;
    return session_info.status == SESSION_SETUP ? 2 : 1;
}
static bool session_masked(unsigned field) {
    return !(session_manager && session_form == 1 && field < 2);
}
static unsigned session_field_limit(unsigned field) {
    if (session_manager && session_form == 1 && field < 2)
        return field ? 63 : 23;
    return 64;
}
static void session_submit(void) {
    ArkAccountRequest q;
    memset(&q, 0, sizeof q);
    if (session_info.status == SESSION_ERROR)
        return;
    if (session_manager) {
        if (session_form == 1) {
            if (strcmp(session_fields[2], session_fields[3])) {
                strcopy(session_message, "The two passwords do not match", sizeof session_message);
                dirty = true;
                return;
            }
            q.op = ARK_ACCOUNT_CREATE_USER;
            q.flags = session_new_admin ? 1 : 0;
            strcopy(q.user, session_fields[0], sizeof q.user);
            strcopy(q.display, session_fields[1], sizeof q.display);
            strcopy(q.secret, session_fields[2], sizeof q.secret);
        } else if (session_form == 2) {
            if (strcmp(session_fields[1], session_fields[2])) {
                strcopy(session_message, "The two new passwords do not match",
                        sizeof session_message);
                dirty = true;
                return;
            }
            q.op = ARK_ACCOUNT_CHANGE_PASSWORD;
            strcopy(q.secret, session_fields[0], sizeof q.secret);
            strcopy(q.new_secret, session_fields[1], sizeof q.new_secret);
        } else
            return;
    } else if (session_info.status == SESSION_SETUP) {
        if (strcmp(session_fields[0], session_fields[1])) {
            strcopy(session_message, "The two passwords do not match", sizeof session_message);
            dirty = true;
            return;
        }
        q.op = ARK_ACCOUNT_ENROLL;
        strcopy(q.secret, session_fields[0], sizeof q.secret);
    } else if (session_info.status == SESSION_LOCKED) {
        q.op = ARK_ACCOUNT_UNLOCK;
        strcopy(q.secret, session_fields[0], sizeof q.secret);
    } else if (session_info.status == SESSION_OUT && session_user_count) {
        q.op = ARK_ACCOUNT_LOGIN;
        strcopy(q.user, session_users[session_choice].user, sizeof q.user);
        strcopy(q.secret, session_fields[0], sizeof q.secret);
    } else
        return;
    bool managed = session_manager;
    if (session_call(&q) && managed) {
        if (q.op == ARK_ACCOUNT_CREATE_USER)
            serial_write("[account] User record saved\n");
        settings_navigate(4, 43, -1);
        session_form = 0;
        session_keyboard = false;
        strcopy(session_message, "账户更改已保存", sizeof session_message);
    }
    session_erase(&q, sizeof q);
}
static void session_toggle_disabled(void) {
    if (!(session_info.flags & 1) || session_choice >= session_user_count)
        return;
    ArkAccountRequest q;
    memset(&q, 0, sizeof q);
    q.op = ARK_ACCOUNT_SET_DISABLED;
    q.uid = session_users[session_choice].uid;
    q.flags = (session_users[session_choice].flags & 2) ? 0 : 2;
    (void)session_call(&q);
    session_erase(&q, sizeof q);
}
static bool session_key(int key) {
    if (session_booting)
        return true;
    if (session_active() && !session_users_focused()) {
        if (key == 21) {
            session_open_users();
            return true;
        }
        return false;
    }
    if (session_active() &&
        (key == 23 || key == 20 || (key >= KEY_F1 && key <= KEY_F4) || key == KEY_LAUNCHER)) {
        session_clear_fields();
        session_keyboard = false;
        return false;
    }
    if (session_display_editing() &&
        (key == 9 || (ime_enabled && (strlen(ime_composition) || (key >= 'a' && key <= 'z')))))
        return false;
    if (key == 11 && session_active()) {
        session_lock();
        return true;
    }
    if (key == KEY_ESCAPE) {
        session_clear_fields();
        session_message[0] = 0;
        if (session_keyboard)
            session_keyboard = false;
        else if (session_manager) {
            settings_navigate(4, session_form ? 43 : 0, -1);
        }
        dirty = true;
        return true;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        if ((session_manager && !session_form) || session_info.status == SESSION_OUT)
            session_choose(key == KEY_UP ? -1 : 1);
        return true;
    }
    unsigned fields = session_field_count();
    if (!fields)
        return !session_active();
    if (key == KEY_TAB) {
        session_field = (session_field + 1) % fields;
        dirty = true;
        return true;
    }
    if (key == KEY_ENTER) {
        for (unsigned i = 0; i < fields; i++)
            if (!session_fields[i][0]) {
                session_field = i;
                dirty = true;
                return true;
            }
        session_submit();
        return true;
    }
    if (key == KEY_BACKSPACE) {
        size_t n = strlen(session_fields[session_field]);
        if (n)
            session_fields[session_field][utf8_prev(session_fields[session_field], n)] = 0;
    } else if (key >= 32 && key < 127) {
        size_t n = strlen(session_fields[session_field]);
        if (n < session_field_limit(session_field)) {
            session_fields[session_field][n] = (char)key;
            session_fields[session_field][n + 1] = 0;
        }
    }
    dirty = true;
    return true;
}
static void session_bounds(int *x, int *y, int *width, int *height) {
    *width = min(sw - 32, 500);
    *height = 448;
    *height = min(*height, sh - 32);
    *x = (sw - *width) / 2;
    *y = session_keyboard ? 16 : max(16, (sh - *height) / 2);
}
static void session_input(int x, int y, int width, unsigned field, const char *title) {
    bool managed = session_active() && session_manager;
    label(x, y, title, managed ? muted : 0xc5d6ed);
    click_text_region(x, y + 25, width, 42);
    rr(x, y + 25, width, 42, 10, managed ? (night ? 0x253449 : 0xffffff) : 0x071426,
       managed ? 220 : 180);
    stroke(x, y + 25, width, 42, 10,
           session_field == field ? (managed ? accent : 0x8eceff) : (managed ? muted : 0xc2d8ed),
           session_field == field ? 110 : 28);
    char visible[97];
    if (managed && native_window != session_users_window) {
        visible[0] = 0;
    } else if (session_masked(field)) {
        size_t n = strlen(session_fields[field]);
        for (size_t i = 0; i < n; i++)
            visible[i] = '*';
        visible[n] = 0;
    } else
        strcopy(visible, session_fields[field], sizeof visible);
    const char *view = visible;
    while (*view && utf8_width(view) > width - 34)
        utf8_decode(&view);
    label(x + 13, y + 36, view, managed ? ink : 0xf6faff);
    if (session_field == field && ((session_now / 50) & 1) == 0)
        rect(x + 14 + utf8_width(view), y + 36, 1, 20, managed ? ink : 0xffffff);
    session_erase(visible, sizeof visible);
}
static void session_error_draw(int x, int y, int width) {
    if (!session_message[0])
        return;
    const char *p = session_message;
    for (unsigned row = 0; row < 2 && *p; row++) {
        size_t n = utf8_fit(p, width);
        char linebuf[129];
        if (n > 128)
            n = 128;
        memcpy(linebuf, p, n);
        linebuf[n] = 0;
        label(x, y + (int)row * 20, linebuf,
              session_active() ? (night ? 0xffb4a5 : 0xa54743) : 0xffd1bc);
        p += n;
    }
}
static void session_keyboard_draw(void) {
    if (!session_keyboard)
        return;
    int width = min(sw - 24, 760), x = (sw - width) / 2, y = sh - 228;
    rr(x, y, width, 218, 18, 0x102942, 245);
    stroke(x, y, width, 218, 18, 0xffffff, 65);
    const char *rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    int kw = (width - 44) / 10;
    for (unsigned row = 0; row < 4; row++) {
        int n = (int)strlen(rows[row]), left = x + (width - n * (kw + 3)) / 2;
        for (int i = 0; i < n; i++) {
            char c = rows[row][i];
            if (session_shift)
                c = row ? (char)(c - 32) : "!@#$%^&*()"[i];
            char value[2] = {c, 0};
            button(left + i * (kw + 3), y + 13 + (int)row * 39, kw, value, false);
        }
    }
    button(x + 13, y + 171, 92, session_shift ? "abc" : "SHIFT", false);
    button(x + 114, y + 171, width - 358, "SPACE", false);
    button(x + width - 235, y + 171, 70, "DEL", false);
    button(x + width - 157, y + 171, 65, "HIDE", false);
    button(x + width - 84, y + 171, 71, "ENTER", true);
}
static bool session_keyboard_pointer(void) {
    if (!session_keyboard || my < sh - 228)
        return false;
    int width = min(sw - 24, 760), x = (sw - width) / 2, y = sh - 228;
    const char *rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    int kw = (width - 44) / 10;
    for (unsigned row = 0; row < 4; row++) {
        int n = (int)strlen(rows[row]), left = x + (width - n * (kw + 3)) / 2;
        for (int i = 0; i < n; i++)
            if (in(mx, my, left + i * (kw + 3), y + 13 + (int)row * 39, kw, 36)) {
                char c = rows[row][i];
                if (session_shift)
                    c = row ? (char)(c - 32) : "!@#$%^&*()"[i];
                session_key(c);
                return true;
            }
    }
    if (in(mx, my, x + 13, y + 171, 92, 36))
        session_shift = !session_shift;
    else if (in(mx, my, x + 114, y + 171, width - 358, 36))
        session_key(' ');
    else if (in(mx, my, x + width - 235, y + 171, 70, 36))
        session_key(KEY_BACKSPACE);
    else if (in(mx, my, x + width - 157, y + 171, 65, 36))
        session_keyboard = false;
    else if (in(mx, my, x + width - 84, y + 171, 71, 36))
        session_key(KEY_ENTER);
    dirty = true;
    return true;
}
static bool session_users_pointer(bool clicked);
static bool session_pointer(bool pressed) {
    if (session_active())
        return session_users_pointer(pressed);
    if (!pressed || session_booting)
        return true;
    if (session_keyboard_pointer())
        return true;
    int x, y, w, h;
    session_bounds(&x, &y, &w, &h);
    (void)h;
    unsigned fields = session_field_count();
    int start = session_info.status == SESSION_SETUP ? 116 : 158;
    for (unsigned i = 0; i < fields; i++)
        if (in(mx, my, x + 24, y + start + (int)i * 77 + 25, w - 48, 42)) {
            session_field = i;
            if (pointer_touch_mode)
                session_keyboard = true;
            dirty = true;
            return true;
        }
    int submit_y = session_info.status == SESSION_SETUP ? 310 : 260;
    if (in(mx, my, x + 24, y + submit_y, w - 48, 36)) {
        session_submit();
        return true;
    }
    if (session_info.status == SESSION_OUT) {
        if (in(mx, my, x + 24, y + 99, 98, 36))
            session_choose(-1);
        else if (in(mx, my, x + w - 122, y + 99, 98, 36))
            session_choose(1);
    }
    if (in(mx, my, x + 24, y + submit_y + 48, 116, 36))
        session_keyboard = !session_keyboard;
    else if (session_info.status == SESSION_LOCKED &&
             in(mx, my, x + w - 158, y + submit_y + 48, 134, 36))
        session_logout();
    dirty = true;
    return true;
}
static bool session_render(void) {
    if (!session_overlay_visible())
        return false;
    memcpy(canvas, wallpaper, (size_t)sw * sh * 4);
    rr(0, 0, sw, sh, 0, 0x08162b, 145);
    if (session_booting) {
        unsigned elapsed = (unsigned)(platform_millis() - session_boot_at * 10);
        if (elapsed > 300)
            elapsed = 300;
        int size = 68 + (int)elapsed / 30, cx = sw / 2;
        int group_h = size + 23 + 40 + 36 + 4, top = (sh - group_h) / 2, cy = top + size / 2;
        rr(cx - size / 2, cy - size / 2, size, size, 19, 0xffffff, 40 + (int)elapsed / 5);
        text(cx - utf8_width("A") * 3 / 2, cy - 30, "A", 0xffffff, 3);
        text(cx - utf8_width("ArkOS"), top + size + 23, "ArkOS", 0xeef7ff, 2);
        rr(cx - 90, top + group_h - 4, 180, 4, 2, 0xffffff, 35);
        rr(cx - 90, top + group_h - 4, (int)elapsed * 180 / 300, 4, 2, 0xd3eeff, 235);
        return true;
    }
    int x, y, w, h;
    session_bounds(&x, &y, &w, &h);
    rr(x, y, w, h, 25, 0x122b49, 228);
    stroke(x, y, w, h, 25, 0xb9ddff, 65);
    uint32_t saved_ink = ink;
    ink = 0xebf4ff;
    if (session_info.status == SESSION_ERROR) {
        text(x + 24, y + 28, "账户服务不可用", 0xffffff, 2);
        label(x + 24, y + 101, "现有账户记录无法安全读取。", 0xc5d6ed);
        label(x + 24, y + 136, "系统没有自动重置密码或开放桌面。", 0xc5d6ed);
        session_error_draw(x + 24, y + 190, w - 48);
        label(x + 24, y + h - 58, "请检查数据磁盘后重新启动。", 0xc5d6ed);
        ink = saved_ink;
        return true;
    }
    {
        const char *title = session_info.status == SESSION_SETUP    ? "创建你的 ArkOS"
                            : session_info.status == SESSION_LOCKED ? "屏幕已锁定"
                                                                    : "欢迎回来";
        text(x + 24, y + 25, title, 0xffffff, 2);
        const char *display = session_info.status == SESSION_SETUP    ? "Ark · 首位管理员"
                              : session_info.status == SESSION_LOCKED ? session_info.display
                              : session_user_count ? session_users[session_choice].display
                                                   : "请选择本地账户";
        label(x + 24, y + 75, display, 0xc5d6ed);
        if (session_info.status == SESSION_OUT) {
            button(x + 24, y + 99, 98, "上一个", false);
            button(x + w - 122, y + 99, 98, "下一个", false);
            if (session_user_count)
                clip(x + 137, y + 108, session_users[session_choice].user, w - 274, 0xbbd0eb);
        }
        int start = session_info.status == SESSION_SETUP ? 116 : 158;
        session_input(x + 24, y + start, w - 48, 0,
                      session_info.status == SESSION_SETUP ? "设置密码（8-64 字节）" : "输入密码");
        if (session_info.status == SESSION_SETUP)
            session_input(x + 24, y + start + 77, w - 48, 1, "再次输入密码");
        int submit_y = session_info.status == SESSION_SETUP ? 310 : 260;
        button(x + 24, y + submit_y, w - 48,
               session_info.status == SESSION_SETUP    ? "创建账户并进入"
               : session_info.status == SESSION_LOCKED ? "解锁"
                                                       : "登录",
               true);
        button(x + 24, y + submit_y + 48, 116, "触控键盘", false);
        if (session_info.status == SESSION_LOCKED)
            button(x + w - 158, y + submit_y + 48, 134, "切换用户", false);
        session_error_draw(x + 24, y + submit_y + 92, w - 48);
        if (!session_message[0])
            label(x + 24, y + h - 31,
                  (session_info.flags & 0x100) ? "本地账户 · 密码校验不会加密磁盘"
                                               : "临时内存会话 · 重启后账户不会保留",
                  0xaec7e3);
    }
    session_keyboard_draw();
    ink = saved_ink;
    return true;
}
static void session_tick(uint64_t now) {
    session_now = now;
    if (session_booting) {
        dirty = true;
        if (now >= session_boot_at && now - session_boot_at >= 30) {
            session_booting = false;
            session_ready_log();
        }
    }
    if (session_overlay_visible() && now / 50 != session_blink_at / 50)
        dirty = true;
    session_blink_at = now;
    if (now >= session_poll_at && now - session_poll_at >= 100) {
        session_poll_at = now;
        session_status();
    }
}

static void session_users_bounds(Window *w, int *x, int *y, int *width, int *height) {
    int side = clamp(w->w / 4, 220, 270);
    *x = w->x + side + 25;
    *y = w->y + 68;
    *width = w->w - side - 50;
    *height = w->h - 82;
}
static int session_users_rows(int height) {
    return clamp((height - 249) / 46, 1, 8);
}
static unsigned session_form_rows(int height, unsigned form) {
    return (unsigned)clamp((height - (form == 1 ? 246 : 206)) / 72, 1, form == 1 ? 4 : 3);
}
static bool session_disable_allowed(void) {
    if (!(session_info.flags & 1) || session_choice >= session_user_count)
        return false;
    SessionUser *u = &session_users[session_choice];
    if (u->flags & 2)
        return true;
    if (u->uid == session_info.uid)
        return false;
    if (u->flags & 1) {
        unsigned admins = 0;
        for (unsigned i = 0; i < session_user_count; i++)
            if ((session_users[i].flags & 3) == 1)
                admins++;
        if (admins <= 1)
            return false;
    }
    return true;
}
static void session_user_avatar(int x, int y, unsigned index, int size) {
    static const uint32_t colors[] = {0x627ae4, 0xe89b62, 0x49a7a0, 0xb17ad3, 0xdb778a, 0x729faa};
    rr(x, y, size, size, size / 2, colors[index % 6], 255);
    symbol(x + size / 4, y + size / 4, size / 2, ARKUI_SYMBOL_USER, 0xffffff);
}
static void session_users_draw(Window *w) {
    int x, y, width, height;
    session_users_bounds(w, &x, &y, &width, &height);
    unsigned form = settings_subpage == 46 ? 1 : settings_subpage == 47 ? 2 : 0;
    click_region(x + 8, y + 8, 36, 36);
    circle_tint(x + 26, y + 26, 18, night ? 0x768398 : 0xffffff, 100);
    symbol(x + 18, y + 17, 17, ARKUI_SYMBOL_BACK, ink);
    text(x + 61, y + 13, form == 1 ? "新建用户" : form == 2 ? "修改密码" : "用户与账户", ink, 2);
    clip(x + 62, y + 60,
         form == 1   ? "为家人或同事创建自己的本机账户"
         : form == 2 ? "输入当前密码，再设置新密码"
                     : "管理本机账户、登录与安全",
         width - 72, muted);
    if (!form) {
        int rows = session_users_rows(height), yy = y + 94;
        if (session_user_scroll > (unsigned)max(0, (int)session_user_count - rows))
            session_user_scroll = (unsigned)max(0, (int)session_user_count - rows);
        if (session_choice < session_user_scroll)
            session_user_scroll = session_choice;
        else if (session_choice >= session_user_scroll + (unsigned)rows)
            session_user_scroll = session_choice - (unsigned)rows + 1;
        rr(x, yy, width, rows * 46 + 4, 16, night ? 0x3b485c : 0xf2f3f5, 215);
        for (unsigned i = session_user_scroll;
             i < session_user_count && i < session_user_scroll + (unsigned)rows; i++) {
            SessionUser *u = &session_users[i];
            int row = yy + 2 + (int)(i - session_user_scroll) * 46;
            click_region(x + 4, row, width - 8, 44);
            bool chosen = i == session_choice;
            if (chosen)
                rr(x + 4, row, width - 8, 44, 12, accent, night ? 80 : 25);
            session_user_avatar(x + 13, row + 6, i, 32);
            clip(x + 59, row + 2, u->display[0] ? u->display : u->user, width - 250, ink);
            clip(x + 59, row + 23, u->user, width - 250, muted);
            const char *role = (u->flags & 2) ? "已停用" : (u->flags & 1) ? "管理员" : "标准用户";
            label(x + width - 144, row + 13, role, muted);
            if (u->uid == session_info.uid)
                symbol(x + width - 35, row + 12, 18, ARKUI_SYMBOL_CHECK, accent);
        }
        int by = y + height - 130;
        button(x, by, 136, "修改我的密码", false);
        if (session_info.flags & 1) {
            button(x + 148, by, 112, "新建用户", true);
            bool allowed = session_disable_allowed();
            if (allowed)
                button(x + width - 145, by, 145,
                       session_choice < session_user_count &&
                               (session_users[session_choice].flags & 2)
                           ? "启用所选账户"
                           : "停用所选账户",
                       false);
            else {
                rr(x + width - 145, by, 145, 36, 10, night ? 0x64738a : 0xe2e5ea, 65);
                label(x + width - 128, by + 8, "停用所选账户", muted);
            }
        }
        button(x, by + 46, 100, "锁定屏幕", false);
        button(x + 112, by + 46, 100, "退出登录", false);
        session_error_draw(x, y + height - 29, width);
    } else {
        const char *create[] = {"账户名称", "显示名称", "密码", "再次输入密码"},
                   *change[] = {"当前密码", "新密码", "再次输入新密码"};
        unsigned fields = form == 1 ? 4 : 3;
        unsigned rows = session_form_rows(height, form);
        if (session_field < session_form_first)
            session_form_first = session_field;
        else if (session_field >= session_form_first + rows)
            session_form_first = session_field - rows + 1;
        rr(x, y + 92, width, (int)rows * 72 + 10, 16, night ? 0x3b485c : 0xf2f3f5, 205);
        for (unsigned i = session_form_first; i < fields && i < session_form_first + rows; i++)
            session_input(x + 16, y + 98 + (int)(i - session_form_first) * 72, width - 32, i,
                          form == 1 ? create[i] : change[i]);
        int by = y + height - 98;
        if (form == 1) {
            click_region(x + 4, by - 39, width - 8, 30);
            rr(x + 5, by - 36, 23, 23, 6,
               session_new_admin ? accent : (night ? 0x718096 : 0xd5dbe4), 200);
            if (session_new_admin)
                symbol(x + 8, by - 33, 17, ARKUI_SYMBOL_CHECK, 0xffffff);
            label(x + 40, by - 34, "管理员账户", ink);
        }
        button(x + width - 132, by, 132, "保存", true);
        button(x, by, 104, "取消", false);
        button(x + 116, by, 134, "屏幕键盘", false);
        if (session_message[0])
            session_error_draw(x + 4, by + 45, width - 8);
        else
            clip(x + 4, by + 45,
                 form == 1 ? "账户名称使用小写英文；密码为 8–64 个英文字符。"
                           : "密码可以使用英文字母、数字和符号。",
                 width - 8, muted);
    }
}
static bool session_users_pointer(bool clicked) {
    if (!session_users_focused())
        return false;
    if (clicked && session_keyboard && session_keyboard_pointer())
        return true;
    Window *w = &windows[session_users_window];
    int x, y, width, height;
    session_users_bounds(w, &x, &y, &width, &height);
    if (!in(mx, my, x, y, width, height)) {
        if (clicked)
            session_keyboard = false;
        return false;
    }
    if (in(mx, my, x + 8, y + 8, 36, 36))
        return false;
    if (!clicked)
        return false;
    if (!session_form) {
        int rows = session_users_rows(height), yy = y + 96, by = y + height - 130;
        if (in(mx, my, x + 4, yy, width - 8, rows * 46)) {
            unsigned row = session_user_scroll + (unsigned)(my - yy) / 46;
            if (row < session_user_count)
                session_choice = row;
        } else if (in(mx, my, x, by, 136, 36))
            settings_navigate(4, 47, 1);
        else if (in(mx, my, x + 148, by, 112, 36) && (session_info.flags & 1)) {
            session_new_admin = false;
            settings_navigate(4, 46, 1);
        } else if (in(mx, my, x + width - 145, by, 145, 36) && session_disable_allowed())
            session_toggle_disabled();
        else if (in(mx, my, x, by + 46, 100, 36))
            session_lock();
        else if (in(mx, my, x + 112, by + 46, 100, 36))
            session_logout();
    } else {
        unsigned fields = session_field_count(), rows = session_form_rows(height, session_form);
        for (unsigned i = session_form_first; i < fields && i < session_form_first + rows; i++)
            if (in(mx, my, x + 16, y + 123 + (int)(i - session_form_first) * 72, width - 32, 42)) {
                session_field = i;
                if (pointer_touch_mode)
                    session_keyboard = true;
                dirty = true;
                return true;
            }
        int by = y + height - 98;
        if (session_form == 1 && in(mx, my, x + 4, by - 39, width - 8, 30))
            session_new_admin = !session_new_admin;
        if (in(mx, my, x + width - 132, by, 132, 36))
            session_submit();
        else if (in(mx, my, x, by, 104, 36))
            settings_navigate(4, 43, -1);
        else if (in(mx, my, x + 116, by, 134, 36))
            session_keyboard = !session_keyboard;
    }
    dirty = true;
    return true;
}
static bool session_users_scroll(int wid, int delta) {
    if (!session_manager || wid != session_users_window || settings_category != 4 ||
        (settings_subpage != 43 && settings_subpage != 46 && settings_subpage != 47))
        return false;
    int x, y, width, height;
    session_users_bounds(&windows[wid], &x, &y, &width, &height);
    if (session_form) {
        unsigned rows = session_form_rows(height, session_form), fields = session_field_count();
        if (!in(mx, my, x, y + 92, width, (int)rows * 72 + 10))
            return false;
        session_form_first = (unsigned)clamp((int)session_form_first + (delta > 0 ? 1 : -1), 0,
                                             (int)(fields - rows));
        session_field = session_form_first;
        dirty = true;
        return true;
    }
    if (!in(mx, my, x, y + 94, width, session_users_rows(height) * 46))
        return false;
    session_user_scroll =
        (unsigned)clamp((int)session_user_scroll + delta, 0,
                        max(0, (int)session_user_count - session_users_rows(height)));
    session_choice = session_user_scroll;
    dirty = true;
    return true;
}
static void session_password_keyboard_draw(void) {
    if (session_users_focused() && session_keyboard)
        session_keyboard_draw();
}
