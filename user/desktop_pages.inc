/* Navigation redraws both semantic pages over this frame's live glass. */
static struct {
    MotionTrack track;
    int window, category, subpage, scroll, direction;
    unsigned frames;
    uint32_t *base, *outgoing;
    size_t capacity;
} settings_page = {.window = -1};
static void settings_page_reset(void) {
    settings_page.track.active = false;
    settings_page.window = -1;
    if (settings_page.base) {
        (void)ark_memory_release(settings_page.base, settings_page.capacity);
        (void)ark_memory_release(settings_page.outgoing, settings_page.capacity);
        settings_page.base = settings_page.outgoing = 0;
        settings_page.capacity = 0;
    }
}
static void settings_navigate(int category, int subpage, int direction) {
    if (category == settings_category && subpage == settings_subpage)
        return;
    int wid = native_window;
    if (wid < 0 || window_app(wid) != 3)
        wid = latest_window(3);
    settings_page.category = settings_category;
    settings_page.subpage = settings_subpage;
    settings_page.scroll = settings_scroll;
    settings_page.direction = direction;
    settings_page.window = wid;
    settings_page.frames = 0;
    settings_page.track = (MotionTrack){0};
    motion_start(&settings_page.track, MOTION_ONE, ark_millis(), reduced_motion ? 0 : 240);
    settings_cancel_pointer();
    click_pending = false;
    settings_category = category;
    settings_subpage = subpage;
    settings_scroll = 0;
    session_page_changed(wid, category, subpage);
    serial_write("[ui] Settings page ");
    serial_write(settings_page_title());
    serial_write("\n");
    dirty = true;
}
static void settings_page_tick(uint64_t now) {
    (void)now;
    if (!settings_page.track.active)
        return;
    if (settings_page.window < 0 || !windows[settings_page.window].visible) {
        settings_page.track.active = false;
        return;
    }
    dirty = true;
}
static bool settings_page_buffers(size_t bytes) {
    bytes = (bytes + 4095) & ~(size_t)4095;
    if (bytes <= settings_page.capacity)
        return true;
    uint32_t *a = ark_memory(bytes), *b = ark_memory(bytes);
    if (!a || !b) {
        if (a)
            (void)ark_memory_release(a, bytes);
        if (b)
            (void)ark_memory_release(b, bytes);
        return false;
    }
    if (settings_page.base) {
        (void)ark_memory_release(settings_page.base, settings_page.capacity);
        (void)ark_memory_release(settings_page.outgoing, settings_page.capacity);
    }
    settings_page.base = a;
    settings_page.outgoing = b;
    settings_page.capacity = bytes;
    return true;
}
static void draw_settings(Window *w) {
    settings_sidebar(w);
    if (!settings_page.track.active || settings_page.window != native_window) {
        settings_draw_page(w);
        return;
    }
    int side = clamp(w->w / 4, 220, 270), x = max(0, w->x + side + 25), y = max(0, w->y + 68),
        right = min(sw, w->x + w->w - 25), bottom = min(sh, w->y + w->h - 10), width = right - x,
        height = bottom - y;
    if (width <= 0 || height <= 0 || !settings_page_buffers((size_t)width * height * 4)) {
        settings_page.track.active = false;
        settings_draw_page(w);
        return;
    }
    for (int row = 0; row < height; row++)
        memcpy(settings_page.base + (size_t)row * width, canvas + (size_t)(y + row) * sw + x,
               (size_t)width * 4);
    int category = settings_category, subpage = settings_subpage, scroll = settings_scroll;
    unsigned regions = click_region_count;
    settings_category = settings_page.category;
    settings_subpage = settings_page.subpage;
    settings_scroll = settings_page.scroll;
    settings_draw_page(w);
    for (int row = 0; row < height; row++) {
        memcpy(settings_page.outgoing + (size_t)row * width, canvas + (size_t)(y + row) * sw + x,
               (size_t)width * 4);
        memcpy(canvas + (size_t)(y + row) * sw + x, settings_page.base + (size_t)row * width,
               (size_t)width * 4);
    }
    settings_category = category;
    settings_subpage = subpage;
    settings_scroll = scroll;
    click_region_count = regions;
    settings_draw_page(w);
    uint64_t now = ark_millis();
    motion_update(&settings_page.track, now);
    int p = settings_page.track.value;
    if (p > 0 && p < MOTION_ONE)
        settings_page.frames++;
    int old_shift = -settings_page.direction * motion_lerp(0, 32, p),
        new_shift = settings_page.direction * motion_lerp(32, 0, p);
    unsigned alpha = (unsigned)((uint64_t)p * 255 / MOTION_ONE);
    for (int row = 0; row < height; row++) {
        uint32_t *dst = canvas + (size_t)(y + row) * sw + x,
                 *base = settings_page.base + (size_t)row * width,
                 *old = settings_page.outgoing + (size_t)row * width;
        /* The incoming row is copied before overlapping horizontal translation. */
        uint32_t incoming[MAX_W];
        memcpy(incoming, dst, (size_t)width * 4);
        for (int col = 0; col < width; col++) {
            int a = col - old_shift, b = col - new_shift;
            uint32_t from = a >= 0 && a < width ? old[a] : base[col],
                     to = b >= 0 && b < width ? incoming[b] : base[col];
            dst[col] = compositor_mix(from, to, alpha);
        }
    }
    if (!settings_page.track.active) {
        char n[24];
        serial_write("[page] end frames=");
        uint_to_str(settings_page.frames, n);
        serial_write(n);
        serial_write(" elapsed_ms=");
        uint_to_str(now - settings_page.track.start, n);
        serial_write(n);
        serial_write("\n");
    }
}
