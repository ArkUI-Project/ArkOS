/* Trusted package UI. Package parsing, admission and storage execute in kernel. */
static ArkPackageInfo package_preview;
static int package_list_scroll;
static char package_path[128], package_status[128] = "选择 .arkpkg 文件，或输入本地路径后读取。";
static int package_selected = -1;
static bool package_editing, package_preview_valid;
static uint64_t package_refresh_at;
static bool desktop_app_available(int app) {
    return app >= 0 && app < APPS && package_entries[app].installed;
}
static void package_ui_refresh(void) {
    bool *seen = ark_alloc((size_t)APPS);
    if (!seen) {
        strcopy(package_status, "内存不足，无法刷新应用", sizeof package_status);
        return;
    }
    unsigned seen_count = (unsigned)APPS;
    bool complete = false;
    for (unsigned index = 0;; index++) {
        ArkPackageRequest q = {.op = ARK_PACKAGE_LIST, .index = index};
        int64_t result = ark_package(&q);
        if (result == -2) {
            complete = true;
            break;
        }
        if (result < 0) {
            if (q.error[0])
                strcopy(package_status, q.error, sizeof package_status);
            break;
        }
        unsigned app =
            q.info.installed == 2 ? q.info.slot : ARK_PACKAGE_DESKTOP_FIRST + q.info.slot;
        if (app >= (unsigned)APPS) {
            bool *next = ark_realloc(seen, app + 1);
            if (!next)
                break;
            seen = next;
            memset(seen + seen_count, 0, app + 1 - seen_count);
            seen_count = app + 1;
            if (!desktop_resize(app + 1)) {
                strcopy(package_status, "内存不足，无法显示更多应用", sizeof package_status);
                break;
            }
        }
        seen[app] = true;
        if (memcmp(&q.info, &package_entries[app], sizeof q.info))
            dirty = true;
        package_entries[app] = q.info;
    }
    for (int app = 0; app < APPS; app++) {
        if (complete && !seen[app]) {
            if (package_entries[app].installed && dock_slot(app) >= 0)
                dock_toggle(app);
            memset(&package_entries[app], 0, sizeof package_entries[app]);
            windows[app].visible = false;
            minimized[app] = false;
        }
        titles[app] = package_entries[app].title;
        eng_titles[app] = app < (int)ARK_PACKAGE_DESKTOP_FIRST ? builtin_eng_titles[app]
                                                               : package_entries[app].id;
    }
    for (int w = APPS; w < WINS; w++) {
        int a = window_app(w);
        if (a >= 0) {
            titles[w] = titles[a];
            eng_titles[w] = eng_titles[a];
            if (!package_entries[a].installed) {
                windows[w].visible = false;
                minimized[w] = false;
            }
        }
    }
    ark_free(seen);
    if (package_selected >= 0 && !package_entries[package_selected].installed)
        package_selected = -1;
    package_refresh_at = ark_ticks() + 100;
}
static void package_ui_reset(void) {
    memset(package_entries, 0, (size_t)APPS * sizeof *package_entries);
    memset(&package_preview, 0, sizeof package_preview);
    package_path[0] = 0;
    package_selected = -1;
    package_editing = false;
    package_preview_valid = false;
    package_refresh_at = 0;
    strcopy(package_status, "选择 .arkpkg 文件，或输入本地路径后读取。", sizeof package_status);
    if (session_active())
        package_ui_refresh();
}
static void package_ui_inspect(void) {
    ArkPackageRequest q = {.op = ARK_PACKAGE_INSPECT};
    if (package_path[0] == '/' || !strncmp(package_path, "blob:", 5))
        strcopy(q.path, package_path, sizeof q.path);
    else
        path_join(q.path, user_home, package_path);
    package_preview_valid = ark_package(&q) >= 0;
    if (package_preview_valid) {
        package_preview = q.info;
        strcopy(package_path, q.path, sizeof package_path);
        strcopy(package_status, "安装文件已检查，可以继续安装。", sizeof package_status);
    } else
        strcopy(package_status, q.error[0] ? q.error : "无法读取安装包", sizeof package_status);
    package_editing = false;
    dirty = true;
}
static void package_ui_source(const char *path) {
    strcopy(package_path, path, sizeof package_path);
    package_ui_inspect();
}
static void package_ui_version(int x, int y, const ArkPackageInfo *p) {
    char version[48] = "", n[24];
    uint_to_str(p->major, n);
    cat(version, sizeof version, n);
    cat(version, sizeof version, ".");
    uint_to_str(p->minor, n);
    cat(version, sizeof version, n);
    cat(version, sizeof version, ".");
    uint_to_str(p->patch, n);
    cat(version, sizeof version, n);
    label(x, y, version, muted);
}
static bool package_ui_upgrade_available(void) {
    if (!package_preview.installed)
        return true;
    if (package_preview.slot + ARK_PACKAGE_DESKTOP_FIRST >= (unsigned)APPS)
        return false;
    const ArkPackageInfo *p = &package_preview,
                         *old = &package_entries[ARK_PACKAGE_DESKTOP_FIRST + p->slot];
    if (p->major != old->major)
        return p->major > old->major;
    if (p->minor != old->minor)
        return p->minor > old->minor;
    return p->patch > old->patch;
}
static void package_ui_draw(Window *w) {
    int x = w->x + 24, y = w->y + 77, width = w->w - 48;
    label(x, y, "ArkPkg", ink);
    label(x + 90, y, "应用管理", muted);
    rr(x, y + 34, width - 122, 38, 10, night ? 0x091727 : 0xffffff, 180);
    stroke(x, y + 34, width - 122, 38, 10, accent, package_editing ? 140 : 35);
    clip(x + 12, y + 44, *package_path ? package_path : "/mnt/fat32/application.arkpkg",
         width - 148, *package_path ? ink : muted);
    button(x + width - 110, y + 35, 110, "读取包", true);
    if (package_preview_valid) {
        clip(x, y + 88, package_preview.title, width - 240, ink);
        package_ui_version(x + width - 220, y + 88, &package_preview);
        clip(x, y + 117, package_preview.summary, width, muted);
        if (package_ui_upgrade_available())
            button(x, y + 147, 160, package_preview.installed ? "升级到此版本" : "安装此应用",
                   true);
        else {
            rr(x, y + 147, 160, 36, 8, accent, 20);
            label(x + 16, y + 156, "已有此版或更新版", muted);
        }
        label(x + 180, y + 157, "本地包 · 发布者未签名", muted);
    } else
        clip(x, y + 105, "应用安装后会出现在启动台；每个账号独立管理。", width, muted);
    clip(x, y + 193, package_status, width, muted);
    label(x, y + 229, "已安装", ink);
    int row = 0;
    for (unsigned i = 0; i < (unsigned)APPS; i++)
        if (package_entries[i].installed) {
            int visible = row++ - package_list_scroll;
            if (visible < 0 || visible >= (w->h - 371) / 31)
                continue;
            int yy = y + 262 + visible * 31;
            rr(x - 4, yy - 5, 244, 29, 8, accent, package_selected == (int)i ? 32 : 0);
            clip(x + 8, yy, package_entries[i].title, 226, ink);
        }
    if (!row)
        label(x + 8, y + 269, "还没有安装包", muted);
    if (package_selected >= 0) {
        const ArkPackageInfo *p = &package_entries[package_selected];
        int px = x + 264;
        clip(px, y + 230, p->id, width - 266, ink);
        package_ui_version(px, y + 260, p);
        button(px, y + 292, 104, "打开", true);
        if (p->installed != 2)
            button(px + 118, y + 292, 104, "卸载", false);
        else
            label(px + 120, y + 300, "系统应用", muted);
        static const unsigned bits[] = {ARK_CAP_UI, ARK_CAP_FILES, ARK_CAP_NETWORK,
                                        ARK_CAP_ACTIVITY};
        static const char *names[] = {"运行与显示", "用户文件", "网络访问", "顶部活动"};
        if (p->installed == 2) {
            clip(px, y + 343, "随系统提供，由系统镜像更新。", width - 266, muted);
            return;
        }
        for (unsigned i = 0; i < 4; i++) {
            int yy = y + 341 + (int)(i / 2) * 35, bx = px + (int)(i % 2) * 170;
            bool supported = (p->maximum & bits[i]) != 0;
            rr(bx, yy, 18, 18, 5, accent, supported ? ((p->grants & bits[i]) ? 255 : 25) : 10);
            if (p->grants & bits[i])
                symbol(bx + 1, yy + 1, 16, ARKUI_SYMBOL_CHECK, 0xffffff);
            label(bx + 26, yy, names[i], supported ? ink : muted);
        }
    }
}
static void package_ui_click(int px, int py) {
    Window *w = app_window(16);
    int x = w->x + 24, y = w->y + 77, width = w->w - 48;
    package_editing = in(px, py, x, y + 34, width - 122, 38);
    if (in(px, py, x + width - 110, y + 35, 110, 36))
        package_ui_inspect();
    else if (package_preview_valid && package_ui_upgrade_available() &&
             in(px, py, x, y + 147, 160, 36)) {
        ArkPackageRequest q = {.op = package_preview.installed ? ARK_PACKAGE_UPGRADE
                                                               : ARK_PACKAGE_INSTALL};
        strcopy(q.path, package_path, sizeof q.path);
        memcpy(q.expected_manifest_sha256, package_preview.manifest_sha256, 32);
        if (ark_package(&q) >= 0) {
            package_preview = q.info;
            package_selected = (int)(ARK_PACKAGE_DESKTOP_FIRST + q.info.slot);
            strcopy(package_status, "包与权限已提交到磁盘，可从启动台或这里打开。",
                    sizeof package_status);
            package_ui_refresh();
        } else
            strcopy(package_status, q.error, sizeof package_status);
    } else {
        int row = 0;
        for (unsigned i = 0; i < (unsigned)APPS; i++)
            if (package_entries[i].installed) {
                if (in(px, py, x, y + 257 + (row - package_list_scroll) * 31, 240, 31))
                    package_selected = (int)i;
                row++;
            }
        if (package_selected >= 0) {
            const ArkPackageInfo *p = &package_entries[package_selected];
            int bx = x + 264;
            if (in(px, py, bx, y + 292, 104, 36))
                open_app(package_selected);
            else if (p->installed != 2 && in(px, py, bx + 118, y + 292, 104, 36)) {
                ArkPackageRequest q = {.op = ARK_PACKAGE_REMOVE};
                strcopy(q.id, p->id, sizeof q.id);
                if (ark_package(&q) >= 0) {
                    strcopy(package_status, "应用包已卸载，用户文档继续保留。",
                            sizeof package_status);
                    package_preview_valid = false;
                    package_ui_refresh();
                } else
                    strcopy(package_status, q.error, sizeof package_status);
            } else if (p->installed != 2) {
                static const unsigned bits[] = {ARK_CAP_UI, ARK_CAP_FILES, ARK_CAP_NETWORK,
                                                ARK_CAP_ACTIVITY};
                for (unsigned i = 0; i < 4; i++)
                    if ((p->maximum & bits[i]) &&
                        in(px, py, bx + (int)(i % 2) * 170, y + 341 + (int)(i / 2) * 35, 164, 24)) {
                        ArkPackageRequest q = {.op = ARK_PACKAGE_GRANTS,
                                               .grants = p->grants ^ bits[i]};
                        strcopy(q.id, p->id, sizeof q.id);
                        if (ark_package(&q) < 0)
                            strcopy(package_status, q.error, sizeof package_status);
                        else {
                            strcopy(package_status, "权限已保存，正在运行的应用也立即生效。",
                                    sizeof package_status);
                            package_ui_refresh();
                        }
                        break;
                    }
            }
        }
    }
    dirty = true;
}
static bool package_ui_key(int key) {
    if (focused_app() != 16)
        return false;
    if (!package_editing) {
        if (key == KEY_DOWN || key == KEY_PAGE_DOWN || key == KEY_UP || key == KEY_PAGE_UP) {
            package_list_scroll = clamp(package_list_scroll + (key == KEY_DOWN        ? 1
                                                               : key == KEY_PAGE_DOWN ? 5
                                                               : key == KEY_UP        ? -1
                                                                                      : -5),
                                        0, max(0, APPS - 1));
            dirty = true;
            return true;
        }
        return false;
    }
    size_t n = strlen(package_path);
    if (key == KEY_ENTER)
        package_ui_inspect();
    else if (key == KEY_ESCAPE)
        package_editing = false;
    else if (key == KEY_BACKSPACE) {
        if (n)
            package_path[utf8_prev(package_path, n)] = 0;
        package_preview_valid = false;
    } else if (key >= 32 && key < 127 && n + 1 < sizeof package_path) {
        package_path[n] = (char)key;
        package_path[n + 1] = 0;
        package_preview_valid = false;
    } else
        return false;
    dirty = true;
    return true;
}

static void package_ui_scroll(int delta) {
    package_list_scroll = clamp(package_list_scroll + delta, 0, max(0, APPS - 1));
    dirty = true;
}
