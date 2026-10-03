/* Reference motion compositor. Cached native app pixels, no video playback.
 * ArkUI owns transition timing/presets; timestamps use the millisecond clock. */
#include "arkui_animation.h"
#include "thread.h"
enum {
    SCENE_STILL,
    SCENE_WINDOW,
    SCENE_LAUNCHER,
    SCENE_PREVIEW,
    SCENE_GLASS,
    SCENE_GLASS_SOURCE,
    SCENE_GLASS_PREP,
    SCENE_GLASS_BLUR
};
#define THUMB_W 224
#define THUMB_H 130
static uint32_t *motion_base, *motion_layer;
static uint32_t motion_tiles[((MAX_W + 7) / 8) * ((MAX_H + 7) / 8)];
#define WINDOW_CACHE_SLOTS 4
static uint32_t *cached_backdrop, *cached_window[WINDOW_CACHE_SLOTS];
static int *cached_slot;
static int cache_owner[WINDOW_CACHE_SLOTS] = {-1, -1, -1, -1};
static uint64_t cache_stamp[WINDOW_CACHE_SLOTS], cache_clock;
static bool cache_composing;
static int cached_app = -1, *cached_x, *cached_y, *cached_w, *cached_h;
static bool *cached_valid;
static uint32_t *launcher_layer;
static uint32_t launcher_tiles[((MAX_W + 7) / 8) * ((MAX_H + 7) / 8)];
static uint32_t launcher_dock[MAX_W * DOCK_H];
static uint8_t launcher_dock_cover[MAX_W * DOCK_H];
static uint32_t dock_plate[MAX_W * DOCK_H];
static bool dock_plate_valid;
static MotionTrack dock_lift[DOCK_MAX + 1];
static uint32_t **thumbnails;
static bool *thumbnail_valid;
static ArkUIAnimation scene_animation;
static int scene_kind, scene_app = -1, scene_x, scene_y, scene_w, scene_h;
static uint64_t scene_epoch, scene_last_frame, scene_max_gap;
static uint64_t scene_draw_ms, scene_upload_ms, scene_back_ms, scene_window_ms, scene_tiles_ms;
static uint64_t scene_blend_ms;
static GpuRect scene_previous;
static bool scene_first_frame;
static unsigned scene_frames;
static uint64_t scene_backdrop_at;
static bool scene_logged, launcher_target, launcher_keyboard_focus;
static uint64_t scene_gpu_begin[6];
static char launcher_query[48];
static int launcher_selection;
static int *launcher_apps, launcher_count, launcher_first, launcher_last;
static int dock_hover_app = -1;
static uint64_t dock_hover_since;

/* Native Ring3 workers blend disjoint spans. Job buffers stay immutable until
 * release/acquire completion; idle threads sleep on bounded message waits. */
static uint32_t render_tid[7], render_done[7], render_generation;
static unsigned render_workers, render_alpha;
static int render_scene, render_y, render_h;
static uint64_t scene_worker_ms[8];
static int dock_x(void);
static int dock_center(int app);
static void glass_expand_rows(int, int);
static void render_slice(unsigned index) {
    uint64_t started = platform_millis();
    int first = render_y + render_h * (int)index / (int)(render_workers + 1),
        last = render_y + render_h * (int)(index + 1) / (int)(render_workers + 1);
    if (first == last)
        return;
    if (render_scene >= SCENE_GLASS_SOURCE) {
        glass_process_rows((unsigned)(render_scene - SCENE_GLASS_SOURCE), index, first, last);
        return;
    }
    if (render_scene == SCENE_GLASS) {
        glass_expand_rows(first, last);
        return;
    }
    if (render_scene == SCENE_WINDOW) {
        ribbon_draw_cached(canvas + (size_t)first * sw, sw, last - first, motion_layer, scene_w,
                           scene_h, scene_x, scene_y - first, scene_w, scene_h,
                           dock_center(scene_app), sh - 64 - first,
                           arkui_animation_progress(&scene_animation), motion_tiles);
    } else {
        memcpy(canvas + (size_t)first * sw,
               (render_alpha == 255 ? launcher_layer : motion_base) + (size_t)first * sw,
               (size_t)(last - first) * sw * 4);
        if (render_alpha != 255)
            arkui_transition_draw_cached(
                &scene_animation, canvas + (size_t)first * sw, sw, last - first, launcher_layer, sw,
                sh, (ArkUIRect){0, -first, sw, sh}, sw / 2, (sh - 104) / 2 - first, launcher_tiles);
        int start = max(first, sh - 86), end = min(last, sh - 86 + DOCK_H), dx = dock_x();
        for (int y = start; y < end; y++) {
            int at = y * sw + dx, source = (y - (sh - 86)) * DOCK_W;
            if (render_alpha == 255) {
                memcpy(canvas + at, launcher_dock + source, DOCK_W * 4);
                continue;
            }
            uint32_t colors[DOCK_W];
            motion_mix_pixels(colors, motion_base + at, launcher_dock + source, DOCK_W,
                              render_alpha);
            for (int x = 0; x < DOCK_W; x++) {
                unsigned cover = launcher_dock_cover[source + x];
                if (cover)
                    canvas[at + x] =
                        cover == 255 ? colors[x] : mix(canvas[at + x], colors[x], cover);
            }
        }
    }
    scene_worker_ms[index] += platform_millis() - started;
}
static void render_worker(void *argument) {
    unsigned index = (unsigned)(uintptr_t)argument, last = 0;
    for (;;) {
        uint32_t generation = __atomic_load_n(&render_generation, __ATOMIC_ACQUIRE);
        if (generation != last) {
            render_slice(index);
            __atomic_store_n(&render_done[index], generation, __ATOMIC_RELEASE);
            last = generation;
            ArkMessage done = {.peer = ark_pid(), .type = 0x46414445};
            (void)ark_call(ARK_SYS_MSG_SEND, &done, sizeof done);
        }
        ArkMessage message;
        while (ark_call(ARK_SYS_MSG_RECV, &message, sizeof message) >= 0) {
        }
        if (__atomic_load_n(&render_generation, __ATOMIC_ACQUIRE) == last)
            (void)ark_wait_message_ms(1000);
    }
}
static void render_workers_init(void) {
    ArkTaskRequest q = {.op = ARK_TASK_LIST};
    if (ark_call(ARK_SYS_TASKS, &q, sizeof q) < 0)
        return;
    unsigned limit = q.online_cpus > 8 ? 7 : q.online_cpus ? q.online_cpus - 1 : 0;
    for (unsigned i = 0; i < limit; i++) {
        int64_t tid = ark_thread_create(render_worker, (void *)(uintptr_t)i);
        if (tid < 0)
            break;
        render_tid[i] = (uint32_t)tid;
        render_workers++;
    }
}
static void scene_parallel(int kind, unsigned alpha, GpuRect work) {
    render_scene = kind;
    render_alpha = alpha;
    render_y = work.y;
    render_h = work.h;
    if (!render_workers) {
        render_slice(0);
        return;
    }
    uint32_t generation = __atomic_add_fetch(&render_generation, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < render_workers; i++) {
        ArkMessage wake = {.peer = render_tid[i], .type = 0x46414445};
        (void)ark_call(ARK_SYS_MSG_SEND, &wake, sizeof wake);
    }
    render_slice(render_workers);
    for (;;) {
        ArkMessage message;
        while (ark_call(ARK_SYS_MSG_RECV, &message, sizeof message) >= 0) {
        }
        bool ready = true;
        for (unsigned i = 0; i < render_workers; i++)
            ready &= __atomic_load_n(&render_done[i], __ATOMIC_ACQUIRE) == generation;
        if (ready)
            break;
        (void)ark_wait_message_ms(1000);
    }
    ArkMessage message;
    while (ark_call(ARK_SYS_MSG_RECV, &message, sizeof message) >= 0) {
    }
}
static void compositor_glass_parallel(GpuRect work) {
    if ((uint64_t)work.w * work.h < 32768 || !render_workers)
        glass_expand_rows(work.y, work.y + work.h);
    else
        scene_parallel(SCENE_GLASS, 0, work);
}
static void compositor_glass_stage(unsigned stage) {
    if ((uint64_t)glass_work.bw * glass_work.bh < 65536 || !render_workers)
        glass_process_rows(stage, 0, 0, glass_work.bh);
    else
        scene_parallel(SCENE_GLASS_SOURCE + (int)stage, 0,
                       (GpuRect){0, 0, glass_work.bw, glass_work.bh});
}

