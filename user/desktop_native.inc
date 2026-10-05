/* Native desktop utilities. Only ArkOS syscalls are used at runtime. */
#include "capture.h"
static int desktop_x[6], desktop_y[6], desktop_drag = -1, desktop_selected = -1, desktop_dx,
                                       desktop_dy;
static const int desktop_apps[6] = {1, 12, 2, 13, 11, 14};
static const char *desktop_names[6] = {"个人文件", "安装 ArkOS", "文本编辑",
                                       "待办事项", "屏幕录制",   "计时器"};
static bool desktop_moving, context_visible, island_expanded, desktop_hidden;
static bool *hidden_windows;
static int context_x, context_y, context_kind, context_item, context_files_window = -1;
static char context_file[128];
static bool context_target_writable, context_folder_writable;
static uint64_t last_icon_click;
static ArkUIAnimation island_animation;
#define ISLAND_W 340
#define ISLAND_H 142
static uint32_t island_pixels[ISLAND_W * ISLAND_H], island_under[ISLAND_W * ISLAND_H];
static bool island_texture_valid;
static uint64_t island_texture_second;
static uint8_t capture_data[ARK_BLOB_MAX];
static size_t capture_size;
static unsigned capture_frames, capture_serial;
static bool recording;
static uint64_t capture_next, capture_started;
static char capture_status[128] = "截取屏幕或录制短片，文件会保存在你的账户中。";
static ArkBlobRequest capture_files[16];
static unsigned capture_count;
static ArkInstallRequest install_disks[2], install_state;
static unsigned install_choice;
static char install_confirm[8];
static bool install_encrypt, install_compress;
static unsigned install_field; /* 0=ERASE, 1=pass, 2=pass2 */
static char install_pass[64], install_pass2[64];
static ArkActivityRequest external_activity;
static uint64_t activity_refresh;
static int native_island_anchor(void) {
    return sw - 477;
}
static bool reminder_pending;
static char reminder_title[128];
static bool shelf_present;
static ArkDragRequest shelf_item, transfer;
static bool native_island_needed(void) {
    return recording || install_state.state == 1 || external_activity.active || shelf_present ||
           transfer.active || reminder_pending;
}

