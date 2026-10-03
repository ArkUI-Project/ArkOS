/* The desktop brokers copy drops; file permissions remain checked on opening. */
static void transfer_reset(void) {
    memset(&transfer, 0, sizeof transfer);
    memset(&shelf_item, 0, sizeof shelf_item);
    file_drag_path[0] = 0;
    file_drag_candidate = shelf_present = false;
}
static void transfer_refresh(void) {
    ArkDragRequest q = {.op = ARK_DRAG_STATUS};
    if (ark_drag(&q) >= 0) {
        bool changed = q.active != transfer.active || q.token != transfer.token;
        transfer = q;
        if (changed)
            dirty = true;
    }
}
static void transfer_cancel(void) {
    if (transfer.token) {
        ArkDragRequest q = {.op = ARK_DRAG_CANCEL, .token = transfer.token};
        (void)ark_drag(&q);
    }
    memset(&transfer, 0, sizeof transfer);
    dirty = true;
}
static bool transfer_begin(unsigned kind, const char *data, const char *mime) {
    ArkDragRequest q = {.op = ARK_DRAG_BEGIN,
                        .source = UINT32_MAX,
                        .target = UINT32_MAX,
                        .kind = kind,
                        .action = ARK_DRAG_COPY};
    q.length = (unsigned)strlen(data);
    strcopy(q.data, data, 512);
    strcopy(q.mime, mime, 48);
    if (ark_drag(&q) < 0) {
        notice("无法开始拖放，请检查文件与访问权限");
        return false;
    }
    transfer = q;
    dirty = true;
    return true;
}
static void transfer_draw(void) {
    if (!transfer.active)
        return;
    int width = min(
            300,
            utf8_width(transfer.kind == ARK_DRAG_FILE ? base(transfer.data) : transfer.data) + 48),
        x = clamp(mx + 18, 8, sw - width - 8), y = clamp(my + 17, 40, sh - 46);
    shadow(x, y, width, 35);
    glass(x, y, width, 35, 12, 230);
    symbol(x + 9, y + 7, 20,
           transfer.kind == ARK_DRAG_FILE ? ARKUI_SYMBOL_DOCUMENT : ARKUI_SYMBOL_COPY, accent);
    clip(x + 36, y + 8, transfer.kind == ARK_DRAG_FILE ? base(transfer.data) : transfer.data,
         width - 44, ink);
}
static bool transfer_pointer(bool pressed, bool down, bool moved) {
    transfer_refresh();
    if (pressed && !transfer.active && shelf_present && island_expanded) {
        int x = native_island_anchor() - ISLAND_W / 2;
        if (in(mx, my, x + 16, 43 + 35, ISLAND_W - 32, 50)) {
            (void)transfer_begin(shelf_item.kind, shelf_item.data, shelf_item.mime);
            return true;
        }
    }
    if (file_drag_candidate && !transfer.active) {
        if (!down)
            file_drag_candidate = false;
        else if (moved) {
            int dx = mx - file_drag_x, dy = my - file_drag_y;
            if (dx * dx + dy * dy >= 64) {
                file_drag_candidate = false;
                (void)transfer_begin(ARK_DRAG_FILE, file_drag_path, "application/x-ark-file");
            }
        }
    }
    if (!transfer.active)
        return false;
    if (down) {
        if (moved)
            dirty = true;
        return true;
    }
    for (int w = 0; w < WINS; w++)
        if (thirdparty[w].surface == transfer.source)
            thirdparty[w].held = false;
    if (in(mx, my, native_island_anchor() - 24, 0, 48, 36) ||
        (island_expanded &&
         in(mx, my, native_island_anchor() - ISLAND_W / 2, 43, ISLAND_W, ISLAND_H))) {
        shelf_item = transfer;
        shelf_present = true;
        transfer_cancel();
        island_expanded = true;
        island_texture_valid = false;
        arkui_animation_set(&island_animation, true, ark_millis(), reduced_motion);
        notice("已放入暂存区，拖到应用即可使用");
        serial_write("[drop] stored in shelf\n");
        return true;
    }
    int target = -1;
    for (int i = WINS - 1; i >= 0; i--) {
        int w = order[i];
        Window *window = &windows[w];
        if (window->visible && in(mx, my, window->x, window->y + 54, window->w, window->h - 54)) {
            target = w;
            break;
        }
    }
    if (target >= 0) {
        activate(target);
        if (thirdparty_app(target) && thirdparty[target].ready) {
            ArkDragRequest q = {.op = ARK_DRAG_DELIVER,
                                .token = transfer.token,
                                .target = thirdparty[target].surface,
                                .x = mx - windows[target].x - 1,
                                .y = my - windows[target].y - 54};
            if (ark_drag(&q) >= 0) {
                serial_write("[drop] delivered to ");
                serial_write(eng_titles[window_app(target)]);
                serial_write("\n");
                transfer.active = 0;
                dirty = true;
                return true;
            }
        } else if (window_app(target) == 1 && transfer.kind == ARK_DRAG_FILE) {
            char destination[128];
            path_join(destination, folder, base(transfer.data));
            if (!strcmp(destination, transfer.data))
                notice("文件已经在此文件夹中");
            else if (vfs_find(destination) >= 0)
                notice("已有同名文件，请先重命名");
            else if (vfs_copy(transfer.data, destination)) {
                refresh_files();
                save_now();
            } else
                notice(vfs_error()[0] ? vfs_error() : "无法复制这个项目");
        } else if (window_app(target) == 0) {
            if (transfer.kind == ARK_DRAG_TEXT)
                insert_input(transfer.data);
            else {
                char quote[272] = "\"";
                for (unsigned i = 0; transfer.data[i]; i++) {
                    char c = transfer.data[i], part[3] = {c, 0, 0};
                    if (c == '\"' || c == '\\') {
                        part[0] = '\\';
                        part[1] = c;
                    }
                    cat(quote, sizeof quote, part);
                }
                cat(quote, sizeof quote, "\"");
                insert_input(quote);
            }
        } else
            notice("这个位置暂不接受拖放");
    }
    transfer_cancel();
    return true;
}