static int dock_order[DOCK_MAX] = {1,  0,  2,  6,  5,  8,  9, 7, 13, 14,
                                   15, 17, 18, 10, 11, 16, 3, 4, 12};
static int dock_step(void) {
    return min(54, max(16, (sw - 54) / (dock_count + 1)));
}
static int dock_width(void) {
    return (dock_count + 1) * DOCK_STEP + 42;
}
static int dock_x(void) {
    return (sw - DOCK_W) / 2;
}
static int dock_slot(int app) {
    for (int i = 0; i < DOCK_APPS; i++)
        if (dock_order[i] == app)
            return i ? i + 1 : 0;
    return -1;
}
static int dock_slot_center(int slot) {
    return dock_x() + 21 + DOCK_STEP / 2 + slot * DOCK_STEP;
}
static int dock_center(int app) {
    int slot = dock_slot(window_app(app));
    return slot < 0 ? sw / 2 : dock_slot_center(slot);
}
static int dock_app_at(int x, int y) {
    if (keyboard)
        return -1;
    for (int i = 0; i < DOCK_APPS; i++) {
        int a = dock_order[i];
        if (in(x, y, dock_center(a) - DOCK_STEP / 2, sh - 84, DOCK_STEP, DOCK_H - 4))
            return a;
    }
    return -1;
}
static bool launcher_button_at(int x, int y) {
    return (!keyboard &&
            in(x, y, dock_slot_center(1) - DOCK_STEP / 2, sh - 84, DOCK_STEP, DOCK_H - 4));
}
static void launcher_glyph(int x, int y, int size) {
    rr(x, y, size, size, size / 4, 0x18212d, 255);
    stroke(x, y, size, size, size / 4, 0xe5eaff, 55);
    const uint32_t colors[] = {0xff758f, 0xa481e8, 0x64bcff, 0xffc854, 0x72dcb5,
                               0x57a5f5, 0xc391ef, 0x80cb5c, 0xeb728e};
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            rr(x + size * (22 + col * 21) / 100, y + size * (22 + row * 21) / 100, size * 14 / 100,
               size * 14 / 100, 2, colors[row * 3 + col], 255);
}
static void dock_icons(void) {
    for (int slot = 0; slot < DOCK_APPS + 1; slot++) {
        int lift = 6 * dock_lift[slot].value / MOTION_ONE, size = DOCK_ICON + lift;
        int cx = dock_slot_center(slot), y = sh - 86 + 10 - lift;
        if (slot == 1)
            launcher_glyph(cx - size / 2, y, size);
        else {
            int app = dock_order[slot ? slot - 1 : 0];
            icon(app, cx - size / 2, y, size);
            int running = 0;
            for (int w = 0; w < WINS; w++)
                if (window_app(w) == app && (windows[w].visible || minimized[w]))
                    running++;
            if (running)
                rr(cx - 2, sh - 86 + 62, 4, 4, 2, night ? 0xf4f7ff : 0xffffff, 235);
        }
    }
}
static void draw_dock(void) {
    dock_plate_valid = false;
    if (keyboard)
        return;
    int x = dock_x(), y = sh - 86;
    for (int i = 12; i >= 2; i -= 2)
        rr(x - i / 2, y + 5 - i / 2, DOCK_W + i, DOCK_H + i, 34 + i / 2, 0x071321, 6);
    glass_edge(x, y, DOCK_W, DOCK_H, 34, night ? 78 : 68, 80);
    rr(x + 1, y + 1, DOCK_W - 2, DOCK_H - 2, 33, night ? 0x34405a : 0xc6afbb, night ? 35 : 44);

    for (int row = 0; row < DOCK_H; row++)
        memcpy(dock_plate + row * DOCK_W, canvas + (y + row) * sw + x, DOCK_W * 4);
    dock_plate_valid = true;
    dock_icons();
}
static void dock_tick(uint64_t now) {
    if (!dock_plate_valid || keyboard || dialog_kind || permission_showing ||
        scene_kind != SCENE_STILL || drag >= 0 || resize_app >= 0)
        return;
    int app = dock_app_at(mx, my), slot = app < 0 ? (in(mx, my, dock_slot_center(1) - DOCK_STEP / 2,
                                                        sh - 84, DOCK_STEP, DOCK_H - 4)
                                                         ? 1
                                                         : -1)
                                                  : dock_slot(app);
    if (pointer_touch_mode || touch_down)
        slot = -1;
    bool changed = false;
    for (int i = 0; i < DOCK_APPS + 1; i++) {
        int target = i == slot ? MOTION_ONE : 0;
        int previous = dock_lift[i].value;
        if (dock_lift[i].to != target)
            motion_start(&dock_lift[i], target, now, reduced_motion ? 0 : 130);
        changed |= motion_update(&dock_lift[i], now);
        changed |= previous != dock_lift[i].value;
    }
    if (!changed)
        return;
    for (int row = 0; row < DOCK_H; row++)
        memcpy(canvas + (sh - 86 + row) * sw + dock_x(), dock_plate + row * DOCK_W, DOCK_W * 4);
    dock_icons();
    GpuRect damage = {dock_x(), sh - 86, DOCK_W, DOCK_H};
    gpu_present(canvas, (unsigned)sw, &damage);
}
static GpuRect motion_union(GpuRect a, GpuRect b) {
    if (a.w <= 0 || a.h <= 0)
        return b;
    if (b.w <= 0 || b.h <= 0)
        return a;
    int x = min(a.x, b.x), y = min(a.y, b.y);
    return (GpuRect){x, y, max(a.x + a.w, b.x + b.w) - x, max(a.y + a.h, b.y + b.h) - y};
}
static GpuRect motion_clip(GpuRect r) {
    int x = max(0, r.x), y = max(0, r.y), right = min(sw, r.x + r.w), bottom = min(sh, r.y + r.h);
    return (GpuRect){x, y, max(0, right - x), max(0, bottom - y)};
}
static void motion_restore(GpuRect r) {
    r = motion_clip(r);
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(canvas + y * sw + r.x, motion_base + y * sw + r.x, (size_t)r.w * 4);
}
static bool drag_cached;
static GpuRect drag_previous;
static void drag_begin(int app) {
    Window *w = &windows[app];
    scene_w = w->w;
    scene_h = w->h;
    drag_previous = (GpuRect){0, 0, sw, sh};
    drag_cached = true;
}
static bool drag_render(void) {
    if (drag < 0) {
        drag_cached = false;
        return false;
    }
    if (!drag_cached)
        return false;
    Window *w = &windows[drag];
    GpuRect box = {w->x - 12, w->y - 12, w->w + 24, w->h + 36};
    GpuRect damage = motion_clip(motion_union(drag_previous, box));
    window_cache_begin(false);
    compose_scene(-1);
    gpu_present(canvas, (unsigned)sw, &damage);
    present_pointer();
    drag_previous = box;
    dirty = false;
    return true;
}
static void thumbnail_capture(int app) {
    if (scene_kind != SCENE_STILL)
        return;
    Window *w = &windows[app];
    if (w->w <= 0 || w->h <= 0)
        return;
    if (!thumbnails[app])
        thumbnails[app] = ark_alloc(THUMB_W * THUMB_H * 4);
    if (!thumbnails[app])
        return;
    for (int y = 0; y < THUMB_H; y++)
        for (int x = 0; x < THUMB_W; x++) {
            int sx = clamp(w->x + x * w->w / THUMB_W, 0, sw - 1),
                sy = clamp(w->y + y * w->h / THUMB_H, 0, sh - 1);
            thumbnails[app][y * THUMB_W + x] = canvas[sy * sw + sx];
        }
    thumbnail_valid[app] = true;
}
/* Capture while the normal renderer already has the correct backdrop and
 * unobscured focused window. The first response to minimize needs no redraw. */
