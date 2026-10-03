/* Flow wallpaper: scanline advection of a procedural background. No decoder,
 * disk reads, per-frame allocation, or hidden window rerasterization. */
static uint64_t wallpaper_epoch;
static int wallpaper_phase;
static int wallpaper_offset_at(int y, int phase) {
    if (!live_wallpaper || reduced_motion)
        return 0;
    int p = (phase + y / 5) % 256;
    if (p < 0)
        p += 256;
    int t = p < 128 ? p : 255 - p;
    int smooth = t * t * (384 - 2 * t) / 16384;
    return smooth / 3 - 21;
}
static int wallpaper_offset(int y) {
    return wallpaper_offset_at(y, wallpaper_phase);
}
static void wallpaper_span(int y, int x, int width) {
    if (width <= 0)
        return;
    int source = x + wallpaper_offset(y), destination = x;
    if (source < 0) {
        int n = min(width, -source);
        for (int i = 0; i < n; i++)
            canvas[y * sw + destination + i] = wallpaper[y * sw];
        destination += n;
        width -= n;
        source = 0;
    }
    int n = min(width, max(0, sw - source));
    if (n > 0)
        memcpy(canvas + y * sw + destination, wallpaper + y * sw + source, (size_t)n * 4);
    destination += n;
    width -= n;
    for (int i = 0; i < width; i++)
        canvas[y * sw + destination + i] = wallpaper[y * sw + sw - 1];
}
static void wallpaper_draw(void) {
    wallpaper_phase = (int)(ark_ticks() / 5 % 256);
    for (int y = 0; y < sh; y++)
        wallpaper_span(y, 0, sw);
}
static void wallpaper_tick(uint64_t now) {
    if (!session_active() || session_overlay_visible() || !live_wallpaper || reduced_motion ||
        keyboard || dialog_kind || caption_menu >= 0 || scene_kind != SCENE_STILL || dirty ||
        message_ticks > 0 || drag >= 0 || resize_app >= 0)
        return;
    if (now - wallpaper_epoch < 5)
        return;
    int previous = wallpaper_phase;
    wallpaper_epoch = now;
    wallpaper_phase = (int)(now / 5 % 256);
    if (!reduced_transparency) {
        dirty = true;
        return;
    }
    int left = sw, top = sh, right = 0, bottom = 0;
    /* Preserve desktop icons, titlebar, dock and cached window shadows. Merge
     * occupied horizontal intervals by walking their nearest boundary. */
    for (int y = 39; y < sh - 112; y++) {
        if (wallpaper_offset_at(y, previous) == wallpaper_offset(y))
            continue;
        int x = 0;
        while (x < sw) {
            int next = sw, covered_end = x;
            native_background_region(y, x, &next, &covered_end);
            topbar_background_region(y, x, &next, &covered_end);
            for (int a = 0; a < WINS; a++) {
                Window *w = &windows[a];
                if (!w->visible || y < w->y - 8 || y >= w->y + w->h + 20)
                    continue;
                int l = max(0, w->x - 10), r = min(sw, w->x + w->w + 10);
                if (x >= l && x < r)
                    covered_end = max(covered_end, r);
                else if (l > x)
                    next = min(next, l);
            }
            if (covered_end > x) {
                x = covered_end;
                continue;
            }
            wallpaper_span(y, x, next - x);
            left = min(left, x);
            right = max(right, next);
            top = min(top, y);
            bottom = max(bottom, y + 1);
            x = next;
        }
    }
    if (right > left && bottom > top) {
        GpuRect damage = {left, top, right - left, bottom - top};
        gpu_present(canvas, (unsigned)sw, &damage);
        cursor_dirty = true;
    }
}
