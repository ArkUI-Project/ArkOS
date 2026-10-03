/* Independent Ring3 surfaces. Window instances and application catalog IDs are
 * distinct; each surface receives its own input, close and resize requests. */
#include "ark_api.h"
#include "ark_catalog.h"
#define THIRD_COUNT ((unsigned)WINS)
#define THIRD_W ARK_SURFACE_MAX_W
#define THIRD_H ARK_SURFACE_MAX_H
typedef struct {
    uint32_t pid, surface, generation, width, height, requested_w, requested_h;
    bool ready, held, closing, text_input;
    ArkRect caret;
    uint64_t next_query, spawned, close_at;
} ThirdpartyState;
static ThirdpartyState *thirdparty;
static uint32_t *thirdparty_pixels[ARK_MAX_SURFACES];
static size_t thirdparty_capacity[ARK_MAX_SURFACES];
static int thirdparty_index(int wid) {
    int a = window_app(wid);
    if (a < 0)
        return -1;
    if (a >= (int)ARK_PACKAGE_DESKTOP_FIRST && a < APPS && desktop_app_available(a))
        return wid;
    return ark_catalog_desktop(a) >= 0 ? wid : -1;
}
static bool thirdparty_app(int wid) {
    return thirdparty_index(wid) >= 0;
}
static bool thirdparty_covers_window(int wid) {
    if (!thirdparty_app(wid))
        return false;
    ThirdpartyState *s = &thirdparty[wid];
    Window *w = &windows[wid];
    return s->ready && s->width >= (unsigned)(w->w - 2) && s->height >= (unsigned)(w->h - 55);
}
static bool thirdparty_request_event(int wid, ArkEvent event) {
    if (!thirdparty_app(wid))
        return false;
    ThirdpartyState *s = &thirdparty[wid];
    if (!s->ready)
        return false;
    ArkSurfaceRequest r = {.op = ARK_SURFACE_SEND_EVENT, .id = s->surface, .event = event};
    return ark_surface(&r) >= 0;
}
static int window_allocate(int app) {
    int wid = -1;
    if (!windows[app].visible && !minimized[app] && !thirdparty[app].pid)
        wid = app;
    else
        for (int i = APPS; i < WINS; i++)
            if (!windows[i].visible && !minimized[i] && !thirdparty[i].pid) {
                wid = i;
                break;
            }
    if (wid < 0) {
        notice("窗口数量已达到当前会话的资源上限，请关闭一些窗口");
        return -1;
    }
    window_apps[wid] = app;
    Window base = windows[latest_window(app)];
    if (base.w < 360)
        base = (Window){130, 90, 802, 555, 0, 0, 0, 0, false, false};
    base.x = clamp(base.x + 28, 12, sw - 240);
    base.y = clamp(base.y + 24, 40, sh - 140);
    base.visible = false;
    base.maxed = false;
    windows[wid] = base;
    titles[wid] = titles[app];
    eng_titles[wid] = eng_titles[app];
    minimized[wid] = false;
    cached_valid[wid] = thumbnail_valid[wid] = false;
    if (native_contexts[wid].shell) {
        ark_free(native_contexts[wid].shell);
        native_contexts[wid].shell = 0;
    }
    memset(&native_contexts[wid], 0, sizeof native_contexts[wid]);
    if (context_window == wid)
        context_window = -1;
    return wid;
}
static bool thirdparty_spawn(int wid, const char *argument, bool fresh) {
    if (!thirdparty_app(wid))
        return false;
    int a = window_app(wid), i = ark_catalog_desktop(a);
    ThirdpartyState *s = &thirdparty[wid];
    if (s->pid && !s->closing && !argument && !fresh) {
        s->next_query = 0;
        return true;
    }
    ArkSpawn r = {.flags = fresh ? ARK_SPAWN_NEW : 0};
    if (i >= 0)
        strcopy(r.program, ark_catalog[i].program, 64);
    else {
        strcopy(r.program, "pkg.", 64);
        strcopy(r.program + 4, package_entries[a].id, 60);
    }
    if (argument)
        strcopy(r.argument, argument, 128);
    int64_t result = ark_spawn(&r);
    if (result < 0) {
        notice(r.error[0] ? r.error : "无法启动应用");
        return false;
    }
    uint32_t pid = r.pid ? r.pid : (uint32_t)result;
    if (!pid) {
        notice("应用启动失败，请重试");
        return false;
    }
    if (s->pid == pid) {
        s->closing = false;
        s->next_query = 0;
        return true;
    }
    memset(s, 0, sizeof *s);
    s->pid = pid;
    s->surface = UINT32_MAX;
    s->spawned = platform_ticks();
    return true;
}
static bool thirdparty_open_argument(int a, const char *argument) {
    return thirdparty_spawn(a, argument, false);
}
static bool thirdparty_open(int a) {
    return thirdparty_open_argument(a, 0);
}
static void new_window_argument(int app, const char *argument) {
    if (app < 0 || app >= APPS || !desktop_app_available(app))
        return;
    int w = window_allocate(app);
    if (w < 0)
        return;
    if (app == 2 && !argument)
        argument = "new:";
    if (thirdparty_app(w) && !thirdparty_spawn(w, argument, true)) {
        if (w >= APPS)
            window_apps[w] = -1;
        return;
    }
    activate(w);
    if (app == 10)
        tasks_open();
    if (app >= 11)
        native_open(app);
    window_transition(w, true);
    serial_write("[ui] new window ");
    serial_write(eng_titles[app]);
    serial_write("\n");
}
static void minimize_window(int w) {
    if (w < 0)
        return;
    serial_write("[ui] minimize ");
    serial_write(eng_titles[window_app(w)]);
    serial_write("\n");
    windows[w].visible = false;
    minimized[w] = true;
    window_transition(w, false);
    dirty = true;
}
static void close_window(int w) {
    if (w < 0)
        return;
    session_window_closed(w);
    serial_write("[ui] close ");
    serial_write(eng_titles[window_app(w)]);
    serial_write("\n");
    thumbnail_valid[w] = false;
    windows[w].visible = false;
    minimized[w] = false;
    window_transition(w, false);
    if (thirdparty_app(w))
        thirdparty_close(w);
    else {
        int app = window_app(w);
        for (int i = 0; i < WINS; i++)
            if (window_app(i) == app) {
                session_window_closed(i);
                windows[i].visible = false;
                minimized[i] = false;
                thumbnail_valid[i] = cached_valid[i] = false;
                ark_free(native_contexts[i].shell);
                memset(&native_contexts[i], 0, sizeof native_contexts[i]);
                if (context_window == i) {
                    context_window = -1;
                    shell_switch(0);
                }
            }
    }
    dirty = true;
}
static void new_item(void) {
    int app = focused_app();
    if (app == 1)
        dialog_open(1, "未命名.txt");
    else if (app == 8 || app == 17 || app == 18) {
        ArkEvent e = {.type = ARK_EV_NEW};
        (void)thirdparty_request_event(focus(), e);
    } else if (app >= 0)
        new_window(app);
    else {
        open_app(1);
        dialog_open(1, "未命名.txt");
    }
}
static void new_window(int app) {
    new_window_argument(app, 0);
}
static void thirdparty_resize(int wid) {
    if (!thirdparty_app(wid))
        return;
    ThirdpartyState *s = &thirdparty[wid];
    if (!s->ready)
        return;
    Window *w = &windows[wid];
    unsigned width = (unsigned)max(1, w->w - 2), height = (unsigned)max(1, w->h - 55);
    if (s->requested_w == width && s->requested_h == height)
        return;
    ArkEvent e = {.type = ARK_EV_RESIZE, .x = (int)width, .y = (int)height};
    if (thirdparty_request_event(wid, e)) {
        s->requested_w = width;
        s->requested_h = height;
    }
}
static void draw_thirdparty(int wid, Window *w) {
    ThirdpartyState *s = &thirdparty[wid];
    int x = w->x + 1, y = w->y + 54, width = w->w - 2, height = w->h - 55;
    if (!s->ready) {
        label(x + 25, y + 34, s->pid ? "正在打开应用…" : "应用已停止，请重新打开。", muted);
        return;
    }
    const uint32_t *pixels = thirdparty_pixels[s->surface];
    for (int py = max(0, y); py < min(sh, y + min(height, (int)s->height)); py++)
        for (int px = max(0, x); px < min(sw, x + min(width, (int)s->width)); px++) {
            unsigned cover = rounded_coverage(px - x, py - y, width, height, 14);
            uint32_t color = pixels[(size_t)(py - y) * s->width + px - x];
            if (cover)
                canvas[(size_t)py * sw + px] =
                    cover == 255 ? color : mix(canvas[(size_t)py * sw + px], color, cover);
        }
}
static void thirdparty_key(int a, int key) {
    ArkEvent e = {.type = ARK_EV_KEY, .key = key};
    (void)thirdparty_request_event(a, e);
}
static void thirdparty_pointer(int wid, int x, int y, bool down) {
    if (!thirdparty_app(wid))
        return;
    ThirdpartyState *s = &thirdparty[wid];
    Window *w = &windows[wid];
    int bx = w->x + 1, by = w->y + 54;
    bool inside = in(x, y, bx, by, min(w->w - 2, (int)s->width), min(w->h - 55, (int)s->height));
    if (!s->ready || (!s->held && !inside))
        return;
    ArkEvent e = {.type = ARK_EV_POINTER, .x = x - bx, .y = y - by, .buttons = down ? 1u : 0u};
    if (down && inside)
        s->held = true;
    (void)thirdparty_request_event(wid, e);
    if (!down)
        s->held = false;
}
static void thirdparty_close(int wid) {
    if (!thirdparty_app(wid))
        return;
    ThirdpartyState *s = &thirdparty[wid];
    if (!s->pid)
        return;
    ArkEvent e = {.type = ARK_EV_CLOSE};
    if (thirdparty_request_event(wid, e)) {
        uint32_t pid = s->pid;
        uint64_t deadline = platform_ticks() + 100;
        for (int w = 0; w < WINS; w++)
            if (thirdparty[w].pid == pid) {
                if (w != wid)
                    (void)thirdparty_request_event(w, e);
                thirdparty[w].closing = true;
                thirdparty[w].held = false;
                thirdparty[w].close_at = deadline;
                thirdparty[w].next_query = 0;
                windows[w].visible = false;
                minimized[w] = false;
                thumbnail_valid[w] = false;
            }
    } else {
        windows[wid].visible = true;
        minimized[wid] = false;
        notice("应用正忙，请稍后关闭窗口");
    }
}
static bool surface_assigned(unsigned id) {
    for (int w = 0; w < WINS; w++)
        if (thirdparty[w].pid && thirdparty[w].surface == id)
            return true;
    return false;
}
static void thirdparty_tick(uint64_t now) {
    for (int wid = 0; wid < WINS; wid++) {
        ThirdpartyState *s = &thirdparty[wid];
        if (!s->pid || now < s->next_query)
            continue;
        s->next_query = now + 5;
        ArkSurfaceRequest r = {0};
        bool found = false;
        if (s->surface != UINT32_MAX) {
            r.op = ARK_SURFACE_QUERY;
            r.id = s->surface;
            if (ark_surface(&r) >= 0 && (r.flags & ARK_SURFACE_ALIVE) && r.pid == s->pid)
                found = true;
        } else
            for (unsigned id = 0; id < ARK_MAX_SURFACES; id++) {
                if (surface_assigned(id))
                    continue;
                memset(&r, 0, sizeof r);
                r.op = ARK_SURFACE_QUERY;
                r.id = id;
                if (ark_surface(&r) >= 0 && (r.flags & ARK_SURFACE_ALIVE) && r.pid == s->pid) {
                    s->surface = id;
                    found = true;
                    break;
                }
            }
        if (!found) {
            if (s->ready || s->closing || now - s->spawned > 500) {
                memset(s, 0, sizeof *s);
                s->surface = UINT32_MAX;
                windows[wid].visible = false;
                minimized[wid] = false;
                cached_valid[wid] = thumbnail_valid[wid] = false;
                dirty = true;
            }
            continue;
        }
        s->text_input = (r.flags & ARK_SURFACE_TEXT_INPUT) != 0;
        s->caret = r.damage;
        if (s->closing) {
            if (now >= s->close_at) {
                s->closing = false;
                activate(wid);
                notice("请保存文档，或在任务管理器结束进程");
            }
            continue;
        }
        if (!r.width || !r.height || r.width > THIRD_W || r.height > THIRD_H) {
            notice("应用窗口尺寸无效");
            thirdparty_close(wid);
            continue;
        }
        if (windows[wid].visible && (!s->ready || r.generation != s->generation)) {
            size_t need = (size_t)r.width * r.height * 4;
            if (need > thirdparty_capacity[s->surface]) {
                size_t capacity = (need + need / 4 + 4095) & ~(size_t)4095;
                if (capacity > (size_t)THIRD_W * THIRD_H * 4)
                    capacity = (size_t)THIRD_W * THIRD_H * 4;
                uint32_t *pixels = ark_memory(capacity);
                if (!pixels) {
                    notice("内存不足，请关闭一些应用");
                    continue;
                }
                if (thirdparty_pixels[s->surface])
                    (void)ark_memory_release(thirdparty_pixels[s->surface],
                                             thirdparty_capacity[s->surface]);
                thirdparty_pixels[s->surface] = pixels;
                thirdparty_capacity[s->surface] = capacity;
            }
            if (!s->ready) {
                Window *w = &windows[wid];
                w->w = min(max(360, (int)r.width + 2), sw - 28);
                w->h = min(max(180, (int)r.height + 55), sh - 142);
                ArkEvent theme = {.type = ARK_EV_THEME, .key = night};
                s->ready = true;
                (void)thirdparty_request_event(wid, theme);
            }
            r.op = ARK_SURFACE_COPY;
            r.pixels = (uintptr_t)thirdparty_pixels[s->surface];
            r.capacity = r.width * r.height;
            r.stride = r.width;
            if (ark_surface(&r) >= 0) {
                s->ready = true;
                s->width = r.width;
                s->height = r.height;
                s->generation = r.generation;
                dirty = true;
            }
        }
        /* Discover additional surfaces created by the same native process. */
        for (unsigned id = 0; id < ARK_MAX_SURFACES; id++) {
            if (surface_assigned(id))
                continue;
            ArkSurfaceRequest extra = {.op = ARK_SURFACE_QUERY, .id = id};
            if (ark_surface(&extra) < 0 || !(extra.flags & ARK_SURFACE_ALIVE) ||
                extra.pid != s->pid)
                continue;
            int w = window_allocate(window_app(wid));
            if (w < 0)
                break;
            thirdparty[w] = (ThirdpartyState){.pid = s->pid, .surface = id, .spawned = now};
            activate(w);
            dirty = true;
        }
    }
}
static void thirdparty_reset(void) {
    for (unsigned i = 0; i < THIRD_COUNT; i++) {
        ThirdpartyState *s = &thirdparty[i];
        if (s->pid && s->surface != UINT32_MAX) {
            ArkSurfaceRequest r = {.op = ARK_SURFACE_CLOSE, .id = s->surface};
            (void)ark_surface(&r);
        }
    }
    memset(thirdparty, 0, THIRD_COUNT * sizeof *thirdparty);
    for (unsigned i = 0; i < ARK_MAX_SURFACES; i++)
        if (thirdparty_pixels[i])
            memset(thirdparty_pixels[i], 0, thirdparty_capacity[i]);
    for (unsigned i = 0; i < THIRD_COUNT; i++)
        thirdparty[i].surface = UINT32_MAX;
}