static void window_cache_begin(bool begin) {
    cache_composing = begin;
    if (!begin && focus() == cached_app && cached_app >= 0) {
        int y = max(0, sh - 112);
        memcpy(cached_backdrop + y * sw, canvas + y * sw, (size_t)sw * (sh - y) * 4);
    }
}
static void window_cache_before(int app) {
    if (cache_composing && app == focus()) {
        memcpy(cached_backdrop, canvas, (size_t)sw * sh * 4);
        cached_app = app;
    }
}
static int window_cache_slot(int app) {
    if (cached_valid[app])
        return cached_slot[app];
    int slot = 0;
    for (int i = 0; i < WINDOW_CACHE_SLOTS; i++) {
        if (cache_owner[i] < 0) {
            slot = i;
            break;
        }
        if (cache_stamp[i] < cache_stamp[slot])
            slot = i;
    }
    if (cache_owner[slot] >= 0)
        cached_valid[cache_owner[slot]] = false;
    cache_owner[slot] = app;
    cached_slot[app] = slot;
    return slot;
}
static void window_cache_after(int app) {
    if (!cache_composing || !windows[app].visible)
        return;
    Window *w = &windows[app];
    int slot = window_cache_slot(app);
    cache_stamp[slot] = ++cache_clock;
    cached_valid[app] = true;
    cached_x[app] = w->x;
    cached_y[app] = w->y;
    cached_w[app] = w->w;
    cached_h[app] = w->h;
    memset(cached_window[slot], 0, (size_t)w->w * w->h * 4);
    int left = max(0, w->x), right = min(sw, w->x + w->w);
    if (left < right)
        for (int y = max(0, w->y); y < min(sh, w->y + w->h); y++)
            memcpy(cached_window[slot] + (size_t)(y - w->y) * w->w + left - w->x,
                   canvas + (size_t)y * sw + left, (size_t)(right - left) * 4);
}
static void motion_number(const char *key, uint64_t value) {
    char b[24];
    serial_write(key);
    uint_to_str(value, b);
    serial_write(b);
}
static const char *motion_name(void) {
    return scene_kind == SCENE_WINDOW     ? "window"
           : scene_kind == SCENE_LAUNCHER ? "launcher"
                                          : "preview";
}
static void motion_log_begin(bool reverse) {
    serial_write(reverse ? "[motion] reverse kind=" : "[motion] begin kind=");
    serial_write(motion_name());
    motion_number(" from=", (uint64_t)scene_animation.track.from);
    motion_number(" to=", (uint64_t)scene_animation.track.to);
    motion_number(" duration_ms=", scene_animation.track.duration);
    motion_number(" start_ms=", scene_animation.track.start);
    serial_write("\n");
    scene_epoch = scene_animation.track.start;
    scene_frames = 0;
    scene_logged = false;
    scene_max_gap = 0;
    scene_last_frame = scene_epoch;
    scene_first_frame = true;
    scene_draw_ms = scene_upload_ms = scene_blend_ms = scene_back_ms = scene_window_ms =
        scene_tiles_ms = 0;
    memset(glass_stage_ms, 0, sizeof glass_stage_ms);
    memcpy(scene_gpu_begin, glass_gpu_stage_ms, sizeof scene_gpu_begin);
    memset(scene_worker_ms, 0, sizeof scene_worker_ms);
}
static void motion_log_end(void) {
    if (scene_logged)
        return;
    serial_write("[motion] end kind=");
    serial_write(motion_name());
    motion_number(" value=", (uint64_t)scene_animation.track.value);
    motion_number(" frames=", scene_frames);
    motion_number(" elapsed_ms=", platform_millis() - scene_epoch);
    motion_number(" max_gap_ms=", scene_max_gap);
    motion_number(" draw_ms=", scene_draw_ms);
    motion_number(" upload_ms=", scene_upload_ms);
    motion_number(" blend_ms=", scene_blend_ms);
    motion_number(" backdrop_ms=", scene_back_ms);
    motion_number(" material_ms=", scene_window_ms);
    motion_number(" tiles_ms=", scene_tiles_ms);
    motion_number(" glass_source_ms=", glass_stage_ms[0]);
    motion_number(" glass_prep_ms=", glass_stage_ms[1]);
    motion_number(" glass_blur_ms=", glass_stage_ms[2]);
    motion_number(" glass_lens_ms=", glass_stage_ms[3]);
    motion_number(" target_fps=", (uint64_t)frame_rate);
    for (unsigned i = 0; i <= render_workers; i++) {
        char key[] = " worker0_ms=";
        key[7] = (char)('0' + i);
        motion_number(key, scene_worker_ms[i]);
    }
    if (glass_gpu > 0) {
        static const char *const keys[] = {
            " gpu_validate_ms=", " gpu_stage_ms=",    " gpu_upload_ms=",
            " gpu_submit_ms=",   " gpu_readback_ms=", " gpu_copy_ms="};
        for (unsigned i = 0; i < 6; i++)
            motion_number(keys[i], glass_gpu_stage_ms[i] - scene_gpu_begin[i]);
    }
    serial_write("\n");
    scene_logged = true;
}
static void motion_retarget(bool presented) {
    uint64_t now = platform_millis();
    arkui_animation_set(&scene_animation, presented, now, reduced_motion);
    motion_log_begin(true);
    dirty = true;
}
static void motion_cancel(void) {
    if (scene_kind != SCENE_STILL) {
        memcpy(canvas, motion_base, (size_t)sw * sh * 4);
        if (scene_kind == SCENE_WINDOW && windows[scene_app].visible)
            ribbon_draw(canvas, sw, sh, motion_layer, scene_w, scene_h, scene_x, scene_y, scene_w,
                        scene_h, dock_center(scene_app), sh - 64, MOTION_ONE);
        scene_kind = SCENE_STILL;
        arkui_animation_finish(&scene_animation);
        launcher_target = false;
        dirty = true;
    }
    dock_hover_app = -1;
    dock_hover_since = platform_ticks();
}
static int dock_picker, dock_pin_first;
static void dock_save(void) {
    char items[512] = "";
    for (int i = 0; i < dock_count; i++) {
        const char *id = package_entries[dock_order[i]].id;
        if (strlen(items) + strlen(id) + 2 >= sizeof items) {
            notice("程序坞配置已达到存储长度上限");
            return;
        }
        if (i)
            cat(items, sizeof items, ",");
        cat(items, sizeof items, id);
    }
    ArkRegistryRequest q = {.op = ARK_REG_SET, .type = ARK_REG_STRING};
    strcopy(q.key, "/user/appearance/dock", 128);
    q.length = (unsigned)strlen(items);
    memcpy(q.value, items, q.length);
    if (ark_registry(&q) < 0)
        notice("无法保存程序坞，请检查系统磁盘");
    dock_plate_valid = false;
    motion_cancel();
    dirty = true;
}
static void dock_load(void) {
    static const int defaults[] = {1,  0,  2,  6,  5,  8,  9, 7, 13, 14,
                                   15, 17, 18, 10, 11, 16, 3, 4, 12};
    dock_count = 19;
    memcpy(dock_order, defaults, sizeof defaults);
    ArkRegistryRequest q = {.op = ARK_REG_GET};
    strcopy(q.key, "/user/appearance/dock", 128);
    if (ark_registry(&q) < 0 || q.type != ARK_REG_STRING || q.length >= 512)
        return;
    char items[512];
    memcpy(items, q.value, q.length);
    items[q.length] = 0;
    dock_count = 0;
    char *p = items;
    while (*p && dock_count < DOCK_MAX) {
        char *end = p;
        while (*end && *end != ',')
            end++;
        bool more = *end != 0;
        *end = 0;
        for (int a = 0; a < APPS; a++)
            if (desktop_app_available(a) && !strcmp(p, package_entries[a].id)) {
                bool duplicate = false;
                for (int i = 0; i < dock_count; i++)
                    if (dock_order[i] == a)
                        duplicate = true;
                if (!duplicate)
                    dock_order[dock_count++] = a;
                break;
            }
        if (!more)
            break;
        p = end + 1;
    }
    dock_plate_valid = false;
    memset(dock_lift, 0, sizeof dock_lift);
}
static void dock_toggle(int app) {
    int at = -1;
    for (int i = 0; i < dock_count; i++)
        if (dock_order[i] == app)
            at = i;
    if (at >= 0) {
        memmove(dock_order + at, dock_order + at + 1,
                (size_t)(dock_count - at - 1) * sizeof *dock_order);
        dock_count--;
    } else if (dock_count < DOCK_MAX)
        dock_order[dock_count++] = app;
    else {
        notice("程序坞已满，请先减少一些图标");
        return;
    }
    dock_save();
}
static void dock_settings(int content) {
    int card = settings_card(content);
    settings_text(card, "已固定的应用", false);
    dock_pin_first = clamp(dock_pin_first, 0, max(0, dock_count - 7));
    for (int i = dock_pin_first; i < min(dock_count, dock_pin_first + 7); i++) {
        int row = arkui_hstack(&settings_ui, card, 0, 6);
        settings_text(row, titles[dock_order[i]], false);
        settings_button(row, "↑", 300000 + i * 3, false);
        settings_button(row, "↓", 300001 + i * 3, false);
        settings_button(row, "取消固定", 300002 + i * 3, false);
    }
    int pages = arkui_hstack(&settings_ui, card, 0, 8);
    settings_button(pages, "前面", 400002, false);
    settings_button(pages, "后面", 400003, false);
    card = settings_card(content);
    settings_text(card, "选择应用", false);
    int rows = 0, shown = 0;
    for (int a = 0; a < APPS; a++)
        if (desktop_app_available(a)) {
            if (rows++ < dock_picker)
                continue;
            if (shown++ == 6)
                break;
            settings_button(card, titles[a], 2000 + a, dock_slot(a) >= 0);
        }
    int row = arkui_hstack(&settings_ui, card, 0, 8);
    settings_button(row, "上一页", 400000, false);
    settings_button(row, "下一页", 400001, false);
    settings_text(content, "拖动程序坞图标可调整顺序；拖出程序坞可取消固定。", true);
}
static void dock_settings_action(int id) {
    if (id == 400000)
        dock_picker = max(0, dock_picker - 6);
    else if (id == 400001)
        dock_picker = min(max(0, APPS - 6), dock_picker + 6);
    else if (id == 400002)
        dock_pin_first = max(0, dock_pin_first - 7);
    else if (id == 400003)
        dock_pin_first = min(max(0, dock_count - 7), dock_pin_first + 7);
    else if (id >= 300000) {
        int at = (id - 300000) / 3, action = (id - 300000) % 3;
        if (at >= dock_count)
            return;
        if (action == 2) {
            dock_toggle(dock_order[at]);
            return;
        }
        int other = action == 0 ? at - 1 : at + 1;
        if (other >= 0 && other < dock_count) {
            int a = dock_order[at];
            dock_order[at] = dock_order[other];
            dock_order[other] = a;
            dock_save();
        }
    } else if (id >= 2000 && id - 2000 < APPS)
        dock_toggle(id - 2000);
    dirty = true;
}
static int dock_drag_app = -1, dock_drag_x, dock_drag_y;
static bool dock_dragging;
static bool dock_pointer(bool pressed, bool down, bool moved) {
    if (keyboard || dialog_kind || permission_showing)
        return false;
    if (pressed) {
        int app = dock_app_at(mx, my);
        if (app < 0)
            return false;
        dock_drag_app = app;
        dock_drag_x = mx;
        dock_drag_y = my;
        dock_dragging = false;
        return true;
    }
    if (dock_drag_app < 0)
        return false;
    if (down && moved) {
        int dx = mx - dock_drag_x, dy = my - dock_drag_y;
        if (dx * dx + dy * dy > 64)
            dock_dragging = true;
        if (dock_dragging) {
            int target = dock_app_at(mx, my), from = -1, to = -1;
            for (int i = 0; i < dock_count; i++) {
                if (dock_order[i] == dock_drag_app)
                    from = i;
                if (dock_order[i] == target)
                    to = i;
            }
            if (from >= 0 && to >= 0 && from != to) {
                int app = dock_order[from];
                if (from < to)
                    memmove(dock_order + from, dock_order + from + 1,
                            (size_t)(to - from) * sizeof *dock_order);
                else
                    memmove(dock_order + to + 1, dock_order + to,
                            (size_t)(from - to) * sizeof *dock_order);
                dock_order[to] = app;
                dirty = true;
            }
        }
    }
    if (!down) {
        int app = dock_drag_app;
        dock_drag_app = -1;
        if (dock_dragging) {
            if (my < sh - 132)
                dock_toggle(app);
            else
                dock_save();
        } else if (dock_app_at(mx, my) == app)
            open_app(app);
        dock_dragging = false;
    }
    return true;
}
static void window_transition(int app, bool show) {
    if (scene_kind == SCENE_WINDOW && scene_app == app) {
        motion_retarget(show);
        return;
    }
    int old_kind = scene_kind;
    motion_cancel();
    if (reduced_motion) {
        dirty = true;
        return;
    }
    scene_backdrop_at = 0;
    scene_app = app;
    Window *w = &windows[app];
    scene_x = w->x;
    scene_y = w->y;
    scene_w = w->w;
    scene_h = w->h;
    bool cached = (show || cached_app == app) && cached_valid[app] && cached_x[app] == scene_x &&
                  cached_y[app] == scene_y && cached_w[app] == scene_w && cached_h[app] == scene_h;
    if (cached && (old_kind == SCENE_STILL || old_kind == SCENE_PREVIEW)) {
        if (show) {
            if (old_kind == SCENE_PREVIEW)
                memcpy(canvas, motion_base, (size_t)sw * sh * 4);
            memcpy(motion_base, canvas, (size_t)sw * sh * 4);
        } else
            memcpy(motion_base, cached_backdrop, (size_t)sw * sh * 4);
        cache_stamp[cached_slot[app]] = ++cache_clock;
        memcpy(motion_layer, cached_window[cached_slot[app]], (size_t)scene_w * scene_h * 4);
    } else {
        /* A never-painted app is rasterized once; no glass/text work per frame. */
        compose_scene(app);
        memcpy(motion_base, canvas, (size_t)sw * sh * 4);
        bool final_visible = w->visible;
        w->visible = true;
        draw_window(app);
        w->visible = final_visible;
        memset(motion_layer, 0, (size_t)scene_w * scene_h * 4);
        int left = max(0, scene_x), right = min(sw, scene_x + scene_w);
        if (left < right)
            for (int y = max(0, scene_y); y < min(sh, scene_y + scene_h); y++)
                memcpy(motion_layer + (size_t)(y - scene_y) * scene_w + left - scene_x,
                       canvas + (size_t)y * sw + left, (size_t)(right - left) * 4);
    }
    motion_texture_tiles(motion_tiles, motion_layer, scene_w, scene_h);
    scene_kind = SCENE_WINDOW;
    arkui_animation_init(&scene_animation, ARKUI_TRANSITION_SHEET, !show);
    uint64_t now = platform_millis();
    arkui_animation_set(&scene_animation, show, now, reduced_motion);
    scene_previous = (GpuRect){0, 0, sw, sh};
    scene_first_frame = true;
    motion_log_begin(false);
    caret_valid = false;
    dirty = true;
}
static bool contains_ascii(const char *value, const char *query) {
    if (!*query)
        return true;
    for (; *value; value++) {
        const char *a = value, *b = query;
        while (*a && *b) {
            int x = *a, y = *b;
            if (x >= 'A' && x <= 'Z')
                x += 32;
            if (y >= 'A' && y <= 'Z')
                y += 32;
            if (x != y)
                break;
            a++;
            b++;
        }
        if (!*b)
            return true;
    }
    return false;
}
static int launcher_page_size(void) {
    return 5 * max(1, (sh - 220) / 144);
}
static void launcher_layout(int index, int *x, int *y) {
    if (index < launcher_first || index >= launcher_last) {
        *x = -1000;
        *y = -1000;
        return;
    }
    int count = launcher_last - launcher_first, columns = min(5, max(1, count)),
        gap = min(190, (sw - 100) / columns);
    int rows = (count + columns - 1) / columns, step = min(180, (sh - 220) / max(1, rows)),
        local = index - launcher_first;
    *x = sw / 2 - (columns - 1) * gap / 2 + (local % columns) * gap - 32;
    *y = (sh - 104) / 2 - rows * step / 2 + 45 + (local / columns) * step;
}
static void build_launcher(void) {
    launcher_art(canvas, sw, sh, night);
    launcher_count = 0;
    for (int a = 0; a < (int)APPS; a++)
        if (desktop_app_available(a) && (contains_ascii(eng_titles[a], launcher_query) ||
                                         contains_ascii(titles[a], launcher_query)))
            launcher_apps[launcher_count++] = a;
    launcher_selection = clamp(launcher_selection, 0, max(0, launcher_count - 1));
    launcher_first = launcher_selection / launcher_page_size() * launcher_page_size();
    launcher_last = min(launcher_count, launcher_first + launcher_page_size());
    int sx = (sw - 320) / 2;
    rr(sx, 48, 320, 40, 20, 0xb5c7ec, 50);
    stroke(sx, 48, 320, 40, 20, 0xdceaff, 50);
    stroke(sx + 17, 60, 10, 10, 5, 0xcad6ee, 255);
    line(sx + 26, 69, sx + 31, 74, 0xcad6ee);
    clip(sx + 40, 58, *launcher_query ? launcher_query : "搜索应用", 264, 0xdbe7ff);
    for (int i = launcher_first; i < launcher_last; i++) {
        int x, y;
        launcher_layout(i, &x, &y);
        int a = launcher_apps[i];
        if ((launcher_keyboard_focus || *launcher_query) && i == launcher_selection)
            rr(x - 16, y - 12, 96, 117, 19, 0xa4c7ff, 34);
        icon(a, x, y, 64);
        label(x + 32 - utf8_width(titles[a]) / 2, y + 79, titles[a], 0xe7efff);
    }
    if (!launcher_count)
        label(sw / 2 - 64, (sh - 104) / 2, "未找到应用", 0xa9bddf);
    int pages = (launcher_count + launcher_page_size() - 1) / launcher_page_size();
    for (int i = 0; i < pages; i++)
        circle(sw / 2 + (i - pages / 2) * 18, sh - 153, 3,
               i == launcher_first / launcher_page_size() ? 0xf8fbff : 0x8294b7);
    memcpy(launcher_layer, canvas, (size_t)sw * sh * 4);
    motion_texture_tiles(launcher_tiles, launcher_layer, sw, sh);
    draw_dock();
    for (int y = 0; y < DOCK_H; y++)
        for (int x = 0; x < DOCK_W; x++) {
            launcher_dock[y * DOCK_W + x] = canvas[(sh - 86 + y) * sw + dock_x() + x];
            launcher_dock_cover[y * DOCK_W + x] =
                (uint8_t)rounded_coverage(x, y, DOCK_W, DOCK_H, 22);
        }
}
static void launcher_toggle(void) {
    if (dialog_kind)
        return;
    if (scene_kind == SCENE_LAUNCHER) {
        launcher_target = !launcher_target;
        motion_retarget(launcher_target);
        return;
    }
    int old_kind = scene_kind;
    bool had_keyboard = keyboard;
    if (old_kind == SCENE_PREVIEW)
        memcpy(canvas, motion_base, (size_t)sw * sh * 4);
    motion_cancel();
    if (keyboard)
        keyboard_set(false);
    drag = resize_app = -1;
    settings_cancel_pointer();
    if ((old_kind != SCENE_STILL && old_kind != SCENE_PREVIEW) || had_keyboard)
        compose_scene(-1);
    memcpy(motion_base, canvas, (size_t)sw * sh * 4);
    launcher_query[0] = 0;
    launcher_selection = 0;
    launcher_keyboard_focus = false;
    build_launcher();
    launcher_target = true;
    scene_kind = SCENE_LAUNCHER;
    arkui_animation_init(&scene_animation, ARKUI_TRANSITION_LAUNCHER, false);
    uint64_t now = platform_millis();
    arkui_animation_set(&scene_animation, true, now, reduced_motion);
    scene_first_frame = true;
    motion_log_begin(false);
    caret_valid = false;
    dirty = true;
}
static void launcher_open_selection(int index) {
    if (index < 0 || index >= launcher_count)
        return;
    int app = launcher_apps[index];
    motion_cancel();
    open_app(app);
    dirty = true;
    serial_write("[ui] launcher open ");
    serial_write(eng_titles[app]);
    serial_write("\n");
}
static bool launcher_keys(int key) {
    if (scene_kind == SCENE_PREVIEW)
        motion_cancel();
    if (scene_kind != SCENE_LAUNCHER)
        return false;
    if (key == KEY_ESCAPE) {
        if (launcher_target)
            launcher_toggle();
        return true;
    }
    if (key >= KEY_F1 && key <= KEY_F4) {
        motion_cancel();
        open_app(key - KEY_F1);
        return true;
    }
    if (key == KEY_ENTER) {
        launcher_open_selection(launcher_selection);
        return true;
    }
    if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        launcher_selection =
            clamp(launcher_selection +
                      (key == KEY_PAGE_DOWN ? launcher_page_size() : -launcher_page_size()),
                  0, max(0, launcher_count - 1));
        launcher_keyboard_focus = true;
        build_launcher();
        dirty = true;
        return true;
    }
    if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN) {
        launcher_keyboard_focus = true;
        int columns = min(5, max(1, launcher_count));
        int delta = key == KEY_LEFT    ? -1
                    : key == KEY_RIGHT ? 1
                    : key == KEY_UP    ? -columns
                                       : columns;
        launcher_selection = clamp(launcher_selection + delta, 0, max(0, launcher_count - 1));
        build_launcher();
        dirty = true;
        return true;
    }
    size_t len = strlen(launcher_query);
    if (key == KEY_BACKSPACE) {
        if (len)
            launcher_query[len - 1] = 0;
    } else if (key >= 32 && key < 127 && len < sizeof launcher_query - 1) {
        launcher_query[len] = (char)key;
        launcher_query[len + 1] = 0;
    } else
        return true;
    launcher_selection = 0;
    build_launcher();
    dirty = true;
    return true;
}
static void preview_start(int app) {
    if (!thumbnail_valid[app] || (!windows[app].visible && !minimized[app]) ||
        scene_kind != SCENE_STILL)
        return;
    scene_app = app;
    scene_w = 240;
    scene_h = 174;
    scene_x = clamp(dock_center(app) - 120, 12, sw - 252);
    scene_y = sh - 86 - scene_h - 12;
    memcpy(motion_base, canvas, (size_t)sw * sh * 4);
    glass(scene_x, scene_y, scene_w, scene_h, 14, 220);
    label(scene_x + 14, scene_y + 10, titles[app], ink);
    motion_blit(canvas, sw, sh, thumbnails[app], THUMB_W, THUMB_H, (scene_x + 8) * MOTION_ONE,
                (scene_y + 36) * MOTION_ONE, THUMB_W * MOTION_ONE, THUMB_H * MOTION_ONE, 255, 7);
    memset(motion_layer, 0, (size_t)scene_w * scene_h * 4);
    int left = max(0, scene_x), right = min(sw, scene_x + scene_w);
    if (left < right)
        for (int y = max(0, scene_y); y < min(sh, scene_y + scene_h); y++)
            memcpy(motion_layer + (size_t)(y - scene_y) * scene_w + left - scene_x,
                   canvas + (size_t)y * sw + left, (size_t)(right - left) * 4);
    scene_kind = SCENE_PREVIEW;
    arkui_animation_init(&scene_animation, ARKUI_TRANSITION_HOVER, false);
    uint64_t now = platform_millis();
    arkui_animation_set(&scene_animation, true, now, reduced_motion);
    scene_previous = (GpuRect){scene_x - 2, scene_y - 2, scene_w + 4, scene_h + 28};
    scene_first_frame = true;
    motion_log_begin(false);
    dirty = true;
}
static bool motion_pointer(bool pressed, bool down, bool touching) {
    (void)touching;
    (void)down;
    if (scene_kind == SCENE_LAUNCHER) {
        if (pressed) {
            if (launcher_button_at(mx, my))
                launcher_toggle();
            else {
                int app = dock_app_at(mx, my);
                if (app >= 0) {
                    motion_cancel();
                    open_app(app);
                } else if (launcher_target && scene_animation.track.value > MOTION_ONE / 2) {
                    int scale = MOTION_ONE + (MOTION_ONE - scene_animation.track.value) / 5;
                    int px = sw / 2 + (int)((int64_t)(mx - sw / 2) * MOTION_ONE / scale);
                    int py =
                        (sh - 104) / 2 + (int)((int64_t)(my - (sh - 104) / 2) * MOTION_ONE / scale);
                    bool hit = false;
                    for (int i = 0; i < launcher_count; i++) {
                        int x, y;
                        launcher_layout(i, &x, &y);
                        if (in(px, py, x - 20, y - 16, 104, 125)) {
                            launcher_open_selection(i);
                            hit = true;
                            break;
                        }
                    }
                    if (!hit && !in(px, py, (sw - 320) / 2, 40, 320, 64))
                        launcher_toggle();
                }
            }
        }
        return true;
    }
    if (scene_kind == SCENE_PREVIEW) {
        bool over = in(mx, my, scene_x, scene_y - 10, scene_w, scene_h + 24) ||
                    dock_app_at(mx, my) == scene_app;
        if (pressed) {
            int app = scene_app;
            bool hit = in(mx, my, scene_x, scene_y, scene_w, scene_h);
            motion_cancel();
            if (hit) {
                open_app(app);
                return true;
            }
        } else if (over && scene_animation.track.to == 0)
            motion_retarget(true);
        else if (!over && scene_animation.track.to != 0)
            motion_retarget(false);
    }
    if (scene_kind == SCENE_WINDOW) {
        /* Retarget against the destination window while it is unfolding. */
        if (pressed) {
            int app = scene_app;
            Window *w = &windows[app];
            int d = dock_app_at(mx, my);
            if (d == window_app(app)) {
                windows[app].visible = true;
                minimized[app] = false;
                motion_retarget(true);
                return true;
            }
            if (w->visible && in(mx, my, w->x + w->w - 142, w->y + 9, 40, 36)) {
                w->visible = false;
                minimized[app] = true;
                motion_retarget(false);
                return true;
            }
            motion_cancel();
        } else
            return true;
    }
    return false;
}
static void motion_tick(uint64_t now) {
    if (scene_kind == SCENE_STILL) {
        int app = pointer_was_down || touch_down || pointer_touch_mode || dialog_kind
                      ? -1
                      : dock_app_at(mx, my);
        if (app != dock_hover_app) {
            dock_hover_app = app;
            dock_hover_since = now;
        } else if (app >= 0 && now - dock_hover_since >= 24)
            preview_start(app);
        return;
    }
    if (arkui_animation_active(&scene_animation))
        dirty = true;
}
static bool motion_render(void) {
    if (scene_kind == SCENE_STILL)
        return false;
    uint64_t now = platform_millis();
    arkui_animation_update(&scene_animation, now);
    int p = arkui_animation_progress(&scene_animation);
    ArkUIRect bounds =
        arkui_transition_bounds(&scene_animation, (ArkUIRect){scene_x, scene_y, scene_w, scene_h},
                                dock_center(scene_app), sh - 64);
    GpuRect current = {bounds.x, bounds.y, bounds.w, bounds.h};
    GpuRect damage = scene_kind == SCENE_LAUNCHER
                         ? (GpuRect){0, 0, sw, sh}
                         : motion_clip(motion_union(scene_previous, current));
    /* Curved windows restore only their damage. Launcher workers restore their
     * own rows, preserve the centered zoom, and keep the Dock outside scaling. */
    if (scene_kind == SCENE_WINDOW && !reduced_transparency &&
        (glass_gpu > 0 || now / 50 != scene_backdrop_at)) {
        scene_backdrop_at = now / 50;
        uint64_t prep = platform_millis();
        window_cache_begin(false);
        compose_scene(scene_app);
        memcpy(motion_base, canvas, screen_bytes);
        scene_back_ms += platform_millis() - prep;
        prep = platform_millis();
        Window *w = &windows[scene_app];
        bool visible = w->visible;
        if (visible || minimized[scene_app]) {
            w->visible = true;
            draw_window(scene_app);
            w->visible = visible;
            memset(motion_layer, 0, (size_t)scene_w * scene_h * 4);
            int left = max(0, scene_x), right = min(sw, scene_x + scene_w);
            if (left < right)
                for (int y = max(0, scene_y); y < min(sh, scene_y + scene_h); y++)
                    memcpy(motion_layer + (size_t)(y - scene_y) * scene_w + left - scene_x,
                           canvas + (size_t)y * sw + left, (size_t)(right - left) * 4);
            scene_window_ms += platform_millis() - prep;
            prep = platform_millis();
            motion_texture_tiles(motion_tiles, motion_layer, scene_w, scene_h);
            scene_tiles_ms += platform_millis() - prep;
        }
        damage = (GpuRect){0, 0, sw, sh};
    }
    if (scene_kind != SCENE_LAUNCHER)
        motion_restore(damage);
    caret_valid = false;
    if (scene_kind == SCENE_WINDOW) {
        scene_parallel(SCENE_WINDOW, 0, damage);
    } else if (scene_kind == SCENE_LAUNCHER) {
        unsigned alpha = (unsigned)((uint64_t)p * 255 / MOTION_ONE);
        scene_parallel(SCENE_LAUNCHER, alpha, damage);
        scene_blend_ms += platform_millis() - now;
    } else {
        arkui_transition_draw(&scene_animation, canvas, sw, sh, motion_layer, scene_w, scene_h,
                              (ArkUIRect){scene_x, scene_y, scene_w, scene_h}, 0, 0);
    }
    uint64_t drawn = platform_millis();
    scene_draw_ms += drawn - now;
    if (scene_kind == SCENE_LAUNCHER || scene_kind == SCENE_WINDOW)
        gpu_present_animation_damage(canvas, (unsigned)sw,
                                     scene_kind == SCENE_LAUNCHER ? 0 : &damage);
    else
        gpu_present(canvas, (unsigned)sw, &damage);
    scene_upload_ms += platform_millis() - drawn;
    scene_previous = current;
    scene_frames++;
    if (!scene_first_frame && now - scene_last_frame > scene_max_gap)
        scene_max_gap = now - scene_last_frame;
    scene_last_frame = now;
    scene_first_frame = false;
    present_pointer();
    dirty = false;
    if (!arkui_animation_active(&scene_animation)) {
        motion_log_end();
        if (scene_kind == SCENE_WINDOW || p == 0) {
            scene_kind = SCENE_STILL;
            launcher_target = false;
            dock_hover_app = -1;
            dock_hover_since = platform_ticks();
            dirty = true;
        }
    }
    return true;
}