static unsigned permission_selected;
static bool permission_values[4];
static void native_capture_list(void) {
    capture_count = 0;
    for (unsigned i = 0; i < 16; i++) {
        ArkBlobRequest q = {0};
        q.op = ARK_BLOB_LIST;
        q.index = i;
        if (ark_call(ARK_SYS_BLOB, &q, sizeof q) < 0)
            break;
        capture_files[capture_count++] = q;
    }
}
static void native_store_capture(const char *extension) {
    ArkBlobRequest q = {0};
    q.op = ARK_BLOB_WRITE;
    q.buffer = (uintptr_t)capture_data;
    q.capacity = (uint32_t)capture_size;
    char n[24];
    strcopy(q.name, "Capture-", sizeof q.name);
    uint_to_str(ark_ticks(), n);
    cat(q.name, sizeof q.name, n);
    cat(q.name, sizeof q.name, "-");
    uint_to_str(++capture_serial, n);
    cat(q.name, sizeof q.name, n);
    cat(q.name, sizeof q.name, extension);
    int64_t r = ark_call(ARK_SYS_BLOB, &q, sizeof q);
    strcopy(capture_status,
            r >= 0     ? q.name
            : r == -28 ? "空间或目录已满，请删除旧录制"
                       : "无法保存：需要可写的系统磁盘",
            sizeof capture_status);
    if (r >= 0) {
        serial_write("[capture] Native file saved: ");
        serial_write(q.name);
        serial_write("\n");
    }
    notice(capture_status);
    native_capture_list();
    dirty = true;
}
static void native_capture(bool video) {
    if (!session_active() || session_overlay_visible())
        return;
    if (video) {
        if (recording) {
            recording = false;
            if (capture_frames) {
                capture_data[capture_size++] = 0x3b;
                native_store_capture(".gif");
            }
            return;
        }
        capture_size = capture_gif_begin(capture_data, sizeof capture_data, 512, 320);
        if (!capture_size)
            return;
        recording = true;
        capture_frames = 0;
        capture_started = capture_next = ark_ticks();
        strcopy(capture_status, "录制中 · 512×320 · 目标 5 fps · 最长 6 秒", sizeof capture_status);
        dirty = true;
    } else {
        if (recording) {
            notice("请先停止录屏");
            return;
        }
        capture_size =
            capture_bmp(capture_data, sizeof capture_data, canvas, (unsigned)sw, (unsigned)sh);
        if (capture_size)
            native_store_capture(".bmp");
    }
}
static void native_layout_save(void) {
    char data[256] = "", n[24];
    for (unsigned i = 0; i < 6; i++) {
        uint_to_str((unsigned)desktop_x[i], n);
        cat(data, sizeof data, n);
        cat(data, sizeof data, " ");
        uint_to_str((unsigned)desktop_y[i], n);
        cat(data, sizeof data, n);
        cat(data, sizeof data, "\n");
    }
    int f = vfs_find(".desktop");
    if (f < 0)
        f = vfs_create(".desktop");
    if (f >= 0)
        vfs_write(f, data);
}
static unsigned native_number(const char **p) {
    unsigned n = 0;
    for (unsigned i = 0; i < 6 && **p >= '0' && **p <= '9'; i++) {
        n = n * 10 + (unsigned)(*(*p)++ - '0');
    }
    return n;
}
static void native_reset(void) {
    topbar_reset();
    package_ui_reset();
    island_texture_valid = false;
    island_texture_second = 0;
    recording = false;
    capture_size = 0;
    capture_frames = 0;
    capture_count = 0;
    memset(capture_data, 0, sizeof capture_data);
    memset(&external_activity, 0, sizeof external_activity);
    activity_refresh = 0;
    context_visible = false;
    desktop_drag = -1;
    desktop_hidden = false;
    island_expanded = false;
    arkui_animation_init(&island_animation, ARKUI_TRANSITION_ISLAND, false);
    for (unsigned i = 0; i < 6; i++) {
        desktop_x[i] = 22 + (int)(i / 4) * 100;
        desktop_y[i] = 66 + (int)(i % 4) * 108;
    }
    if (!session_active())
        return;
    int f = vfs_find(".desktop");
    if (f >= 0) {
        const char *p = vfs_files[f].data;
        for (unsigned i = 0; i < 6 && *p; i++) {
            int x = (int)native_number(&p);
            if (*p != ' ')
                break;
            p++;
            int y = (int)native_number(&p);
            if (*p != '\n')
                break;
            p++;
            desktop_x[i] = clamp(x, 6, sw - 94);
            desktop_y[i] = clamp(y, 42, sh - 204);
        }
    }
    native_capture_list();
}
static void native_open(int app) {
    if (app == 16)
        package_ui_refresh();
    if (app == 11)
        native_capture_list();
    if (app == 12) {
        install_confirm[0] = 0;
        install_pass[0] = install_pass2[0] = 0;
        install_encrypt = install_compress = false;
        install_field = 0;
        for (unsigned i = 0; i < 2; i++) {
            install_disks[i] = (ArkInstallRequest){0};
            install_disks[i].disk = i;
            ark_call(ARK_SYS_INSTALL, &install_disks[i], sizeof install_disks[i]);
        }
    }
}
static void native_desktop_draw(void) {
    for (unsigned i = 0; i < 6; i++) {
        int x = desktop_x[i], y = desktop_y[i];
        if (desktop_selected == (int)i)
            rr(x - 5, y - 5, 91, 96, 13, 0xbde1ff, 65);
        icon(desktop_apps[i], x + 17, y + 3, 48);
        label(x + 41 - utf8_width(desktop_names[i]) / 2, y + 62, desktop_names[i], 0xf5faff);
    }
}
static bool native_context_enabled(int row) {
    if (context_kind != 2)
        return row != 0 || context_kind == 1;
    if (row == 0)
        return context_file[0] && vfs_find(context_file) >= 0;
    if (row >= 3)
        return context_file[0] && vfs_find(context_file) >= 0 && context_target_writable;
    return context_folder_writable;
}
static void native_context_action(int row) {
    if (!native_context_enabled(row))
        return;
    context_visible = false;
    if (context_kind == 2) {
        if (context_files_window < 0 || !windows[context_files_window].visible)
            return;
        window_context(context_files_window);
        selected = context_file[0] ? vfs_find(context_file) : -1;
        if (row == 0)
            load_note(selected);
        else if (row == 1)
            dialog_open(1, "新建文件.txt");
        else if (row == 2)
            dialog_open(2, "新建文件夹");
        else if (row == 3 && selected >= 0)
            dialog_open(3, base(vfs_entry(selected)->name));
        else if (row == 4 && selected >= 0)
            dialog_open(5, "");
    } else {
        if (row == 0) {
            if (context_kind == 1)
                open_app(desktop_apps[context_item]);
            else {
                files_open_path(user_home);
            }
        } else if (row == 1) {
            for (unsigned i = 0; i < 6; i++) {
                desktop_x[i] = 22 + (int)(i / 4) * 100;
                desktop_y[i] = 66 + (int)(i % 4) * 108;
            }
            native_layout_save();
        } else if (row == 2) {
            open_app(3);
            settings_category = 0;
            settings_subpage = settings_scroll = 0;
        } else if (row == 3)
            native_capture(false);
        else if (row == 4)
            launcher_toggle();
    }
    dirty = true;
}
static bool native_pointer(bool pressed, bool down, bool moved, bool right_pressed, bool clicked) {
    if (dialog_kind || session_overlay_visible() || scene_kind == SCENE_LAUNCHER)
        return false;
    if (right_pressed) {
        motion_cancel();
        desktop_drag = -1;
        context_visible = true;
        context_x = clamp(mx, 4, sw - 234);
        context_y = clamp(my, 40, sh - 194);
        context_kind = 0;
        context_item = 0;
        context_file[0] = 0;
        context_files_window = -1;
        for (int i = WINS - 1; i >= 0; i--) {
            int a = order[i];
            Window *w = &windows[a];
            if (w->visible && in(mx, my, w->x, w->y, w->w, w->h)) {
                if (window_app(a) == 1) {
                    context_kind = 2;
                    activate(a);
                    window_context(a);
                    context_files_window = a;
                    context_folder_writable = extfs_path_writable(folder);
                    context_target_writable = false;
                    selected = -1;
                    int row = (my - w->y - 129) / 40 + file_scroll;
                    if (mx >= w->x + 193 && my >= w->y + 129 && my < w->y + w->h - 47 && row >= 0 &&
                        row < file_count) {
                        selected = file_list[row];
                        strcopy(context_file, vfs_entry(selected)->name, 128);
                        context_target_writable = extfs_path_writable(context_file);
                    }
                } else {
                    context_visible = false;
                    return true;
                }
                break;
            }
        }
        if (!context_kind)
            for (unsigned i = 0; i < 6; i++)
                if (in(mx, my, desktop_x[i] - 5, desktop_y[i] - 5, 91, 96)) {
                    context_kind = 1;
                    context_item = (int)i;
                    desktop_selected = (int)i;
                    break;
                }
        dirty = true;
        return true;
    }
    if (context_visible) {
        if (clicked) {
            if (in(mx, my, context_x, context_y, 230, 190))
                native_context_action((my - context_y) / 38);
            else {
                context_visible = false;
                dirty = true;
            }
        }
        return true;
    }
    if (desktop_drag >= 0) {
        if (down) {
            if (moved) {
                desktop_x[desktop_drag] = clamp(mx - desktop_dx, 6, sw - 94);
                desktop_y[desktop_drag] = clamp(my - desktop_dy, 42, sh - 204);
                desktop_moving = true;
                dirty = true;
            }
        } else {
            if (desktop_moving) {
                native_layout_save();
                serial_write("[desktop] Icon layout saved\n");
            } else {
                uint64_t now = ark_ticks();
                if (desktop_selected == desktop_drag && now - last_icon_click < 35) {
                    open_app(desktop_apps[desktop_drag]);
                    last_icon_click = 0;
                } else
                    last_icon_click = now;
            }
            desktop_selected = desktop_drag;
            desktop_drag = -1;
        }
        return true;
    }
    if (!pressed || my < 40 || my > sh - 110)
        return false;
    for (int i = WINS - 1; i >= 0; i--) {
        Window *w = &windows[order[i]];
        if (w->visible && in(mx, my, w->x, w->y, w->w, w->h))
            return false;
    }
    for (unsigned i = 0; i < 6; i++)
        if (in(mx, my, desktop_x[i] - 5, desktop_y[i] - 5, 91, 96)) {
            motion_cancel();
            desktop_drag = (int)i;
            desktop_dx = mx - desktop_x[i];
            desktop_dy = my - desktop_y[i];
            desktop_moving = false;
            dirty = true;
            return true;
        }
    desktop_selected = -1;
    return false;
}
static const char *native_activity(void) {
    return reminder_pending           ? "提醒事项"
           : shelf_present            ? "暂存区"
           : transfer.active          ? "拖到这里暂存"
           : recording                ? "正在录制屏幕"
           : install_state.state == 1 ? "正在安装 ArkOS"
                                      : external_activity.title;
}
static int native_activity_app(void) {
    return reminder_pending           ? 18
           : recording                ? 11
           : install_state.state == 1 ? 12
                                      : (int)external_activity.app;
}
static void native_island_dot(void) {
    if (!native_island_needed())
        return;
    int x = native_island_anchor();
    uint32_t color = recording ? 0xff7c8e : 0x9ee7d2;
    rr(x - 8, 8, 16, 16, 8, color, 25);
    circle(x, 16, 4, color);
}
static void native_island_texture(void) {
    int x = native_island_anchor() - ISLAND_W / 2, y = 43;
    for (int row = 0; row < ISLAND_H; row++)
        memcpy(island_under + row * ISLAND_W, canvas + (y + row) * sw + x, ISLAND_W * 4);
    glass_edge(x, y, ISLAND_W, ISLAND_H, 34, 25, 0);
    rr(x, y, ISLAND_W, ISLAND_H, 34, 0x0d1624, 240);
    stroke(x, y, ISLAND_W, ISLAND_H, 34, 0xb6c4d6, 20);
    circle(x + 23, y + 25, 4, recording ? 0xff7c8e : 0x9ee7d2);
    clip(x + 40, y + 14, native_activity(), ISLAND_W - 60, 0xf0f4f9);
    char detail[96] = "", n[24];
    unsigned progress = 0;
    if (reminder_pending) {
        strcopy(detail, reminder_title, sizeof detail);
    } else if (shelf_present) {
        strcopy(detail, shelf_item.kind == ARK_DRAG_FILE ? base(shelf_item.data) : shelf_item.data,
                sizeof detail);
    } else if (recording) {
        uint_to_str(capture_frames, n);
        cat(detail, sizeof detail, n);
        cat(detail, sizeof detail, " / 30 帧 · 512×320 GIF");
        progress = capture_frames * 255 / 30;
    } else if (install_state.state == 1) {
        strcopy(detail, install_state.message, sizeof detail);
        if (install_state.total)
            progress = (unsigned)((uint64_t)install_state.done * 255 / install_state.total);
    } else {
        uint64_t now = ark_ticks(), seconds = external_activity.deadline > now
                                                  ? (external_activity.deadline - now + 99) / 100
                                                  : 0;
        strcopy(detail, "剩余 ", sizeof detail);
        uint_to_str(seconds / 60, n);
        cat(detail, sizeof detail, n);
        cat(detail, sizeof detail, " 分 ");
        uint_to_str(seconds % 60, n);
        cat(detail, sizeof detail, n);
        cat(detail, sizeof detail, " 秒");
    }
    clip(x + 20, y + 46, detail, ISLAND_W - 40, 0xaebdd0);
    rr(x + 20, y + 75, ISLAND_W - 40, 4, 2, 0xffffff, 25);
    if (progress)
        rr(x + 20, y + 75, (ISLAND_W - 40) * (int)progress / 255, 4, 2, 0x9ee7d2, 235);
    rr(x + 16, y + 96, 146, 32, 12, 0xaecbdf, 35);
    rr(x + 178, y + 96, 146, 32, 12, 0xffffff, 12);
    label(x + 57, y + 102, shelf_present ? "打开项目" : "打开应用", 0xf0f4f9);
    label(x + 207, y + 102,
          reminder_pending ? "知道了"
          : shelf_present  ? "清空暂存"
                           : "收起",
          0xbbcadb);
    for (int row = 0; row < ISLAND_H; row++) {
        memcpy(island_pixels + row * ISLAND_W, canvas + (y + row) * sw + x, ISLAND_W * 4);
        memcpy(canvas + (y + row) * sw + x, island_under + row * ISLAND_W, ISLAND_W * 4);
    }
    island_texture_valid = true;
    island_texture_second = ark_ticks() / 100;
}
static void native_overlay_draw(void) {
    int p = arkui_animation_progress(&island_animation);
    if (p) {
        if (native_island_needed())
            native_island_texture();
        if (island_texture_valid) {
            int w = motion_lerp(10 * 65536, ISLAND_W * 65536, p),
                h = motion_lerp(10 * 65536, ISLAND_H * 65536, p);
            int x = native_island_anchor() * 65536 - w / 2,
                y = motion_lerp(11 * 65536, 43 * 65536, p);
            motion_blit(canvas, sw, sh, island_pixels, ISLAND_W, ISLAND_H, x, y, w, h,
                        (unsigned)(255 * p / MOTION_ONE), min(34, h / 131072));
        }
    }
    if (context_visible) {
        shadow(context_x, context_y, 230, 190);
        glass(context_x, context_y, 230, 190, 15, 235);
        static const char *desktop_menu[] = {"打开", "整理桌面图标", "个性化设置", "截取屏幕",
                                             "所有应用"};
        static const char *files_menu[] = {"打开", "新建文件", "新建文件夹", "重命名", "删除…"};
        for (int i = 0; i < 5; i++) {
            bool enabled = native_context_enabled(i);
            click_region(context_x, context_y + i * 38, 230, 38);
            if (!enabled && click_region_count)
                click_regions[click_region_count - 1].shape = ARK_CURSOR_ARROW;
            if (enabled && in(mx, my, context_x, context_y + i * 38, 230, 38))
                rr(context_x + 4, context_y + i * 38 + 3, 222, 32, 8, accent, 45);
            label(context_x + 18, context_y + 10 + i * 38,
                  context_kind == 2 ? files_menu[i] : desktop_menu[i], enabled ? ink : muted);
        }
    }
}
static bool native_island_pointer(bool pressed) {
    if (!pressed || session_overlay_visible() || dialog_kind || scene_kind == SCENE_LAUNCHER)
        return false;
    int anchor = native_island_anchor();
    if (native_island_needed() && in(mx, my, anchor - 16, 0, 32, 32)) {
        island_expanded = !island_expanded;
        if (island_expanded)
            island_texture_valid = false;
        arkui_animation_set(&island_animation, island_expanded, ark_millis(), reduced_motion);
        dirty = true;
        return true;
    }
    if (island_expanded) {
        int x = anchor - ISLAND_W / 2, y = 43;
        if (in(mx, my, x, y, ISLAND_W, ISLAND_H)) {
            if (arkui_animation_progress(&island_animation) > MOTION_ONE * 9 / 10 &&
                in(mx, my, x + 16, y + 96, 146, 32)) {
                if (shelf_present && !reminder_pending) {
                    if (shelf_item.kind == ARK_DRAG_FILE)
                        load_note(vfs_find(shelf_item.data));
                    else {
                        new_window(2);
                        insert_input(shelf_item.data);
                    }
                } else
                    open_app(native_activity_app());
            } else if (!in(mx, my, x + 178, y + 96, 146, 32))
                return true;
            else {
                if (reminder_pending)
                    reminder_pending = false;
                else if (shelf_present) {
                    shelf_present = false;
                    memset(&shelf_item, 0, sizeof shelf_item);
                }
            }
            island_expanded = false;
            arkui_animation_set(&island_animation, false, ark_millis(), reduced_motion);
            dirty = true;
            return true;
        }
        island_expanded = false;
        arkui_animation_set(&island_animation, false, ark_millis(), reduced_motion);
        dirty = true;
    }
    return false;
}
static bool native_key(int k) {
    if (!session_active() || session_overlay_visible() || scene_kind == SCENE_LAUNCHER)
        return false;
    if (k == KEY_ESCAPE && island_expanded) {
        island_expanded = false;
        arkui_animation_set(&island_animation, false, ark_millis(), reduced_motion);
        dirty = true;
        return true;
    }
    if (package_ui_key(k))
        return true;
    if (context_visible) {
        if (k == KEY_ESCAPE) {
            context_visible = false;
            dirty = true;
        }
        return true;
    }
    if (k == 4 && !dialog_kind) {
        motion_cancel();
        if (!desktop_hidden) {
            for (int i = 0; i < WINS; i++) {
                hidden_windows[i] = windows[i].visible;
                windows[i].visible = false;
            }
        } else
            for (int i = 0; i < WINS; i++)
                windows[i].visible = hidden_windows[i];
        desktop_hidden = !desktop_hidden;
        dirty = true;
        return true;
    }
    if (k == 16 && !dialog_kind) {
        native_capture(false);
        return true;
    }
    if (k == 18 && focused_app() != 6 && !dialog_kind) {
        native_capture(true);
        return true;
    }
    if (k == 23 && !dialog_kind) {
        int a = focus();
        if (a >= 0)
            close_window(a);
        return true;
    }
    if (focused_app() == 12 && !dialog_kind && install_state.state != 1) {
        char *buf = install_field == 1   ? install_pass
                    : install_field == 2 ? install_pass2
                                         : install_confirm;
        size_t cap = install_field ? 63 : 5;
        size_t n = strlen(buf);
        if (k == KEY_BACKSPACE && n)
            buf[n - 1] = 0;
        else if (k >= 32 && k < 127 && n < cap) {
            buf[n] = (char)k;
            buf[n + 1] = 0;
        } else if (k == KEY_TAB) {
            if (install_encrypt)
                install_field = (install_field + 1) % 3;
            else
                install_field = 0;
        } else if (k == KEY_ENTER)
            native_click(12, windows[12].x + 35, windows[12].y + 520);
        else
            return false;
        dirty = true;
        return true;
    }
    return false;
}
static void native_draw_app(int app, Window *w) {
    int x = w->x + 24, y = w->y + 77, width = w->w - 48;
    if (app == 16) {
        package_ui_draw(w);
        return;
    }
    if (app == 11) {
        label(x, y, "截图与屏幕录制", ink);
        button(x, y + 38, 136, "截取屏幕", true);
        button(x + 150, y + 38, 150, recording ? "停止并保存" : "录制 6 秒", recording);
        clip(x, y + 93, capture_status, width, muted);
        label(x, y + 126, "我的截图与录屏", ink);
        for (unsigned i = 0; i < capture_count && i < 7; i++) {
            int yy = y + 163 + (int)i * 34;
            clip(x, yy, capture_files[i].name, width - 130, ink);
            num(x + width - 124, yy, capture_files[i].size / 1024, muted);
            label(x + width - 70, yy, "KiB", muted);
            symbol(x + width - 25, yy, 20, ARKUI_SYMBOL_TRASH, muted);
        }
        clip(x, w->y + w->h - 33, "Ctrl+P 截图 · Ctrl+R 开始或停止录屏", width, muted);
    } else if (app == 12) {
        bool can_enc = install_disks[install_choice].can_encrypt != 0;
        label(x, y, "将 ArkOS 安装到硬盘", ink);
        clip(x, y + 35, "清空目标盘并迁移账号与文本；已挂载的盘不可覆盖。", width, 0xcc574e);
        for (unsigned i = 0; i < 2; i++) {
            int yy = y + 70 + (int)i * 52;
            rr(x, yy, width, 44, 10, accent, install_choice == i ? 45 : 15);
            label(x + 15, yy + 12, i ? "磁盘 1" : "磁盘 0", ink);
            if (install_disks[i].present) {
                num(x + 125, yy + 12, install_disks[i].sectors / 2048, ink);
                label(x + 210, yy + 12, "MiB", muted);
                label(x + 300, yy + 12,
                      install_disks[i].in_use ? "正在使用 · 禁止覆盖" : "可选择 · 安装将清空",
                      muted);
            } else
                label(x + 125, yy + 12, "未发现", muted);
        }
        clip(x, y + 178, install_disks[install_choice].message, width, muted);
        /* Options sit below the disk list and above ERASE (Designer). */
        {
            int ty = y + 208;
            uint32_t enc_ink = can_enc ? ink : muted;
            rr(x, ty, width, 40, 10, accent, install_encrypt && can_enc ? 35 : 12);
            label(x + 14, ty + 11, "加密磁盘", enc_ink);
            rr(x + width - 62, ty + 10, 44, 20, 10, accent, can_enc ? (install_encrypt ? 200 : 55) : 30);
            rr(x + width - (install_encrypt && can_enc ? 34 : 56), ty + 12, 16, 16, 8, 0xffffff,
               can_enc ? 230 : 90);
            clip(x, ty + 44, can_enc ? "开机需要输入口令；忘记口令无法恢复"
                                     : "此设备不支持加密",
                 width, muted);
            ty += 78;
            rr(x, ty, width, 40, 10, accent, install_compress ? 35 : 12);
            label(x + 14, ty + 11, "压缩", ink);
            rr(x + width - 62, ty + 10, 44, 20, 10, accent, install_compress ? 200 : 55);
            rr(x + width - (install_compress ? 34 : 56), ty + 12, 16, 16, 8, 0xffffff, 230);
            clip(x, ty + 44, "节省空间，读写会稍慢", width, muted);
            ty += 78;
            if (install_encrypt && can_enc) {
                label(x, ty, "口令", ink);
                rr(x + 110, ty - 8, width - 110, 36, 8, accent, install_field == 1 ? 40 : 18);
                label(x + 122, ty + 2, install_pass[0] ? "••••••••" : "", muted);
                ty += 48;
                label(x, ty, "再次输入", ink);
                rr(x + 110, ty - 8, width - 110, 36, 8, accent, install_field == 2 ? 40 : 18);
                label(x + 122, ty + 2, install_pass2[0] ? "••••••••" : "", muted);
                ty += 48;
            }
            label(x, ty, "输入 ERASE 确认清盘：", ink);
            rr(x + 230, ty - 8, 140, 36, 8, accent, install_field == 0 ? 40 : 18);
            label(x + 245, ty + 2, install_confirm, ink);
            ty += 52;
            bool ready = !strcmp(install_confirm, "ERASE");
            if (install_encrypt && can_enc)
                ready = ready && install_pass[0] && !strcmp(install_pass, install_pass2);
            button(x, ty, 160, install_state.state == 1 ? "安装进行中" : "开始安装", ready);
            button(x + 175, ty, 120, "刷新设备", false);
            if (install_state.total) {
                rr(x, ty + 52, width, 8, 4, accent, 25);
                rr(x, ty + 52, (int)((uint64_t)width * install_state.done / install_state.total), 8, 4,
                   accent, 255);
            }
            clip(x, ty + 72, install_state.message, width, muted);
        }
    }
}
static void native_click(int app, int px, int py) {
    Window *w = app_window(app);
    int x = w->x + 24, y = w->y + 77, width = w->w - 48;
    if (app == 16) {
        package_ui_click(px, py);
        return;
    }
    if (app == 11) {
        if (in(px, py, x, y + 38, 136, 36))
            native_capture(false);
        else if (in(px, py, x + 150, y + 38, 150, 36))
            native_capture(true);
        else
            for (unsigned i = 0; i < capture_count && i < 7; i++)
                if (in(px, py, x + width - 30, y + 158 + (int)i * 34, 30, 30) && !recording) {
                    ArkBlobRequest q = capture_files[i];
                    q.op = ARK_BLOB_REMOVE;
                    ark_call(ARK_SYS_BLOB, &q, sizeof q);
                    native_capture_list();
                    break;
                }
    }
    if (app == 12 && install_state.state != 1) {
        bool can_enc = install_disks[install_choice].can_encrypt != 0;
        for (unsigned i = 0; i < 2; i++)
            if (in(px, py, x, y + 70 + (int)i * 52, width, 44))
                install_choice = i;
        int ty = y + 208;
        if (in(px, py, x, ty, width, 40) && can_enc)
            install_encrypt = !install_encrypt;
        ty += 78;
        if (in(px, py, x, ty, width, 40))
            install_compress = !install_compress;
        ty += 78;
        if (install_encrypt && can_enc) {
            if (in(px, py, x + 110, ty - 8, width - 110, 36))
                install_field = 1;
            ty += 48;
            if (in(px, py, x + 110, ty - 8, width - 110, 36))
                install_field = 2;
            ty += 48;
        }
        if (in(px, py, x + 230, ty - 8, 140, 36))
            install_field = 0;
        ty += 52;
        if (in(px, py, x + 175, ty, 120, 36))
            native_open(12);
        if (in(px, py, x, ty, 160, 36)) {
            bool can_start = !strcmp(install_confirm, "ERASE");
            if (install_encrypt && can_enc)
                can_start = can_start && install_pass[0] && !strcmp(install_pass, install_pass2);
            if (install_encrypt && !can_enc) {
                notice("此设备不支持加密");
                return;
            }
            if (!can_start) {
                if (strcmp(install_confirm, "ERASE"))
                    notice("请先输入 ERASE，确认清空所选磁盘");
                else if (install_encrypt && can_enc && !install_pass[0])
                    notice("口令不能为空");
                else if (install_encrypt && can_enc)
                    notice("两次口令不一致");
                return;
            }
            install_state = install_disks[install_choice];
            install_state.op = ARK_INSTALL_BEGIN;
            strcopy(install_state.confirm, install_confirm, sizeof install_state.confirm);
            install_state.features = 0;
            if (install_encrypt && can_enc)
                install_state.features |= 1u;
            if (install_compress)
                install_state.features |= 2u;
            if (install_state.features & 1u)
                strcopy(install_state.passphrase, install_pass, sizeof install_state.passphrase);
            else
                install_state.passphrase[0] = 0;
            if (ark_call(ARK_SYS_INSTALL, &install_state, sizeof install_state) < 0) {
                notice("无法开始安装：检查目标盘、管理员权限与 AHCI 光驱");
                install_state.state = 0;
            }
            memset(install_state.passphrase, 0, sizeof install_state.passphrase);
            install_confirm[0] = 0;
            install_pass[0] = install_pass2[0] = 0;
        }
    }
    dirty = true;
}
static void native_tick(uint64_t now) {
    if (now >= package_refresh_at)
        package_ui_refresh();
    if (arkui_animation_active(&island_animation)) {
        arkui_animation_update(&island_animation, ark_millis());
        dirty = true;
    }
    if (install_state.state == 1) {
        install_state.op = ARK_INSTALL_STEP;
        ark_call(ARK_SYS_INSTALL, &install_state, sizeof install_state);
        if (install_state.state != 1 || now % 10 == 0)
            dirty = true;
    }
    if (recording && now >= capture_next) {
        if (now - capture_started >= 600 || capture_frames >= 30) {
            native_capture(true);
        } else {
            size_t n = capture_gif_frame(capture_data + capture_size,
                                         sizeof capture_data - capture_size - 1, canvas,
                                         (unsigned)sw, (unsigned)sh, 512, 320, 20);
            if (!n)
                native_capture(true);
            else {
                capture_size += n;
                capture_frames++;
                capture_next = now + 20;
            }
        }
    }
    if (now >= activity_refresh) {
        activity_refresh = now + 100;
        ArkActivityRequest q = {.op = ARK_ACTIVITY_GET, .app = UINT32_MAX};
        if (ark_call(ARK_SYS_ACTIVITY, &q, sizeof q) >= 0) {
            if (q.active != external_activity.active || q.app != external_activity.app ||
                q.deadline != external_activity.deadline)
                dirty = true;
            external_activity = q;
        }
    }
    if (!native_island_needed() && island_expanded) {
        island_expanded = false;
        arkui_animation_set(&island_animation, false, ark_millis(), reduced_motion);
        dirty = true;
    }
    if (island_expanded && now / 100 != island_texture_second) {
        island_texture_valid = false;
        dirty = true;
    }
}
static void native_settings(int content, int category) {
    int card = settings_card(content);
    if (category == 5) {
        ArkNetworkRequest q = {0};
        ark_network(&q);
        settings_info(card, "网卡", q.present ? "有线网络" : "未发现受支持网卡");
        settings_info(card, "链路", q.link ? "已连接" : "未连接");
        settings_info(card, "配置", q.configured ? "自动获取地址" : "正在获取地址");
        char s[64] = "", n[24];
        for (unsigned i = 0; i < 4; i++) {
            uint_to_str((q.ipv4 >> (24 - i * 8)) & 255, n);
            cat(s, sizeof s, n);
            if (i < 3)
                cat(s, sizeof s, ".");
        }
        settings_info(card, "IPv4", s);
        settings_text(card, "当前设备未检测到可用的无线网卡。", true);
        settings_button(card, "打开浏览器", 600, false);
    } else if (category == 6) {
        if (!settings_subpage) {
            for (unsigned i = 0; i < ARK_CATALOG_COUNT; i++)
                arkui_navigation(&settings_ui, card, ark_catalog[i].label, ARKUI_SYMBOL_LOCK,
                                 800 + (int)i);
            settings_button(card, "管理下载的应用", 511, false);
            return;
        }
        ArkPermissionRequest q = {.op = ARK_PERMISSION_GET, .app = permission_selected};
        if (ark_call(ARK_SYS_PERMISSION, &q, sizeof q) >= 0) {
            int box = settings_card(content);
            settings_text(box, ark_catalog[permission_selected].label, false);
            static const unsigned bits[] = {ARK_CAP_UI, ARK_CAP_FILES, ARK_CAP_NETWORK,
                                            ARK_CAP_ACTIVITY};
            static const char *labels[] = {"运行与显示", "文件与外置磁盘", "网络访问",
                                           "顶部活动提示"};
            for (unsigned j = 0; j < 4; j++) {
                permission_values[j] = (q.grants & bits[j]) != 0;
                if (q.maximum & bits[j])
                    arkui_toggle(&settings_ui, box, labels[j], &permission_values[j], 850 + (int)j);
            }
        }
        settings_text(content, "撤销立即生效；关闭运行权限会终止应用。", true);
        settings_text(content, "下载的应用可以在应用管理中单独设置权限。", true);
    } else if (category == 7) {
        arkui_toggle(&settings_ui, card, "减少动态效果", &reduced_motion, 207);
        arkui_toggle(&settings_ui, card, "减少透明效果", &reduced_transparency, 204);
        arkui_slider(&settings_ui, card, "指针大小", &pointer_scale, 1, 2, 302);
        arkui_toggle(&settings_ui, card, "屏幕键盘", &keyboard, 303);
        settings_text(card, "支持中文显示与离线拼音候选。", true);
    } else if (category == 8) {
        settings_info(card, "F1–F4", "终端 / 文件 / 编辑 / 设置");
        settings_info(card, "F5", "应用启动台（可搜索）");
        settings_info(card, "Ctrl+D", "显示 / 恢复桌面");
        settings_info(card, "Ctrl+W", "关闭当前窗口");
        settings_info(card, "Ctrl+P / R", "截图 / 开始或停止录屏");
        settings_info(card, "Ctrl+T / K", "任务管理器 / 锁屏");
        settings_info(card, "Ctrl+I / S", "拼音输入 / 保存");
        settings_info(card, "Ctrl+C/X/V", "编辑器整篇复制 / 剪切 / 粘贴");
        settings_info(card, "Tab", "切换可见窗口");
    } else {
        settings_text(card, "设备与安装", false);
        settings_text(card, "可使用系统磁盘与光盘安装介质", true);
        settings_text(card, "支持鼠标、键盘与触控输入", true);
        settings_text(card, "显示与网络配置可在对应设置查看", true);
        settings_text(card, "设备信息可以在关于本机中查看", true);
        settings_text(card, "请选择系统能够识别的安装磁盘。", true);
        settings_button(card, "安装 ArkOS", 601, true);
        settings_button(card, "打开屏幕录制", 602, false);
        settings_button(card, "打开任务管理器", 603, false);
    }
}
static void native_settings_action(int id) {
    if (id == 600)
        open_app(6);
    else if (id == 601)
        open_app(12);
    else if (id == 602)
        open_app(11);
    else if (id == 603)
        open_app(10);
    else if (id == 604)
        open_app(17);
    else if (id >= 800 && id < 800 + (int)ARK_CATALOG_COUNT) {
        permission_selected = (unsigned)(id - 800);
        settings_navigate(settings_category, 60, 1);
        settings_build(app_window(3));
    } else if (id >= 850 && id < 854) {
        ArkPermissionRequest q = {.op = ARK_PERMISSION_SET, .app = permission_selected};
        static const unsigned bits[] = {ARK_CAP_UI, ARK_CAP_FILES, ARK_CAP_NETWORK,
                                        ARK_CAP_ACTIVITY};
        for (unsigned j = 0; j < 4; j++)
            if (permission_values[j])
                q.grants |= bits[j];
        if (ark_call(ARK_SYS_PERMISSION, &q, sizeof q) < 0)
            notice("权限已尝试更新，但无法持久化到磁盘");
        else {
            notice("应用权限已更新");
            serial_write("[permission] Settings ");
            serial_write(q.name);
            serial_write(" grants=");
            char n[24];
            uint_to_str(q.grants, n);
            serial_write(n);
            serial_write("\n");
        }
    }
    dirty = true;
}

static void native_background_region(int y, int x, int *next, int *covered) {
    for (unsigned i = 0; i < 8; i++) {
        int l, r, top, bottom;
        if (i < 6) {
            l = desktop_x[i] - 5;
            r = l + 94;
            top = desktop_y[i] - 5;
            bottom = top + 101;
        } else if (i == 6) {
            if (!arkui_animation_progress(&island_animation))
                continue;
            l = native_island_anchor() - ISLAND_W / 2 - 2;
            r = l + ISLAND_W + 4;
            top = 32;
            bottom = 43 + ISLAND_H + 2;
        } else {
            if (!context_visible)
                continue;
            l = context_x - 12;
            r = context_x + 243;
            top = context_y - 12;
            bottom = context_y + 209;
        }
        if (y < top || y >= bottom)
            continue;
        l = max(l, 0);
        r = min(r, sw);
        if (x >= l && x < r)
            *covered = max(*covered, r);
        else if (l > x)
            *next = min(*next, l);
    }
}
