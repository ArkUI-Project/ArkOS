/* Cursor semantics follow the top visible window and captured interaction. */
static unsigned pointer_context(void) {
    if (transfer.active || drag >= 0 || dock_dragging || desktop_drag >= 0)
        return ARK_CURSOR_GRABBING;
    if (resize_app >= 0) {
        if ((resize_edges & 3) && (resize_edges & 12))
            return ((resize_edges & 1) != 0) == ((resize_edges & 4) != 0) ? ARK_CURSOR_RESIZE_NWSE
                                                                          : ARK_CURSOR_RESIZE_NESW;
        return resize_edges & 3 ? ARK_CURSOR_RESIZE_EW : ARK_CURSOR_RESIZE_NS;
    }
    if (session_overlay_visible()) {
        for (unsigned i = click_region_count; i > 0; i--) {
            ClickRegion *r = &click_regions[i - 1];
            if (in(mx, my, r->x, r->y, r->w, r->h))
                return r->shape;
        }
        return ARK_CURSOR_ARROW;
    }
    if (dialog_kind) {
        int x = (sw - 480) / 2, y = (sh - 232) / 2;
        if (dialog_kind <= 4 && in(mx, my, x + 24, y + 76, 432, 48))
            return ARK_CURSOR_TEXT;
    }
    int wid = pointer_window_at(mx, my);
    if (wid >= 0 && !dialog_kind && !permission_showing && caption_menu < 0 && !context_visible) {
        Window *w = &windows[wid];
        int edges = (mx < w->x + 6           ? 1
                     : mx >= w->x + w->w - 6 ? 2
                                             : 0) |
                    (my < w->y + 6           ? 4
                     : my >= w->y + w->h - 6 ? 8
                                             : 0);
        if (!w->maxed && (edges || in(mx, my, w->x + w->w - 22, w->y + w->h - 22, 22, 22))) {
            if (!edges)
                edges = 10;
            if ((edges & 3) && (edges & 12))
                return ((edges & 1) != 0) == ((edges & 4) != 0) ? ARK_CURSOR_RESIZE_NWSE
                                                                : ARK_CURSOR_RESIZE_NESW;
            return edges & 3 ? ARK_CURSOR_RESIZE_EW : ARK_CURSOR_RESIZE_NS;
        }
    }
    for (unsigned i = click_region_count; i > 0; i--) {
        ClickRegion *r = &click_regions[i - 1];
        if (r->owner >= 0 && r->owner != wid)
            continue;
        if (in(mx, my, r->x, r->y, r->w, r->h))
            return r->shape;
    }
    if (permission_showing || dialog_kind || context_visible)
        return ARK_CURSOR_ARROW;
    if (dock_app_at(mx, my) >= 0)
        return ARK_CURSOR_GRAB;
    if (my < 36)
        return ARK_CURSOR_POINTER;
    if (island_expanded && shelf_present &&
        in(mx, my, native_island_anchor() - ISLAND_W / 2 + 16, 78, ISLAND_W - 32, 50))
        return ARK_CURSOR_GRAB;
    if (wid < 0) {
        for (unsigned i = 0; i < 6; i++)
            if (in(mx, my, desktop_x[i] - 5, desktop_y[i] - 5, 94, 101))
                return ARK_CURSOR_GRAB;
        return ARK_CURSOR_ARROW;
    }
    Window *w = &windows[wid];
    if (my < w->y + 54)
        return w->maxed ? ARK_CURSOR_ARROW : ARK_CURSOR_GRAB;
    int app = window_app(wid);
    if (app == 0)
        return ARK_CURSOR_TEXT;
    if (app == 1 && in(mx, my, w->x + 193, w->y + 129, w->w - 195, w->h - 176)) {
        NativeContext *c = &native_contexts[wid];
        int scroll = context_window == wid ? file_scroll : c->file_scroll;
        int row = (my - w->y - 129) / 40 + scroll;
        if (row >= 0 && row < file_count)
            return ARK_CURSOR_GRAB;
    }
    if (thirdparty_app(wid) && thirdparty[wid].ready) {
        static int cached_w = -1, cached_x, cached_y;
        static uint32_t cached_generation;
        static unsigned cached_shape;
        int x = mx - w->x - 1, y = my - w->y - 54;
        uint32_t generation = thirdparty[wid].generation;
        if (wid != cached_w || x != cached_x || y != cached_y || generation != cached_generation) {
            ArkSurfaceRequest r = {.op = ARK_SURFACE_CURSOR_AT,
                                   .id = thirdparty[wid].surface,
                                   .event = {.x = x, .y = y}};
            cached_shape =
                ark_surface(&r) >= 0 && r.flags < ARK_CURSOR_COUNT ? r.flags : ARK_CURSOR_ARROW;
            cached_w = wid;
            cached_x = x;
            cached_y = y;
            cached_generation = generation;
        }
        return cached_shape;
    }
    return ARK_CURSOR_ARROW;
}
