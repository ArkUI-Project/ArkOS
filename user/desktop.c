#include "ark.h"
#include "storage.h"
#include "extfs.h"
#include "unicode.h"
#include "virtio_input.h"
#include "arkui.h"
#include "gpu.h"
#include "raster.h"
#include "motion.h"
#include "ribbon.h"
#include "launcher_art.h"
#include "ark_api.h"
#define ARK_DRV_PAGE_MAX 8u
#include "arkui_icons.h"
#include "ark_catalog.h"
#include "package.h"
#include "alloc.h"
#include "liquid_glass.h"
#define MAX_W 3840
#define MAX_H 2160
#define APPS app_count
#define EXTRA_WINDOWS 12
#define WINS (app_count + EXTRA_WINDOWS)
static int app_count;
static int *window_apps;
static int window_app(int w) {
    return w < 0 || w >= WINS || !window_apps ? -1 : window_apps[w];
}
static int focused_app(void);
static void new_window(int);
static void new_window_argument(int, const char *);
static void close_window(int);
static void minimize_window(int);
static void new_item(void);
static void refresh_files(void);
static void refresh_files_visible(void);
static void files_open_path(const char *);
static void thirdparty_resize(int);
static bool desktop_resize(unsigned);
#define DOCK_MAX 32
static int dock_count = 19;
static int dock_width(void), dock_step(void);
#define DOCK_APPS dock_count
#define DOCK_STEP dock_step()
#define DOCK_ICON min(44, DOCK_STEP - 8)
#define DOCK_H 72
#define DOCK_W dock_width()
#define LINE 24
static BootInfo boot;
static uint32_t *canvas, *wallpaper;
static size_t screen_bytes;
static uint8_t shadow_channels[3][256];
static bool shadow_channels_ready, shadow_pending;
static int shadow_x0, shadow_x1, shadow_y0, shadow_y1;
static uint32_t shadow_color(uint32_t c) {
    return ((uint32_t)shadow_channels[0][(c >> 16) & 255] << 16) |
           ((uint32_t)shadow_channels[1][(c >> 8) & 255] << 8) | shadow_channels[2][c & 255];
}
static int sw, sh, mx, my, drag = -1, dragx, dragy;
static bool dirty = true, night = false, keyboard = false, touch_down = false;
static bool keyboard_symbols, keyboard_upper, ime_enabled;
static bool ime_key(int), ime_pointer(bool);
static bool transfer_pointer(bool, bool, bool);
static void transfer_draw(void), transfer_reset(void), transfer_refresh(void);
static void reminders_reset(void), reminders_tick(uint64_t);
static void ime_draw(void), ime_reset(void), ime_tick(void), text_dispatch(const char *);
static bool pointer_was_down = false, cursor_dirty = true, pointer_touch_mode = false;
typedef struct {
    int x, y, w, h, owner;
    unsigned shape;
    bool liquid;
} ClickRegion;
static ClickRegion click_regions[384], click_capture;
static unsigned click_region_count;
static int click_owner = -1, click_x, click_y, click_window = -1;
static bool click_has_region, click_pending, pointer_clicked;
static void click_region(int x, int y, int w, int h) {
    if (click_region_count < 384)
        click_regions[click_region_count++] = (ClickRegion){
            .x = x, .y = y, .w = w, .h = h, .owner = click_owner, .shape = ARK_CURSOR_POINTER};
}
static void click_text_region(int x, int y, int w, int h) {
    click_region(x, y, w, h);
    if (click_region_count)
        click_regions[click_region_count - 1].shape = ARK_CURSOR_TEXT;
}
static unsigned pointer_context(void);
static unsigned pointer_shape;
static bool *minimized;
static bool reduced_motion = false;
static int frame_rate = 120;
static MotionFrameClock frame_clock;
static void window_transition(int app, bool show);
static void launcher_toggle(void);
static bool launcher_keys(int key);
static bool motion_pointer(bool pressed, bool down, bool touching);
static void motion_tick(uint64_t now);
static bool motion_render(void);
static void compositor_glass_parallel(GpuRect);
static void compositor_glass_stage(unsigned);
static void settings_page_tick(uint64_t), settings_page_reset(void);
static void thumbnail_capture(int app);
static void draw_dock(void);
static int dock_app_at(int, int), dock_center(int);
static bool dock_pointer(bool, bool, bool);
static void dock_load(void);
static bool launcher_button_at(int, int);
static void dock_tick(uint64_t);
static void drag_begin(int);
static bool drag_render(void);
static void compose_scene(int skip);
static void motion_cancel(void);
static void window_cache_begin(bool begin);
static void window_cache_before(int app);
static void window_cache_after(int app);
static int resize_app = -1, resize_start_x, resize_start_y, resize_start_w, resize_start_h,
           resize_origin_x, resize_origin_y, resize_edges;
static int opacity = 170, wall = 0, selected = -1, term_scroll = 0, file_scroll = 0;
static int pointer_speed = 100, pointer_scale = 1, utc_offset = 8;
static bool reduced_transparency = false;
static uint32_t ink = 0x24324a, muted = 0x5e7086, accent = 0x1769d4;
static char folder[128] = "/home/ark", command[512], dialog[128];
static size_t command_len, command_cursor, dialog_len;
static bool dialog_select_all;
static bool listing_failed;
static int history_age = -1, dialog_kind = 0;
static char file_drag_path[128];
static int file_drag_x, file_drag_y;
static bool file_drag_candidate;
static uint64_t file_click_at;
static int file_click_index = -1;
static char message[160] = "欢迎来到 ArkOS";
static int message_ticks = 0;
static const char *builtin_titles[19] = {
    "终端",   "文件",       "文本编辑", "系统设置",   "关于 ArkOS", "计算器",     "浏览器",
    "时钟",   "画板",       "Markdown", "任务管理器", "屏幕录制",   "安装 ArkOS", "待办事项",
    "计时器", "应用运行器", "包管理器", "日历",       "提醒事项"};
static const char *builtin_eng_titles[19] = {
    "Terminal", "Files", "Notes",    "Settings", "About",    "Calculator", "Browser",
    "Clock",    "Paint", "Markdown", "Tasks",    "Capture",  "Installer",  "Todo",
    "Timer",    "WASM",  "Packages", "Calendar", "Reminders"};
static const char **titles, **eng_titles;
typedef struct {
    int x, y, w, h, ox, oy, ow, oh;
    bool visible, maxed;
} Window;
static Window *windows;
static int native_window = -1;
static Window *app_window(int app) {
    return &windows[native_window >= 0 && window_app(native_window) == app ? native_window : app];
}
static int settings_category = 4, settings_scroll, settings_subpage;
static char settings_search[64];
static size_t settings_search_len;
static bool settings_search_active;
typedef struct {
    ShellContext *shell;
    char command[512], folder[128];
    size_t length, cursor;
    int selected, term_scroll, file_scroll, history_age, category, subpage, settings_scroll;
    char search[64];
    bool settings_initialized, search_active;
} NativeContext;
static NativeContext *native_contexts;
static int context_window = -1;
static void window_context(int);
static bool session_active(void), session_overlay_visible(void), session_boot_visible(void);
static void session_init(void), session_lock(void), session_tick(uint64_t),
    session_open_users(void);
static bool session_key(int), session_pointer(bool), session_render(void),
    session_display_editing(void);
static void session_users_draw(Window *), session_page_changed(int, int, int),
    session_display_insert(const char *), session_password_keyboard_draw(void);
static bool session_users_scroll(int, int);
static void session_window_closed(int), session_window_focused(int);
static void desktop_session_changed(void);
static bool thirdparty_request_event(int, ArkEvent);
static bool thirdparty_covers_window(int);
static bool thirdparty_open(int), thirdparty_app(int), thirdparty_open_argument(int, const char *);
static void permission_tick(uint64_t), permission_draw(void);
static bool permission_key(int), permission_pointer(bool), permission_showing;
static void draw_thirdparty(int, Window *), thirdparty_key(int, int),
    thirdparty_pointer(int, int, int, bool), thirdparty_tick(uint64_t), thirdparty_close(int),
    thirdparty_reset(void);
static void draw_tasks(Window *), tasks_click(int, int), tasks_tick(uint64_t), tasks_scroll_by(int),
    tasks_open(void);
static bool tasks_key(int), task_search_active;
static void tasks_insert(const char *);
static void wallpaper_draw(void), wallpaper_tick(uint64_t);
static ArkPackageInfo *package_entries;
static void package_ui_refresh(void), package_ui_draw(Window *), package_ui_click(int, int),
    package_ui_source(const char *);
static bool package_ui_key(int);
static void package_ui_scroll(int);
static bool desktop_app_available(int);
static void topbar_draw(void), topbar_overlay(void), topbar_tick(uint64_t), topbar_reset(void);
static bool topbar_pointer(bool), topbar_key(int);
static void topbar_background_region(int, int, int *, int *);
static void native_draw_app(int, Window *), native_click(int, int, int), native_open(int),
    native_tick(uint64_t), native_reset(void);
static bool native_key(int), native_pointer(bool, bool, bool, bool, bool),
    native_island_pointer(bool);
static void native_background_region(int, int, int *, int *);
static void native_island_dot(void);
static void native_desktop_draw(void), native_overlay_draw(void), native_settings(int, int),
    native_settings_action(int), native_capture(bool);
static int caption_menu = -1;
static bool live_wallpaper = true;
static char user_home[128] = "/home/ark";
static int *order;
static int min(int a, int b) {
    return a < b ? a : b;
}
static int max(int a, int b) {
    return a > b ? a : b;
}
static int clamp(int n, int lo, int hi) {
    return max(lo, min(n, hi));
}
static bool in(int x, int y, int a, int b, int w, int h) {
    return x >= a && x < a + w && y >= b && y < b + h;
}
static uint32_t mix(uint32_t a, uint32_t b, unsigned alpha) {
    unsigned inv = 255 - alpha;
    return (((((a >> 16) & 255) * inv + ((b >> 16) & 255) * alpha) / 255) << 16) |
           (((((a >> 8) & 255) * inv + ((b >> 8) & 255) * alpha) / 255) << 8) |
           (((a & 255) * inv + (b & 255) * alpha) / 255);
}
static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int yy = max(0, y); yy < min(sh, y + h); yy++)
        for (int xx = max(0, x); xx < min(sw, x + w); xx++)
            canvas[yy * sw + xx] = c;
}
static unsigned rounded_coverage(int x, int y, int w, int h, int r) {
    return raster_round_coverage(x, y, w, h, r);
}
/* Exact floor(n/255) in two 16-bit color lanes. Products never exceed
 * 65025 per lane, so the correction cannot carry into the next channel. */
static inline uint32_t compositor_mix(uint32_t a, uint32_t b, unsigned alpha) {
    unsigned inv = 255 - alpha;
    uint32_t rb = (a & 0x00ff00ffu) * inv + (b & 0x00ff00ffu) * alpha;
    rb = (rb + 0x00010001u + ((rb >> 8) & 0x00ff00ffu)) >> 8;
    unsigned g = ((a >> 8) & 255) * inv + ((b >> 8) & 255) * alpha;
    return (rb & 0x00ff00ffu) | (((g + 1 + (g >> 8)) >> 8) << 8);
}
static int compositor_radius(int w, int h, int r) {
    return max(0, min(r, min(w / 2, h / 2)));
}
static void compositor_span(uint32_t *dst, int count, uint32_t color, unsigned alpha) {
    raster_blend_span(dst, count, color, alpha);
}
/* The supplied clip is already inside both the screen and the rectangle.
 * Only corner squares need the original 4x4 subpixel coverage calculation. */
static void compositor_round_region(int x, int y, int w, int h, int r, uint32_t c, unsigned alpha,
                                    int x0, int y0, int x1, int y1) {
    if (x0 >= x1 || y0 >= y1)
        return;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = canvas + yy * sw;
        int ay = yy - y;
        if (!r || (ay >= r && ay < h - r)) {
            compositor_span(row + x0, x1 - x0, c, alpha);
            continue;
        }
        int left = min(x1, max(x0, x + r)), right = max(left, min(x1, x + w - r));
        for (int xx = x0; xx < left; xx++) {
            unsigned a = rounded_coverage(xx - x, ay, w, h, r) * alpha / 255;
            if (a)
                row[xx] = a == 255 ? c : compositor_mix(row[xx], c, a);
        }
        compositor_span(row + left, right - left, c, alpha);
        for (int xx = right; xx < x1; xx++) {
            unsigned a = rounded_coverage(xx - x, ay, w, h, r) * alpha / 255;
            if (a)
                row[xx] = a == 255 ? c : compositor_mix(row[xx], c, a);
        }
    }
}
static void rr(int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    alpha = clamp(alpha, 0, 255);
    if (w <= 0 || h <= 0 || !alpha)
        return;
    compositor_round_region(x, y, w, h, compositor_radius(w, h, r), c, (unsigned)alpha, max(0, x),
                            max(0, y), min(sw, x + w), min(sh, y + h));
}
static void stroke(int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    alpha = clamp(alpha, 0, 255);
    if (w <= 0 || h <= 0 || !alpha)
        return;
    if (w <= 2 || h <= 2) {
        rr(x, y, w, h, r, c, alpha);
        return;
    }
    r = compositor_radius(w, h, r);
    int x0 = max(0, x), x1 = min(sw, x + w), edge = max(1, r);
    if (x0 >= x1)
        return;
    for (int yy = max(0, y); yy < min(sh, y + h); yy++) {
        int ay = yy - y;
        if (!ay || ay == h - 1) {
            compositor_round_region(x, y, w, h, r, c, (unsigned)alpha, x0, yy, x1, yy + 1);
            continue;
        }
        int rowedge = ay >= r && ay < h - r ? 1 : edge;
        int left = min(x1, max(x0, x + rowedge)), right = max(left, min(x1, x + w - rowedge));
        /* Between the two edge spans both coverages are exactly 255. */
        for (int part = 0; part < 2; part++) {
            int begin = part ? right : x0, end = part ? x1 : left;
            for (int xx = begin; xx < end; xx++) {
                unsigned outer = rounded_coverage(xx - x, ay, w, h, r);
                unsigned inner = rounded_coverage(xx - x - 1, ay - 1, w - 2, h - 2, max(0, r - 1));
                unsigned a = (outer > inner ? outer - inner : 0) * (unsigned)alpha / 255;
                if (a)
                    canvas[yy * sw + xx] = compositor_mix(canvas[yy * sw + xx], c, a);
            }
        }
    }
}
static void circle(int x, int y, int r, uint32_t c) {
    rr(x - r, y - r, 2 * r, 2 * r, r, c, 255);
}
static void circle_tint(int x, int y, int r, uint32_t c, unsigned alpha) {
    for (int yy = max(0, y - r); yy < min(sh, y + r); yy++)
        for (int xx = max(0, x - r); xx < min(sw, x + r); xx++) {
            unsigned hits = 0;
            for (int sy = 1; sy < 8; sy += 2)
                for (int sx = 1; sx < 8; sx += 2) {
                    int dx = (xx - x) * 8 + sx, dy = (yy - y) * 8 + sy;
                    hits += (unsigned)(dx * dx + dy * dy <= r * r * 64);
                }
            unsigned cover = (hits * 255 + 8) / 16 * alpha / 255;
            if (cover)
                canvas[(size_t)yy * sw + xx] =
                    compositor_mix(canvas[(size_t)yy * sw + xx], c, cover);
        }
}
static void line(int x, int y, int x2, int y2, uint32_t c) {
    int dx = x2 > x ? x2 - x : x - x2, sx = x < x2 ? 1 : -1, dy = -(y2 > y ? y2 - y : y - y2),
        sy = y < y2 ? 1 : -1, e = dx + dy;
    for (;;) {
        rect(x, y, 1, 1, c);
        if (x == x2 && y == y2)
            break;
        int e2 = e * 2;
        if (e2 >= dy) {
            e += dy;
            x += sx;
        }
        if (e2 <= dx) {
            e += dx;
            y += sy;
        }
    }
}
static void text(int x, int y, const char *s, uint32_t c, int scale) {
    unicode_draw(canvas, sw, sw, sh, x, y, s, c, scale);
}
static void label(int x, int y, const char *s, uint32_t c) {
    text(x, y, s, c, 1);
}
static void clip(int x, int y, const char *s, int width, uint32_t c) {
    char b[512];
    size_t n = utf8_fit(s, width);
    n = min((int)n, 511);
    memcpy(b, s, n);
    b[n] = 0;
    label(x, y, b, c);
}
static void num(int x, int y, uint64_t n, uint32_t c) {
    char b[24];
    uint_to_str(n, b);
    label(x, y, b, c);
}
static void cat(char *d, size_t cap, const char *s) {
    size_t n = strlen(d);
    if (n < cap)
        strcopy(d + n, s, cap - n);
}
static const char *base(const char *p) {
    const char *r = p;
    while (*p) {
        if (*p == '/' && p[1])
            r = p + 1;
        p++;
    }
    return r;
}
static void notice(const char *s) {
    strcopy(message, s, sizeof message);
    message_ticks = 400;
    dirty = true;
}
static void palette(void) {
    ink = night ? 0xeaf2ff : 0x24324a;
    muted = night ? 0xacc1d8 : 0x5e7086;
    accent = night ? 0x80c7ff : 0x1769d4;
}
static void make_wallpaper(void) {
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            uint32_t top = wall ? 0x33265c : 0x213f82, bottom = wall ? 0xecaaa1 : 0x95d1df;
            uint32_t c = mix(top, bottom, (unsigned)(y * 255 / sh));
            int dx = x - sw * 3 / 4, dy = y - sh / 3;
            int d = dx * dx + dy * dy;
            int rad = sw * 2 / 3;
            int t = clamp(255 - (int)((int64_t)d * 255 / ((int64_t)rad * rad)), 0, 255);
            c = mix(c, wall ? 0xbba4df : 0x6fa9f5, (unsigned)t * 3 / 4);
            int ridge = sh / 2 + (x - sw / 2) * (x - sw / 2) / max(1, sw * 4);
            if (y > ridge) {
                int a = clamp((y - ridge) * 240 / max(1, sh - ridge), 0, 230);
                c = mix(c, wall ? 0x534583 : 0x083b69, a);
            }
            if (night)
                c = mix(c, 0x081327, 90);
            wallpaper[y * sw + x] = c;
        }
    dirty = true;
}
#include "desktop_glass.inc"
static void shadow(int x, int y, int w, int h) {
    shadow_pending = false;
    /* Every layer fully covers this common interior. Compose its eight exact
     * integer blends through channel tables, retaining all corner/edge layers. */
    int cx0 = max(0, x + 24), cx1 = min(sw, x + w - 24), cy0 = max(0, y + 6),
        cy1 = min(sh, y + h + 8);
    if (w <= 48 || h <= 48 || cx0 >= cx1 || cy0 >= cy1) {
        for (int i = 16; i >= 2; i -= 2)
            rr(x - i / 2, y + 7 - i / 2, w + i, h + i, 24 + i / 2, 0x071c3c, 7);
        return;
    }
    if (!shadow_channels_ready) {
        const unsigned color[3] = {7, 28, 60};
        for (unsigned channel = 0; channel < 3; channel++)
            for (unsigned value = 0; value < 256; value++) {
                unsigned result = value;
                for (unsigned layer = 0; layer < 8; layer++)
                    result = (result * 248 + color[channel] * 7) / 255;
                shadow_channels[channel][value] = (uint8_t)result;
            }
        shadow_channels_ready = true;
    }
    for (int i = 16; i >= 2; i -= 2) {
        int ax = x - i / 2, ay = y + 7 - i / 2, aw = w + i, ah = h + i,
            r = compositor_radius(aw, ah, 24 + i / 2);
        int x0 = max(0, ax), x1 = min(sw, ax + aw), y0 = max(0, ay), y1 = min(sh, ay + ah);
        compositor_round_region(ax, ay, aw, ah, r, 0x071c3c, 7, x0, y0, x1, min(y1, cy0));
        compositor_round_region(ax, ay, aw, ah, r, 0x071c3c, 7, x0, max(y0, cy1), x1, y1);
        compositor_round_region(ax, ay, aw, ah, r, 0x071c3c, 7, x0, max(y0, cy0), min(x1, cx0),
                                min(y1, cy1));
        compositor_round_region(ax, ay, aw, ah, r, 0x071c3c, 7, max(x0, cx1), max(y0, cy0), x1,
                                min(y1, cy1));
    }
    /* The next glass replaces this opaque interior. Apply its exact eight
     * shadow blends only at blur sample positions, retaining exposed rows below
     * the panel and every rounded corner/edge in the framebuffer. */
    for (int yy = max(cy0, y + h); yy < cy1; yy++)
        for (int xx = cx0; xx < cx1; xx++)
            canvas[yy * sw + xx] = shadow_color(canvas[yy * sw + xx]);
    shadow_x0 = cx0;
    shadow_x1 = cx1;
    shadow_y0 = cy0;
    shadow_y1 = min(cy1, y + h);
    shadow_pending = true;
}
static void icon(int app, int x, int y, int size) {
    ArkUISurface surface = arkui_surface(canvas, sw, sw, sh);
    arkui_app_icon(&surface, x, y, size,
                   app >= (int)ARK_PACKAGE_DESKTOP_FIRST ? ARKUI_APP_PACKAGE_APP
                                                         : (ArkUIAppIcon)app);
}
static void symbol(int x, int y, int size, ArkUISymbol kind, uint32_t color) {
    ArkUISurface surface = arkui_surface(canvas, sw, sw, sh);
    arkui_symbol(&surface, x, y, size, kind, color);
}
static int focus(void) {
    for (int i = WINS - 1; i >= 0; i--)
        if (windows[order[i]].visible)
            return order[i];
    return -1;
}
static int focused_app(void) {
    return window_app(focus());
}
static bool *keyboard_fit;
static int *keyboard_old_y, *keyboard_old_h;
static void fit_keyboard_window(int a) {
    if (a < 0 || !keyboard || (window_app(a) != 0 && window_app(a) != 2) || keyboard_fit[a])
        return;
    Window *w = &windows[a];
    keyboard_old_y[a] = w->y;
    keyboard_old_h[a] = w->h;
    keyboard_fit[a] = true;
    w->y = 58;
    w->h = max(260, min(w->h, sh - 344));
}
static void keyboard_set(bool visible) {
    keyboard = visible;
    if (visible)
        fit_keyboard_window(focus());
    else
        for (int a = 0; a < WINS; a++)
            if (keyboard_fit[a]) {
                windows[a].y = keyboard_old_y[a];
                windows[a].h = keyboard_old_h[a];
                keyboard_fit[a] = false;
            }
    dirty = true;
}
static void settings_cancel_pointer(void);
static void activate(int a) {
    if (a < 0 || a >= WINS || window_app(a) < 0)
        return;
    if (window_app(a) != 3)
        settings_cancel_pointer();
    window_context(a);
    session_window_focused(a);
    minimized[a] = false;
    int n = 0;
    while (n < WINS && order[n] != a)
        n++;
    for (int i = n; i < WINS - 1; i++)
        order[i] = order[i + 1];
    order[WINS - 1] = a;
    windows[a].visible = true;
    fit_keyboard_window(a);
    dirty = true;
}
static int latest_window(int app) {
    for (int i = WINS - 1; i >= 0; i--) {
        int w = order[i];
        if (window_app(w) == app && (windows[w].visible || minimized[w]))
            return w;
    }
    return app;
}
static void open_app(int a) {
    if (a < 0 || a >= APPS || !session_active() || !desktop_app_available(a))
        return;
    int w = latest_window(a);
    if (thirdparty_app(w) && !thirdparty_open(w))
        return;
    bool opening = !windows[w].visible;
    activate(w);
    if (a == 10)
        tasks_open();
    if (a >= 11)
        native_open(a);
    if (opening)
        window_transition(w, true);
    serial_write("[ui] open ");
    serial_write(eng_titles[a]);
    serial_write("\n");
}

static int desktop_bottom(void) {
    return keyboard ? sh - 282 : sh - 112;
}
static void button(int x, int y, int w, const char *s, bool primary) {
    unsigned before = click_region_count;
    click_region(x, y, w, 36);
    if (click_region_count > before)
        click_regions[click_region_count - 1].liquid = true;
    int h = 36;
    float progress = 0;
    bool interactive = glass_button_transform(&x, &y, &w, &h, &progress);
    if (interactive)
        glass_edge(x, y, w, h, h / 2, 90, 80);
    rr(x, y, w, h, h / 2, primary ? 0x277ade : (night ? 0x7791ae : 0xffffff), primary ? 230 : 120);
    if (interactive)
        glass_button_highlight(x, y, w, h, progress);
    label(x + (w - utf8_width(s)) / 2, y + (h - 20) / 2, s, primary ? 0xffffff : ink);
}
static void parent(char *p) {
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] != '/')
        n--;
    if (n > 1)
        n--;
    p[n] = 0;
    if (!p[0])
        strcopy(p, "/", 128);
}
static void path_join(char *out, const char *dir, const char *name) {
    strcopy(out, dir, 128);
    if (strcmp(dir, "/"))
        cat(out, 128, "/");
    cat(out, 128, name);
}
static bool child_of(const char *path, const char *dir) {
    size_t n = strlen(dir);
    if (!strcmp(dir, "/")) {
        if (path[0] != '/')
            return false;
        n = 0;
    } else if (strncmp(path, dir, n) || path[n] != '/')
        return false;
    const char *p = path + n + 1;
    if (!*p)
        return false;
    while (*p)
        if (*p++ == '/')
            return false;
    return true;
}
static int file_list[VFS_EXT_BASE + EXTFS_MAX_ENTRIES], file_count;
static uint64_t file_scanned_at;
static char file_scanned_path[128];
static void window_context(int w) {
    if (w == context_window) {
        native_window = w;
        return;
    }
    if (context_window >= 0) {
        NativeContext *c = &native_contexts[context_window];
        strcopy(c->command, command, 512);
        c->length = command_len;
        c->cursor = command_cursor;
        strcopy(c->folder, folder, 128);
        c->selected = selected;
        c->term_scroll = term_scroll;
        c->file_scroll = file_scroll;
        c->history_age = history_age;
        if (window_app(context_window) == 3) {
            c->category = settings_category;
            c->subpage = settings_subpage;
            c->settings_scroll = settings_scroll;
            strcopy(c->search, settings_search, 64);
            c->search_active = settings_search_active;
            c->settings_initialized = true;
        }
    }
    context_window = w;
    native_window = w;
    if (w < 0)
        return;
    NativeContext *c = &native_contexts[w];
    if (!c->folder[0]) {
        strcopy(c->folder, user_home, 128);
        c->selected = -1;
        c->history_age = -1;
    }
    strcopy(command, c->command, 512);
    command_len = c->length;
    command_cursor = c->cursor;
    strcopy(folder, c->folder, 128);
    selected = c->selected;
    term_scroll = c->term_scroll;
    file_scroll = c->file_scroll;
    history_age = c->history_age;
    if (window_app(w) == 3) {
        if (!c->settings_initialized) {
            c->category = 4;
            c->settings_initialized = true;
        }
        settings_category = c->category;
        settings_subpage = c->subpage;
        settings_scroll = c->settings_scroll;
        strcopy(settings_search, c->search, 64);
        settings_search_len = strlen(settings_search);
        settings_search_active = c->search_active;
    }
    if (window_app(w) == 1)
        refresh_files_visible();
    if (window_app(w) == 0) {
        if (!c->shell) {
            c->shell = ark_alloc(sizeof *c->shell);
            if (c->shell) {
                memset(c->shell, 0, sizeof *c->shell);
                shell_switch(c->shell);
                shell_init(&boot);
            }
        }
        shell_switch(c->shell);
    }
}
static void refresh_files(void) {
    listing_failed = !vfs_list(folder);
    file_scanned_at = ark_ticks();
    strcopy(file_scanned_path, folder, 128);
    file_count = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < vfs_entry_limit(); i++) {
            VFile *f = vfs_entry(i);
            if (f && f->used && f->is_dir == (pass == 0) && child_of(f->name, folder)) {
                if (file_count < (int)(sizeof file_list / sizeof file_list[0]))
                    file_list[file_count++] = i;
            }
        }
    }
    bool found = false;
    for (int i = 0; i < file_count; i++)
        if (file_list[i] == selected)
            found = true;
    if (!found)
        selected = -1;
}
static void refresh_files_visible(void) {
    if (strcmp(file_scanned_path, folder) || ark_ticks() - file_scanned_at >= 100)
        refresh_files();
}
static void files_open_path(const char *path) {
    char destination[128];
    strcopy(destination, path, 128);
    window_context(latest_window(1));
    strcopy(folder, destination, 128);
    selected = -1;
    file_scroll = 0;
    refresh_files();
    open_app(1);
}
static void load_note(int i) {
    if (i < 0 || !vfs_entry(i) || !vfs_entry(i)->used)
        return;
    VFile *f = vfs_entry(i);
    if (f->is_dir) {
        files_open_path(f->name);
        return;
    }
    size_t n = strlen(f->name);
    if (n >= 7 && !strcmp(f->name + n - 7, ".arkpkg")) {
        package_ui_source(f->name);
        open_app(16);
        return;
    }
    new_window_argument(2, f->name);
}
static int wrap_count(const char *s, int width) {
    int n = 0;
    while (*s) {
        size_t b = utf8_fit(s, width);
        if (!b)
            b = 1;
        s += b;
        n++;
    }
    return max(1, n);
}
/* Blink only the caret pixels; the glass compositor is never a timer task. */
static GpuRect caret_box;
static uint32_t caret_under[64];
static bool caret_valid, caret_visible;
static void draw_caret(int x, int y, int h) {
    if (dialog_kind || x < 0 || y < 0 || x + 2 > sw || y + h > sh || h > 32)
        return;
    caret_box = (GpuRect){x, y, 2, h};
    caret_valid = true;
    caret_visible = (platform_ticks() / 50) % 2 == 0;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < 2; xx++)
            caret_under[yy * 2 + xx] = canvas[(y + yy) * sw + x + xx];
    if (caret_visible)
        rect(x, y, 2, h, accent);
}
static void present_caret(void) {
    if (!caret_valid || dialog_kind || (focused_app() != 0 && focused_app() != 2))
        return;
    bool show = (platform_ticks() / 50) % 2 == 0;
    if (show == caret_visible)
        return;
    caret_visible = show;
    for (int y = 0; y < caret_box.h; y++)
        for (int x = 0; x < 2; x++)
            canvas[(caret_box.y + y) * sw + caret_box.x + x] =
                show ? accent : caret_under[y * 2 + x];
    gpu_present(canvas, (unsigned)sw, &caret_box);
    cursor_dirty = true;
}
static void draw_terminal(Window *w) {
    int x = w->x + 18, y = w->y + 70, width = w->w - 36, rows = max(1, (w->h - 88) / LINE),
        total = 0;
    char input[672];
    strcopy(input, shell_prompt(), sizeof input);
    cat(input, sizeof input, command);
    for (int i = 0; i < shell_line_count; i++)
        total += wrap_count(shell_lines[i], width);
    int output_rows = total;
    total += wrap_count(input, width);
    term_scroll = clamp(term_scroll, 0, max(0, total - rows));
    int start = max(0, total - rows - term_scroll), row = 0;
    for (int i = 0; i <= shell_line_count; i++) {
        const char *p = i == shell_line_count ? input : shell_lines[i];
        int chunks = wrap_count(p, width);
        size_t offset = 0;
        for (int j = 0; j < chunks; j++, row++) {
            size_t bytes = utf8_fit(p, width);
            if (!bytes && *p)
                bytes = 1;
            if (row >= start && row < start + rows) {
                char line_text[672];
                memcpy(line_text, p, bytes);
                line_text[bytes] = 0;
                label(x, y + (row - start) * LINE, line_text, i == shell_line_count ? accent : ink);
                if (i == shell_line_count && focused_app() == 0 && native_window == focus()) {
                    size_t caret = strlen(shell_prompt()) + command_cursor;
                    if (caret >= offset && caret <= offset + bytes) {
                        char prefix[672];
                        size_t n = caret - offset;
                        memcpy(prefix, p, n);
                        prefix[n] = 0;
                        draw_caret(x + utf8_width(prefix), y + (row - start) * LINE + 2, 19);
                    }
                }
            }
            p += bytes;
            offset += bytes;
        }
    }
    (void)output_rows;
}

/* Native ArkUI toolbar: layout is shared with the public reusable component.
 * Stable action IDs match the existing Files hit regions and commands below. */
static ArkUI files_toolbar_ui;
static const char *draw_files_toolbar(Window *w) {
    ArkUI *ui = &files_toolbar_ui;
    arkui_init(ui);
    int bar = arkui_toolbar(ui, 0, 0, 4);
    arkui_size(ui, bar, ARKUI_FILL, 36);
    arkui_material(ui, bar, ARKUI_MATERIAL_NONE); /* Window already supplies live glass. */
    const ArkUISymbol kinds[] = {
        ARKUI_SYMBOL_BACK, ARKUI_SYMBOL_NEW_DOCUMENT, ARKUI_SYMBOL_FOLDER, ARKUI_SYMBOL_RENAME,
        ARKUI_SYMBOL_COPY, ARKUI_SYMBOL_TRASH,        ARKUI_SYMBOL_FORWARD};
    const char *names[] = {"上一级", "新建文件", "新建文件夹", "重命名", "复制", "删除", "打开"};
    for (int i = 0; i < 7; i++) {
        if (i == 3)
            arkui_spacer(ui, bar, 1);
        int node = arkui_icon_button(ui, bar, kinds[i], names[i], 710 + i);
        if (i >= 3 && selected < 0)
            arkui_node(ui, node)->enabled = false;
    }
    arkui_layout(ui, (ArkUIRect){w->x + 198, w->y + 68, w->w - 220, 36});
    for (int i = 0; i < ui->count; i++)
        if (ui->nodes[i].action && ui->nodes[i].hit_enabled) {
            ArkUIRect r = ui->nodes[i].frame;
            click_region(r.x, r.y, r.w, r.h);
        }
    if (focused_app() == 1)
        arkui_pointer(ui, mx, my, false, 0);
    ArkUISurface surface = arkui_surface(canvas, sw, sw, sh);
    ArkUIPainter painter = arkui_canvas_painter(&surface);
    ArkUITheme theme = arkui_theme(night);
    arkui_draw(ui, &painter, &theme);
    return arkui_hover_label(ui);
}
static void draw_files(Window *w) {
    int side = 180, x = w->x + side + 18, y = w->y + 68;
    rr(w->x + 1, w->y + 54, side, w->h - 55, 14, night ? 0x61768f : 0xd6e2f0, 72);
    label(w->x + 23, w->y + 79, "个人收藏", muted);
    char documents[128];
    path_join(documents, user_home, "Documents");
    const char *names[] = {"个人文件", "文稿", "磁盘", "FAT32", "NTFS"};
    const char *paths[] = {user_home, documents, "/", "/mnt/fat32", "/mnt/ntfs"};
    for (int i = 0; i < 5; i++) {
        int yy = w->y + 115 + i * 44;
        click_region(w->x + 11, yy - 7, 158, 36);
        if (!strcmp(folder, paths[i]))
            rr(w->x + 11, yy - 7, 158, 36, 9, accent, 38);
        symbol(w->x + 22, yy, 20,
               i == 0   ? ARKUI_SYMBOL_HOME
               : i == 1 ? ARKUI_SYMBOL_DOCUMENT
                        : ARKUI_SYMBOL_FOLDER,
               i == 4 ? muted : accent);
        label(w->x + 53, yy, names[i], ink);
    }
    label(w->x + 23, w->y + w->h - 97, "位置", muted);
    symbol(w->x + 23, w->y + w->h - 60, 20, ARKUI_SYMBOL_FOLDER, accent);
    label(w->x + 52, w->y + w->h - 60, "Ark 磁盘", ink);
    const char *toolbar_hint = draw_files_toolbar(w);
    clip(x + 135, y + 8, base(folder), max(50, w->w - side - 382), ink);
    rect(x, y + 50, w->w - side - 37, 1, night ? 0x53637a : 0xdce4ee);
    int rows = max(1, (w->h - 176) / 40), selrow = 0;
    refresh_files_visible();
    for (int i = 0; i < file_count; i++)
        if (file_list[i] == selected)
            selrow = i;
    (void)selrow;
    file_scroll = clamp(file_scroll, 0, max(0, file_count - rows));
    for (int row = file_scroll; row < min(file_count, file_scroll + rows); row++) {
        int i = file_list[row], yy = y + 66 + (row - file_scroll) * 40;
        VFile *f = vfs_entry(i);
        click_region(x - 5, yy - 5, w->w - side - 30, 36);
        if (i == selected)
            rr(x - 5, yy - 5, w->w - side - 30, 36, 8, accent, night ? 72 : 30);
        symbol(x + 4, yy + 1, 23, f->is_dir ? ARKUI_SYMBOL_FOLDER : ARKUI_SYMBOL_DOCUMENT,
               f->is_dir ? accent : muted);
        clip(x + 39, yy + 3, base(f->name), w->w - side - 158, ink);
        if (f->is_dir)
            label(w->x + w->w - 81, yy + 3, "文件夹", muted);
        else {
            num(w->x + w->w - 96, yy + 3, f->size, muted);
            label(w->x + w->w - 36, yy + 3, "B", muted);
        }
    }
    if (!file_count)
        label(x + 20, y + 123, listing_failed ? "无法访问这个位置" : "这个文件夹是空的", muted);
    clip(x, w->y + w->h - 32, *toolbar_hint ? toolbar_hint : folder, w->w - side - 155,
         *toolbar_hint ? accent : muted);
    num(w->x + w->w - 95, w->y + w->h - 32, file_count, muted);
    label(w->x + w->w - 64, w->y + w->h - 32, "个项目", muted);
}
/* Settings is a real ArkUI view tree. The tree may be rebuilt between frames;
 * persistent state stays in bindings and pointer capture uses stable IDs. */
static ArkUI settings_ui;
static bool settings_initialized;
static void dock_settings(int);
static void dock_settings_action(int);
static char settings_account[64];
static ArkUIRect settings_viewport;
static void settings_cancel_pointer(void) {
    if (settings_initialized)
        arkui_cancel_pointer(&settings_ui);
}
static const char *settings_categories[] = {"外观",   "鼠标与键盘", "显示器",   "存储空间",
                                            "通用",   "网络",       "应用权限", "辅助功能",
                                            "快捷键", "安装与设备"};

static void settings_paint_rect(void *context, ArkUIRect r, int radius, uint32_t color, int alpha,
                                ArkUIRect clipping) {
    (void)context;
    ArkUIRect region = arkui_intersection(arkui_intersection(clipping, r), settings_viewport);
    if (alpha > 0)
        compositor_round_region(r.x, r.y, r.w, r.h, compositor_radius(r.w, r.h, radius), color,
                                (unsigned)clamp(alpha, 0, 255), region.x, region.y,
                                region.x + region.w, region.y + region.h);
}
static void settings_paint_text(void *context, int x, int y, const char *value, uint32_t color,
                                ArkUIRect clipping) {
    (void)context;
    ArkUIRect region = arkui_intersection(clipping, settings_viewport);
    if (region.w > 0 && region.h > 0)
        unicode_draw(canvas + (size_t)region.y * (size_t)sw + (size_t)region.x, sw, region.w,
                     region.h, x - region.x, y - region.y, value, color, 1);
}
static int settings_text(int parent_id, const char *value, bool secondary) {
    int node = arkui_text(&settings_ui, parent_id, value);
    ArkUINode *view = arkui_node(&settings_ui, node);
    if (view)
        view->secondary = secondary;
    return node;
}
static int settings_button(int parent_id, const char *title, int action, bool selected_state) {
    int node = arkui_button(&settings_ui, parent_id, title, action);
    arkui_flex(&settings_ui, node, 1);
    ArkUINode *view = arkui_node(&settings_ui, node);
    if (view)
        view->selected = selected_state;
    return node;
}
static void settings_info(int parent_id, const char *title, const char *value) {
    int row = arkui_hstack(&settings_ui, parent_id, 0, 10);
    arkui_size(&settings_ui, settings_text(row, title, true), 96, 22);
    arkui_size(&settings_ui, settings_text(row, value, false), ARKUI_FILL, 22);
}
static void settings_number(char out[64], uint64_t value, const char *suffix) {
    uint_to_str(value, out);
    cat(out, 64, suffix);
}
static void settings_paint_symbol(void *context, int x, int y, int size, ArkUISymbol glyph,
                                  uint32_t color, ArkUIRect clipping) {
    (void)context;
    ArkUIRect region = arkui_intersection(clipping, settings_viewport);
    if (region.w <= 0 || region.h <= 0)
        return;
    ArkUISurface surface =
        arkui_surface(canvas + (size_t)region.y * sw + region.x, sw, region.w, region.h);
    arkui_symbol(&surface, x - region.x, y - region.y, size, glyph, color);
}
static int settings_card(int parent_id) {
    return arkui_card(&settings_ui, parent_id, 14, 8);
}
static void settings_build(Window *w) {
    if (!settings_initialized) {
        arkui_init(&settings_ui);
        settings_initialized = true;
    } else
        arkui_reset(&settings_ui);
    int content = arkui_vstack(&settings_ui, 0, 0, 12);
    arkui_size(&settings_ui, content, ARKUI_FILL, ARKUI_AUTO);
    char value[128], number[64];
    if ((settings_category == 0 || settings_category == 1 || settings_category == 4) &&
        !settings_subpage) {
        int card = settings_card(content);
        if (settings_category == 0) {
            arkui_navigation(&settings_ui, card, "颜色与主题", ARKUI_SYMBOL_SETTINGS, 1001);
            arkui_navigation(&settings_ui, card, "桌面壁纸", ARKUI_SYMBOL_GRID, 1002);
            arkui_navigation(&settings_ui, card, "透明度与动态效果", ARKUI_SYMBOL_MORE, 1003);
            arkui_navigation(&settings_ui, card, "程序坞", ARKUI_SYMBOL_GRID, 1004);
        } else if (settings_category == 1) {
            arkui_navigation(&settings_ui, card, "鼠标与指针", ARKUI_SYMBOL_POINTER, 1011);
            arkui_navigation(&settings_ui, card, "键盘与中文输入", ARKUI_SYMBOL_DOCUMENT, 1012);
        } else {
            arkui_navigation(&settings_ui, card, "关于本机", ARKUI_SYMBOL_MORE, 1041);
            arkui_navigation(&settings_ui, card, "日期与时间", ARKUI_SYMBOL_SETTINGS, 1042);
            arkui_navigation(&settings_ui, card, "用户与账户", ARKUI_SYMBOL_USER, 1043);
            arkui_navigation(&settings_ui, card, "应用与存储", ARKUI_SYMBOL_FOLDER, 1044);
            arkui_navigation(&settings_ui, card, "启动与电源", ARKUI_SYMBOL_POWER, 1045);
        }
    } else if (settings_category == 0) {
        int card = settings_card(content);
        if (settings_subpage == 1) {
            int choices = arkui_hstack(&settings_ui, card, 0, 10);
            settings_button(choices, "浅色", 201, !night);
            settings_button(choices, "深色", 202, night);
            settings_text(card, "应用会跟随所选外观模式。", true);
        } else if (settings_subpage == 2) {
            int choices = arkui_hstack(&settings_ui, card, 0, 10);
            settings_button(choices, "澄澈海湾", 205, wall == 0);
            settings_button(choices, "暮光山脊", 206, wall == 1);
            arkui_toggle(&settings_ui, card, "流动壁纸", &live_wallpaper, 208);
        } else if (settings_subpage == 4)
            dock_settings(content);
        else {
            arkui_slider(&settings_ui, card, "窗口底色浓度", &opacity, 70, 235, 203);
            arkui_toggle(&settings_ui, card, "减少透明效果", &reduced_transparency, 204);
            arkui_toggle(&settings_ui, card, "减少动态效果", &reduced_motion, 207);
            settings_text(card, "透明窗口会实时呈现背后的内容。", true);
        }
    } else if (settings_category == 1) {
        int card = settings_card(content);
        if (settings_subpage == 11) {
            arkui_slider(&settings_ui, card, "鼠标移动速度 (%)", &pointer_speed, 50, 200, 301);
            arkui_slider(&settings_ui, card, "指针大小", &pointer_scale, 1, 2, 302);
            settings_info(card, "输入设备", virtio_input_name());
        } else {
            arkui_toggle(&settings_ui, card, "屏幕键盘", &keyboard, 303);
            settings_text(card, "Ctrl+I 切换中文拼音与英文。", false);
            settings_text(card, "空格选词，数字选择候选；[ / ] 翻页。", true);
            settings_text(card, "中文输入可在文本应用和终端使用。", true);
            settings_text(card, "个人用词会保存在当前账户。", true);
        }
    } else if (settings_category == 2) {
        settings_text(content, "调整屏幕显示与动效流畅度。", true);
        int card = settings_card(content);
        uint_to_str((uint64_t)sw, value);
        cat(value, sizeof value, " × ");
        uint_to_str((uint64_t)sh, number);
        cat(value, sizeof value, number);
        settings_info(card, "分辨率", value);
        settings_info(card, "色彩", "数百万色");
        settings_info(card, "显示设备", "当前显示器");
        const GpuStats *stats = gpu_stats();
        settings_info(card, "图形加速", stats->accelerated ? "可用" : "暂不可用");
        card = settings_card(content);
        settings_text(card, "动效目标帧率", false);
        int rates = arkui_hstack(&settings_ui, card, 0, 8);
        settings_button(rates, "60", 451, frame_rate == 60);
        settings_button(rates, "120", 452, frame_rate == 120);
        settings_button(rates, "144", 453, frame_rate == 144);
        settings_button(rates, "240", 454, frame_rate == 240);
        settings_text(card, "实际流畅度取决于设备；目标帧率会自动保存。", true);
        settings_text(content, "重启时可在启动菜单中选择分辨率。", true);
    } else if (settings_category == 3) {
        settings_text(content, "查看磁盘容量，或保存当前更改。", true);
        int card = settings_card(content);
        if (storage_is_v2()) {
            settings_number(number, storage_used_bytes(), " B");
            settings_info(card, "已用", number);
            settings_number(number, storage_free_bytes(), " B");
            settings_info(card, "剩余", number);
        } else {
            settings_info(card, "系统磁盘",
                          storage_mounted() ? "读写 · 系统数据盘" : "未挂载 · 内存会话");
            settings_number(number, storage_used_bytes(), " B");
            settings_info(card, "已用", number);
            settings_info(card, "单文件", "16KB");
            settings_info(card, "条目", "64");
        }
        for (unsigned i = 0; i < 2; i++) {
            ExtVolumeInfo info = {0};
            bool known = extfs_volume_info(i, &info);
            card = settings_card(content);
            const char *name = i == 0 ? "FAT32" : "NTFS";
            settings_info(card, name,
                          known && info.mounted
                              ? (info.read_only ? "已挂载 · 只读" : "已挂载 · 读写")
                              : "未挂载");
            if (known && info.mounted) {
                settings_info(card, "挂载点", info.mountpoint);
                settings_number(number, info.capacity_bytes / (1024 * 1024), " MiB");
                settings_info(card, "卷容量", number);
            }
        }
        int actions = arkui_hstack(&settings_ui, content, 0, 10);
        settings_button(actions, "立即同步", 401, true);
        settings_button(actions, "打开文件", 402, false);
    } else if (settings_category >= 5) {
        native_settings(content, settings_category);
    } else {
        int card = settings_card(content);
        if (settings_subpage == 41) {
            settings_info(card, "系统", "ArkOS 0.13.0+mouse1");
            settings_number(number, boot.memory_mib, " MiB");
            settings_info(card, "内存", number);
            settings_info(card, "显示设备", gpu_backend_name());
            settings_info(card, "输入设备", virtio_input_name());
            settings_info(card, "系统磁盘", storage_mounted() ? "已连接" : "内存会话");
        } else if (settings_subpage == 42) {
            arkui_slider(&settings_ui, card, "时区偏移（小时）", &utc_offset, -12, 14, 501);
            settings_text(card, "菜单栏使用所选时区。", true);
            settings_button(card, "打开日历", 604, false);
        } else if (settings_subpage == 43 || settings_subpage == 46 || settings_subpage == 47) {
            /* Loadable kernel drivers. Listing needs only an active session;
             * installing and removing need SYSTEM plus an admin account, so
             * this page reports state and points at the dev command. */
            static ArkDriverInfo drivers[ARK_DRV_PAGE_MAX];
            static char names[ARK_DRV_PAGE_MAX][32];
            unsigned count = 0;
            for (unsigned i = 0; i < ARK_DRV_PAGE_MAX; i++) {
                ArkDriverRequest q = {0};
                q.op = ARK_DRV_LIST;
                q.index = i;
                if (ark_driver(&q) < 0)
                    break;
                drivers[count] = q.info;
                strcopy(names[count], q.info.name, sizeof names[0]);
                count++;
            }
            for (unsigned i = 0; i < count; i++) {
                char value[96];
                strcopy(value, drivers[i].state == ARK_DRV_STATE_LOADED ? "已加载" :
                                drivers[i].state == ARK_DRV_STATE_DISABLED ? "已停用" : "失败",
                        sizeof value);
                if (drivers[i].detail[0]) {
                    size_t at = strlen(value);
                    strcopy(value + at, " · ", sizeof value - at);
                    strcopy(value + strlen(value), drivers[i].detail,
                            sizeof value - strlen(value));
                }
                settings_info(card, names[i], value);
            }
            if (!count)
                settings_text(card, "尚未安装可加载驱动。", false);
            settings_text(card, "已安装驱动由内核清单校验后加载；安装或移除需要管理员账户，"
                                "请在终端执行 dev install /dev remove。", true);
            settings_text(card, "驱动在 Ring0 执行，只能来自 ARK_SYS_DRIVER 校验通过的 .arco 映像。",
                          true);
        } else if (settings_subpage == 44) {
            arkui_navigation(&settings_ui, card, "存储空间", ARKUI_SYMBOL_FOLDER, 103);
            arkui_navigation(&settings_ui, card, "应用权限", ARKUI_SYMBOL_LOCK, 106);
            settings_button(card, "打开包管理器", 511, true);
        } else {
            arkui_navigation(&settings_ui, card, "启动磁盘与安装", ARKUI_SYMBOL_FOLDER, 109);
            settings_button(card, "重新启动", 502, false);
            settings_button(card, "关闭系统", 503, false);
        }
    }
    int side = clamp(w->w / 4, 220, 270), x = w->x + side + 25, y = w->y + 207;
    settings_viewport = (ArkUIRect){x, y, w->w - side - 50, w->h - 224};
    arkui_layout(&settings_ui, settings_viewport);
    int height = max(settings_viewport.h, arkui_node(&settings_ui, 0)->measured_h);
    settings_scroll = clamp(settings_scroll, 0, max(0, height - settings_viewport.h));
    arkui_layout(&settings_ui, (ArkUIRect){x, y - settings_scroll, settings_viewport.w, height});
}
static bool settings_match(const char *title) {
    if (!settings_search[0])
        return true;
    for (const char *p = title; *p; p++)
        if (!strncmp(p, settings_search, strlen(settings_search)))
            return true;
    return false;
}
static void settings_sidebar(Window *w) {
    int side = clamp(w->w / 4, 220, 270), x = w->x + 14, y = w->y + 67;
    rr(w->x + 1, w->y + 54, side, w->h - 55, 18, night ? 0x253246 : 0xf4f5f8, 235);
    click_text_region(x + 6, y + 5, side - 40, 38);
    rr(x + 6, y + 5, side - 40, 38, 19, night ? 0x768398 : 0xdadce1,
       settings_search_active ? 100 : 70);
    symbol(x + 20, y + 15, 20, ARKUI_SYMBOL_SEARCH, muted);
    clip(x + 51, y + 14, settings_search[0] ? settings_search : "搜索", side - 95,
         settings_search[0] ? ink : muted);
    if (!settings_account[0]) {
        ArkAccountRequest q = {.op = ARK_ACCOUNT_STATUS};
        if (ark_account(&q) >= 0)
            strcopy(settings_account, q.display[0] ? q.display : q.user, sizeof settings_account);
    }
    click_region(w->x + 24, w->y + 136, side - 25, 55);
    rr(x + 10, y + 69, 46, 46, 23, accent, 32);
    symbol(x + 20, y + 79, 26, ARKUI_SYMBOL_USER, accent);
    clip(x + 69, y + 70, settings_account, side - 92, ink);
    label(x + 69, y + 95, "本机账户", muted);
    static const int order[] = {5, 4, 6, 0, 2, 1, 7, 3, 9, 8};
    static const int symbols[] = {
        ARKUI_SYMBOL_SETTINGS, ARKUI_SYMBOL_POINTER, ARKUI_SYMBOL_GRID, ARKUI_SYMBOL_FOLDER,
        ARKUI_SYMBOL_SETTINGS, ARKUI_SYMBOL_NETWORK, ARKUI_SYMBOL_LOCK, ARKUI_SYMBOL_USER,
        ARKUI_SYMBOL_DOCUMENT, ARKUI_SYMBOL_FOLDER};
    static const uint32_t colors[] = {0x414a62, 0x2b91e7, 0x289bdb, 0x7a8291, 0x89909b,
                                      0x198ddc, 0x794dde, 0x2e94df, 0x6b7cc0, 0x6d8496};
    int row = 0;
    for (int i = 0; i < 10; i++) {
        int c = order[i];
        if (!settings_match(settings_categories[c]))
            continue;
        int yy = y + 137 + row++ * 35;
        click_region(x + 5, yy, side - 25, 33);
        bool selected = settings_category == c;
        if (selected)
            rr(x + 5, yy, side - 25, 33, 10, accent, 245);
        rr(x + 16, yy + 6, 22, 22, 5, colors[c], 255);
        symbol(x + 19, yy + 9, 16, (ArkUISymbol)symbols[c], 0xffffff);
        clip(x + 47, yy + 7, settings_categories[c], side - 73, selected ? 0xffffff : ink);
    }
    if (!row)
        label(x + 17, y + 145, "没有找到设置", muted);
}
static const char *settings_page_title(void) {
    switch (settings_subpage) {
    case 1:
        return "颜色与主题";
    case 2:
        return "桌面壁纸";
    case 3:
        return "透明度与动态效果";
    case 4:
        return "程序坞";
    case 11:
        return "鼠标与指针";
    case 12:
        return "键盘与中文输入";
    case 41:
        return "关于本机";
    case 42:
        return "日期与时间";
    case 43:
        return "用户与账户";
    case 46:
        return "新建用户";
    case 47:
        return "修改密码";
    case 44:
        return "应用与存储";
    case 45:
        return "启动与电源";
    case 60:
        return "应用访问权限";
    default:
        return settings_categories[settings_category];
    }
}
static void settings_draw_page(Window *w) {
    if (settings_category == 4 &&
        (settings_subpage == 43 || settings_subpage == 46 || settings_subpage == 47)) {
        settings_build(w);
        session_users_draw(w);
        return;
    }
    settings_build(w);
    int side = clamp(w->w / 4, 220, 270), x = w->x + side + 25, width = w->w - side - 50,
        y = w->y + 68;
    rr(x, y, width, 124, 20, night ? 0x405069 : 0xffffff, 150);
    if (settings_subpage) {
        click_region(x + 8, y + 8, 36, 36);
        circle_tint(x + 26, y + 26, 18, night ? 0x768398 : 0xffffff, 90);
        symbol(x + 18, y + 17, 17, ARKUI_SYMBOL_BACK, ink);
    }
    icon(3, x + (width - 48) / 2, y + 8, 48);
    text(x + (width - utf8_width(settings_page_title()) * 2) / 2, y + 58, settings_page_title(),
         ink, 2);
    static const char *descriptions[] = {"选择喜欢的桌面样式",       "调整鼠标、指针和键盘",
                                         "查看屏幕与流畅度设置",     "查看磁盘与可用空间",
                                         "管理本机、账户和日常偏好", "查看连接与网络地址",
                                         "决定应用可以访问哪些内容", "让设备更适合你的使用方式",
                                         "查看常用键盘操作",         "查看设备并安装系统"};
    clip(x + (width - min(width - 30, utf8_width(descriptions[settings_category]))) / 2, y + 99,
         descriptions[settings_category], width - 30, muted);
    ArkUISurface surface = arkui_surface(canvas, sw, sw, sh);
    ArkUIPainter painter = arkui_canvas_painter(&surface);
    painter.rect = settings_paint_rect;
    painter.text = settings_paint_text;
    painter.symbol = settings_paint_symbol;
    ArkUITheme theme = arkui_theme(night);
    theme.card = night ? 0x3b485c : 0xf2f3f5;
    theme.card_alpha = 200;
    theme.control = night ? 0x5c6b83 : 0xe2e6ed;
    arkui_draw(&settings_ui, &painter, &theme);
}

#include "desktop_pages.inc"

static void draw_about(Window *w) {
    int x = w->x + 29, y = w->y + 80;
    icon(4, x, y, 64);
    text(x + 83, y + 2, "ArkOS", ink, 2);
    label(x + 86, y + 44, "0.13.0+mouse1 · Aurora", muted);
    label(x, y + 108, "为你的工作与生活，创造清晰的桌面。", ink);
    label(x, y + 154, "系统", muted);
    label(x + 83, y + 154, "64 位系统", ink);
    label(x, y + 190, "内存", muted);
    num(x + 83, y + 190, boot.memory_mib, ink);
    label(x + 140, y + 190, "MiB", muted);
    label(x, y + 226, "存储", muted);
    clip(x + 83, y + 226, storage_mounted() ? "系统磁盘已连接" : "使用内存会话", w->w - 141, ink);
    label(x, y + 262, "输入", muted);
    clip(x + 83, y + 262, "键盘、鼠标与触控", w->w - 141, ink);
    label(x, y + 310, "你的文件、应用与偏好，都在这里。", muted);
    label(x, y + 342, "F1 终端   F2 文件   F3 编辑   F4 设置", muted);
    button(x, y + 389, 132, "重新启动", false);
    button(x + 148, y + 389, 108, "关机", false);
}
static void draw_window(int wid) {
    Window *w = &windows[wid];
    if (!w->visible)
        return;
    click_owner = wid;
    window_context(wid);
    int a = window_app(wid);
    shadow(w->x, w->y, w->w, w->h);
    glass_opaque_client = thirdparty_covers_window(wid);
    glass(w->x, w->y, w->w, w->h, 20, opacity);
    glass_opaque_client = false;
    rr(w->x + 1, w->y + 54, w->w - 2, w->h - 55, 18, night ? 0x14243b : 0xf8fbff,
       a == 1 ? 135 : 190);
    click_region(w->x + 10, w->y + 9, 60, 36);
    rr(w->x + 10, w->y + 9, 60, 36, 18, night ? 0xa8bed9 : 0xffffff,
       in(mx, my, w->x + 10, w->y + 9, 60, 36) ? 95 : 45);
    icon(a, w->x + 18, w->y + 15, 24);
    symbol(w->x + 47, w->y + 20, 14, ARKUI_SYMBOL_DROPDOWN, ink);
    clip(w->x + 84, w->y + 18, titles[a], w->w - 244, ink);
    int cx = w->x + w->w - 142;
    rr(cx, w->y + 9, 128, 36, 18, night ? 0xa8bed9 : 0xffffff, 45);
    for (int b = 0; b < 3; b++) {
        int bx = cx + b * 44;
        click_region(bx, w->y + 9, 40, 36);
        bool hover = in(mx, my, bx, w->y + 9, 40, 36);
        if (hover)
            rr(bx, w->y + 9, 40, 36, 18, b == 2 ? 0xd55459 : accent, b == 2 ? 230 : 35);
        symbol(bx + 11, w->y + 17, 19,
               b == 0   ? ARKUI_SYMBOL_MINIMIZE
               : b == 1 ? ARKUI_SYMBOL_MAXIMIZE
                        : ARKUI_SYMBOL_CLOSE,
               hover && b == 2 ? 0xffffff : ink);
    }
    if (thirdparty_app(wid))
        draw_thirdparty(wid, w);
    else if (a == 0)
        draw_terminal(w);
    else if (a == 1)
        draw_files(w);
    else if (a == 3)
        draw_settings(w);
    else if (a == 4)
        draw_about(w);
    else if (a == 10)
        draw_tasks(w);
    else if (a >= 11)
        native_draw_app(a, w);
    if (!w->maxed) {
        line(w->x + w->w - 17, w->y + w->h - 7, w->x + w->w - 7, w->y + w->h - 17, muted);
        line(w->x + w->w - 12, w->y + w->h - 7, w->x + w->w - 7, w->y + w->h - 12, muted);
    }
    thumbnail_capture(wid);
}
static const char *keyboard_rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *symbol_rows[] = {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "`~01234567"};
static const char *keyboard_row(int row) {
    return keyboard_symbols ? symbol_rows[row] : keyboard_rows[row];
}
static char keyboard_char(int row, int index) {
    char c = keyboard_row(row)[index];
    return keyboard_upper && c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}
static void draw_keyboard(void) {
    if (!keyboard)
        return;
    int x = max(10, (sw - 820) / 2), y = sh - 270, w = min(sw - 20, 820);
    shadow(x, y, w, 258);
    glass(x, y, w, 258, 24, 225);
    label(x + 23, y + 16, ime_enabled ? "屏幕键盘 · 中文拼音" : "屏幕键盘 · English", ink);
    button(x + w - 86, y + 8, 64, "收起", false);
    button(x + w - 240, y + 8, 68, "Shift", keyboard_upper);
    button(x + w - 164, y + 8, 68, keyboard_symbols ? "ABC" : "符号", keyboard_symbols);
    int kw = (w - 55) / 10;
    for (int row = 0; row < 4; row++) {
        int count = (int)strlen(keyboard_row(row)), xx = x + (w - count * (kw + 4)) / 2;
        for (int i = 0; i < count; i++) {
            char b[2] = {keyboard_char(row, i), 0};
            button(xx + i * (kw + 4), y + 48 + row * 39, kw, b, false);
        }
    }
    button(x + 18, y + 211, 98, ime_enabled ? "ABC" : "中文", false);
    button(x + 126, y + 211, w - 364, "空格", false);
    button(x + w - 228, y + 211, 94, "退格", false);
    button(x + w - 124, y + 211, 104, "回车", true);
}
static void draw_dialog(void) {
    if (!dialog_kind)
        return;
    rr(0, 36, sw, sh - 36, 0, 0x102040, 65);
    int x = (sw - 480) / 2, y = (sh - 232) / 2;
    shadow(x, y, 480, 232);
    glass(x, y, 480, 232, 22, 238);
    const char *labels[] = {"",         "新建文件",       "新建文件夹", "重命名",
                            "复制文件", "删除这个项目？", "重新启动？", "关闭系统？"};
    label(x + 25, y + 25, labels[dialog_kind], ink);
    if (dialog_kind <= 4) {
        rr(x + 24, y + 76, 432, 48, 11, night ? 0x0b1d32 : 0xffffff, 235);
        if (dialog_select_all)
            rr(x + 34, y + 87, min(412, utf8_width(dialog) + 8), 26, 4, accent, 80);
        clip(x + 37, y + 90, dialog, 406, ink);
    } else
        clip(x + 25, y + 83,
             dialog_kind == 5 && selected >= 0 ? base((*vfs_entry(selected)).name)
                                               : "将先保存磁盘中的更改。",
             424, muted);
    button(x + 234, y + 167, 100, "取消", false);
    button(x + 345, y + 167, 110, "确定", true);
}
static GpuRect old_cursor;
static bool old_cursor_valid;
static uint32_t cursor_under[80 * 80];
static bool pointer_presenting;
static uint8_t cursor_alpha[2][64 * 64];
static int cursor_cached_scale, cursor_cached_shape = -1;
static GpuRect pointer_overlay(void) {
    int ox = mx - ark_cursor_hot_x(pointer_shape) * pointer_scale,
        oy = my - ark_cursor_hot_y(pointer_shape) * pointer_scale, size = 32 * pointer_scale;
    GpuRect box = {max(0, min(ox, mx - 17)), max(0, min(oy, my - 17)), 0, 0};
    box.w = min(sw, max(ox + size, mx + 17)) - box.x;
    box.h = min(sh, max(oy + size, my + 17)) - box.y;
    for (int y = 0; y < box.h; y++)
        memcpy(cursor_under + y * 80, canvas + (box.y + y) * sw + box.x, (size_t)box.w * 4);
    if (touch_down)
        stroke(mx - 14, my - 14, 28, 28, 14, 0xffffff, 230);
    else {
        if (cursor_cached_scale != pointer_scale || cursor_cached_shape != (int)pointer_shape) {
            for (int plane = 0; plane < 2; plane++)
                for (int y = 0; y < size; y++)
                    for (int x = 0; x < size; x++)
                        cursor_alpha[plane][y * 64 + x] = (uint8_t)raster_cursor_shape_coverage(
                            x, y, pointer_scale, plane, pointer_shape);
            cursor_cached_scale = pointer_scale;
            cursor_cached_shape = (int)pointer_shape;
        }
        for (int y = max(0, -oy); y < size && oy + y < sh; y++)
            for (int x = max(0, -ox); x < size && ox + x < sw; x++) {
                unsigned a = cursor_alpha[0][y * 64 + x],
                         b = min((int)a, cursor_alpha[1][y * 64 + x]);
                if (a) {
                    int at = (oy + y) * sw + ox + x;
                    canvas[at] = mix(canvas[at], 0x172337, a);
                    if (b)
                        canvas[at] = mix(canvas[at], 0xffffff, b);
                }
            }
    }
    return box;
}
static void pointer_restore(GpuRect box) {
    for (int y = 0; y < box.h; y++)
        memcpy(canvas + (box.y + y) * sw + box.x, cursor_under + y * 80, (size_t)box.w * 4);
}
static GpuRect presentation_pointer;
/* Keep a stationary software arrow in every submitted region that crosses
 * it. The cached scene remains clean after synchronous consumption. */
bool desktop_begin_present(const uint32_t *pixels) {
    if (pixels != canvas || pointer_presenting || !old_cursor_valid || session_boot_visible())
        return false;
    presentation_pointer = pointer_overlay();
    return true;
}
void desktop_end_present(void) {
    pointer_restore(presentation_pointer);
}
static void present_pointer(void) {
    unsigned shape = pointer_context();
    if (shape != pointer_shape) {
        pointer_shape = shape;
        gpu_cursor_set_shape(shape);
    }
    gpu_cursor_set_scale((unsigned)pointer_scale);
    GpuRect previous = old_cursor;
    bool had_previous = old_cursor_valid;
    if (gpu_cursor_move(mx, my, !touch_down) && !touch_down) {
        pointer_presenting = true;
        if (had_previous)
            gpu_present(canvas, (unsigned)sw, &previous);
        pointer_presenting = false;
        old_cursor_valid = false;
        cursor_dirty = false;
        return;
    }
    GpuRect box = pointer_overlay();
    /* Publish the new position before erasing the old one. Overlap is safe:
     * both updates read the canvas containing the complete new arrow. */
    pointer_presenting = true;
    gpu_present(canvas, (unsigned)sw, &box);
    if (had_previous)
        gpu_present(canvas, (unsigned)sw, &previous);
    pointer_presenting = false;
    pointer_restore(box);
    old_cursor = box;
    old_cursor_valid = true;
    cursor_dirty = false;
}
static void compose_scene(int skip) {
    click_region_count = 0;
    click_owner = -1;
    caret_valid = false;
    wallpaper_draw();
    topbar_draw();
    native_desktop_draw();
    for (int i = 0; i < WINS; i++)
        if (order[i] != skip) {
            window_cache_before(order[i]);
            draw_window(order[i]);
            window_cache_after(order[i]);
        }
    click_owner = -1;
    window_context(focus());
    draw_dock();
    draw_keyboard();
    draw_dialog();
    native_overlay_draw();
    topbar_overlay();
    ime_draw();
    transfer_draw();
    if (caption_menu >= 0) {
        Window *w = &windows[caption_menu];
        int x = w->x + 10, y = w->y + 51;
        shadow(x, y, 210, 144);
        glass(x, y, 210, 144, 16, 230);
        const char *items[] = {"最大化 / 还原", "贴靠左侧", "贴靠右侧", "关闭窗口"};
        for (int i = 0; i < 4; i++) {
            click_region(x, y + i * 36, 210, 36);
            if (in(mx, my, x, y + i * 36, 210, 36))
                rr(x + 4, y + i * 36 + 2, 202, 32, 9, accent, 30);
            label(x + 19, y + i * 36 + 8, items[i], ink);
        }
    }

    if (message_ticks > 0 && !dialog_kind) {
        int w = min(sw - 40, utf8_width(message) + 36), x = (sw - w) / 2;
        glass(x, sh - 145, w, 35, 12, 220);
        clip(x + 18, sh - 137, message, w - 36, ink);
    }
    permission_draw();
    session_password_keyboard_draw();
}
/* Software arrows are overlaid only during presentation. Any partial update
 * can erase them: repaint before the desktop next waits for an event. */
void desktop_presented(void) {
    if (!pointer_presenting && !session_boot_visible())
        cursor_dirty = true;
}
static void render(void) {
    if (session_overlay_visible()) {
        click_region_count = 0;
        click_owner = -1;
    }
    if (session_render()) {
        gpu_present_animation(canvas, (unsigned)sw);
        old_cursor_valid = false;
        if (!session_boot_visible())
            present_pointer();
        dirty = false;
        return;
    }
    if (motion_render())
        return;
    if (drag_render())
        return;
    window_cache_begin(true);
    compose_scene(-1);
    window_cache_begin(false);
    gpu_present(canvas, (unsigned)sw, 0);
    old_cursor_valid = false;
    present_pointer();
    dirty = false;
}
static const char *preference_keys[] = {
    "/user/appearance/dark",           "/user/appearance/wallpaper",
    "/user/appearance/opacity",        "/user/input/pointer-speed",
    "/user/input/pointer-scale",       "/user/appearance/reduced-transparency",
    "/user/appearance/reduced-motion", "/user/appearance/live-wallpaper",
    "/user/desktop/timezone",          "/user/input/keyboard",
    "/user/appearance/frame-rate"};
static int preference_saved[11];
static bool preference_known[11];
static uint64_t preferences_due;
static int preference_get(unsigned i) {
    int values[] = {night,          wall,           opacity,
                    pointer_speed,  pointer_scale,  reduced_transparency,
                    reduced_motion, live_wallpaper, utc_offset,
                    keyboard,       frame_rate};
    return values[i];
}
static bool config_integer(const char *key, int64_t value) {
    ArkRegistryRequest q = {.op = ARK_REG_SET, .type = ARK_REG_INTEGER, .integer = value};
    strcopy(q.key, key, 128);
    return ark_registry(&q) >= 0;
}
static bool settings_flush(uint64_t now, bool force) {
    if (!preferences_due || (!force && now < preferences_due))
        return true;
    preferences_due = 0;
    bool ok = true;
    for (unsigned i = 0; i < 11; i++) {
        int value = preference_get(i);
        if (preference_known[i] && preference_saved[i] == value)
            continue;
        if (config_integer(preference_keys[i], value)) {
            preference_saved[i] = value;
            preference_known[i] = true;
        } else
            ok = false;
    }
    if (!ok)
        notice("无法保存设置，请检查系统磁盘");
    return ok;
}
static void save_settings(void) {
    preferences_due = ark_millis() + 350;
    for (int w = 0; w < WINS; w++)
        if (thirdparty_app(w)) {
            ArkEvent e = {.type = ARK_EV_THEME, .key = night};
            (void)thirdparty_request_event(w, e);
        }
}
static bool settings_parse_number(const char *begin, const char *end, int *result) {
    bool negative = false;
    if (begin < end && *begin == '-') {
        negative = true;
        begin++;
    }
    if (begin == end)
        return false;
    int value = 0, digits = 0;
    while (begin < end) {
        if (*begin < '0' || *begin > '9' || ++digits > 6)
            return false;
        value = value * 10 + *begin++ - '0';
    }
    *result = negative ? -value : value;
    return true;
}
static void read_legacy_settings(void) {
    char legacy_path[128];
    path_join(legacy_path, user_home, ".arkcfg");
    int index = vfs_find(legacy_path);
    if (index < 0)
        return;
    const char *line = vfs_files[index].data;
    while (*line) {
        const char *end = line, *equal = 0;
        while (*end && *end != '\n') {
            if (*end == '=' && !equal)
                equal = end;
            end++;
        }
        int value;
        if (equal && settings_parse_number(equal + 1, end, &value)) {
            size_t length = (size_t)(equal - line);
            if (length == 5 && !strncmp(line, "theme", length))
                night = clamp(value, 0, 1) != 0;
            else if (length == 4 && !strncmp(line, "wall", length))
                wall = clamp(value, 0, 1);
            else if (length == 7 && !strncmp(line, "opacity", length))
                opacity = clamp(value, 70, 235);
            else if (length == 13 && !strncmp(line, "pointer_speed", length))
                pointer_speed = clamp(value, 50, 200);
            else if (length == 13 && !strncmp(line, "pointer_scale", length))
                pointer_scale = clamp(value, 1, 2);
            else if (length == 20 && !strncmp(line, "reduced_transparency", length))
                reduced_transparency = clamp(value, 0, 1) != 0;
            else if (length == 14 && !strncmp(line, "reduced_motion", length))
                reduced_motion = clamp(value, 0, 1) != 0;
            else if (length == 14 && !strncmp(line, "live_wallpaper", length))
                live_wallpaper = clamp(value, 0, 1) != 0;
            else if (length == 10 && !strncmp(line, "utc_offset", length))
                utc_offset = clamp(value, -12, 14);
            else if (length == 8 && !strncmp(line, "keyboard", length))
                keyboard = clamp(value, 0, 1) != 0;
            else if (length == 10 && !strncmp(line, "frame_rate", length) &&
                     (value == 60 || value == 120 || value == 144 || value == 240))
                frame_rate = value;
        }
        line = *end ? end + 1 : end;
    }
}
static void read_settings(void) {
    memset(preference_known, 0, sizeof preference_known);
    preferences_due = 0;
    read_legacy_settings();
    int values[11];
    static const int low[] = {0, 0, 70, 50, 1, 0, 0, 0, -12, 0, 60},
                     high[] = {1, 1, 235, 200, 2, 1, 1, 1, 14, 1, 240};
    for (unsigned i = 0; i < 11; i++) {
        values[i] = preference_get(i);
        ArkRegistryRequest q = {.op = ARK_REG_GET};
        strcopy(q.key, preference_keys[i], 128);
        if (ark_registry(&q) >= 0 && q.type == ARK_REG_INTEGER && q.integer >= low[i] &&
            q.integer <= high[i]) {
            if (i != 10 || q.integer == 60 || q.integer == 120 || q.integer == 144 ||
                q.integer == 240) {
                values[i] = (int)q.integer;
                preference_saved[i] = values[i];
                preference_known[i] = true;
            }
        }
    }
    night = values[0];
    wall = values[1];
    opacity = values[2];
    pointer_speed = values[3];
    pointer_scale = values[4];
    reduced_transparency = values[5];
    reduced_motion = values[6];
    live_wallpaper = values[7];
    utc_offset = values[8];
    keyboard = values[9];
    frame_rate = values[10];
    preferences_due = ark_millis();
    (void)settings_flush(ark_millis(), true);
}
static void save_now(void) {
    (void)settings_flush(ark_millis(), true);
    if (vfs_sync())
        notice(storage_mounted() ? "更改已写入磁盘" : "已同步可用卷 · 主目录仍为内存会话");
    else
        notice(vfs_error());
}
static void dialog_open(int kind, const char *value) {
    dialog_kind = kind;
    strcopy(dialog, value ? value : "", sizeof dialog);
    dialog_len = strlen(dialog);
    dialog_select_all = kind <= 4 && dialog_len;
    dirty = true;
}
static bool settings_pointer(int x, int y, bool down) {
    Window *w = app_window(3);
    int side = clamp(w->w / 4, 220, 270), sx = w->x + 20, sy = w->y + 72;
    if (settings_subpage && in(x, y, w->x + side + 33, w->y + 76, 36, 36)) {
        if (pointer_clicked) {
            settings_navigate(settings_category,
                              (settings_subpage == 46 || settings_subpage == 47) ? 43 : 0, -1);
            settings_build(w);
        }
        dirty = true;
        return true;
    }
    if (in(x, y, sx, sy, side - 40, 38)) {
        if (pointer_clicked)
            settings_search_active = true;
        dirty = true;
        return true;
    }
    if (in(x, y, w->x + 24, w->y + 136, side - 25, 55)) {
        if (pointer_clicked)
            session_open_users();
        return true;
    }
    if (in(x, y, w->x + 19, w->y + 204, side - 25, 350)) {
        if (pointer_clicked) {
            static const int order[] = {5, 4, 6, 0, 2, 1, 7, 3, 9, 8};
            int row = 0;
            for (int i = 0; i < 10; i++) {
                int c = order[i];
                if (!settings_match(settings_categories[c]))
                    continue;
                if (in(x, y, w->x + 19, w->y + 204 + row * 35, side - 25, 33)) {
                    settings_navigate(c, 0, 0);
                    settings_search_active = false;
                    settings_build(w);
                    break;
                }
                row++;
            }
        }
        dirty = true;
        return true;
    }
    if (pointer_clicked)
        settings_search_active = false;
    if (!in(x, y, settings_viewport.x, settings_viewport.y, settings_viewport.w,
            settings_viewport.h) &&
        !settings_ui.capture_action)
        return false;
    if (!settings_initialized)
        settings_build(app_window(3));
    ArkUIAction action;
    bool handled = arkui_pointer(&settings_ui, x, y, down, &action);
    if (arkui_take_dirty(&settings_ui))
        dirty = true;
    if (!action.changed)
        return handled;
    if (action.id >= 2000) {
        dock_settings_action(action.id);
        settings_build(w);
        return true;
    }
    if (action.id >= 1000 && action.id < 1100) {
        settings_navigate(settings_category, action.id - 1000, 1);
        settings_build(w);
        dirty = true;
        return true;
    }
    if (action.id >= 100 && action.id < 110) {
        settings_navigate(action.id - 100, 0, 0);
        /* Input reports may be drained in one batch before the next render.
         * Update hit boxes now so a quick second tap targets the new page. */
        settings_build(app_window(3));
    } else if (action.id >= 600) {
        native_settings_action(action.id);
    } else if (action.id == 401) {
        if (vfs_sync())
            notice(storage_mounted() ? "已同步挂载的数据卷" : "同步完成；系统偏好仍处于内存会话");
        else
            notice(vfs_error());
    } else if (action.id >= 451 && action.id <= 454) {
        const int rates[] = {60, 120, 144, 240};
        frame_rate = rates[action.id - 451];
        motion_frame_init(&frame_clock, platform_millis(), (unsigned)frame_rate);
        save_settings();
    } else if (action.id == 402) {
        files_open_path("/");
    } else if (action.id == 510) {
        open_app(4);
        return true;
    } else if (action.id == 511) {
        open_app(16);
        return true;
    } else if (action.id == 505) {
        session_open_users();
        return true;
    } else if (action.id == 504) {
        session_lock();
        return true;
    } else if (action.id == 502 || action.id == 503) {
        dialog_open(action.id == 502 ? 6 : 7, "");
    } else {
        if (action.id == 201 || action.id == 202) {
            night = action.id == 202;
            palette();
            make_wallpaper();
        } else if (action.id == 205 || action.id == 206) {
            wall = action.id == 206;
            make_wallpaper();
        } else if (action.id == 302)
            cursor_dirty = true;
        else if (action.id == 303)
            keyboard_set(keyboard);
        save_settings();
    }
    dirty = true;
    return true;
}
static bool settings_key(int k) {
    if (focused_app() != 3 || dialog_kind)
        return false;
    if (k == 6) {
        settings_search_active = true;
        dirty = true;
        return true;
    }
    if (settings_search_active) {
        if (k == KEY_ESCAPE) {
            settings_search_active = false;
            settings_search[0] = 0;
            settings_search_len = 0;
        } else if (k == KEY_BACKSPACE) {
            settings_search_len = utf8_prev(settings_search, settings_search_len);
            settings_search[settings_search_len] = 0;
        } else if (k == KEY_ENTER) {
            for (int i = 0; i < 10; i++)
                if (settings_match(settings_categories[i])) {
                    settings_navigate(i, 0, 0);
                    break;
                }
            settings_search_active = false;
        } else if (k >= 32 && k < 127 && settings_search_len + 1 < sizeof settings_search) {
            settings_search[settings_search_len++] = (char)k;
            settings_search[settings_search_len] = 0;
        }
        dirty = true;
        return true;
    }
    if (k == KEY_ESCAPE && settings_subpage) {
        settings_navigate(settings_category,
                          (settings_subpage == 46 || settings_subpage == 47) ? 43 : 0, -1);
        dirty = true;
        return true;
    }
    if (k == KEY_DOWN || k == KEY_PAGE_DOWN || k == KEY_UP || k == KEY_PAGE_UP) {
        settings_scroll += k == KEY_DOWN ? 40 : k == KEY_PAGE_DOWN ? 200 : k == KEY_UP ? -40 : -200;
        settings_build(app_window(3));
        dirty = true;
        return true;
    }
    return false;
}
static void dialog_accept(void) {
    char path[128];
    bool okay = false;
    int kind = dialog_kind;
    if (kind == 6 || kind == 7) {
        if (!settings_flush(ark_millis(), true) || !vfs_sync()) {
            notice(vfs_error());
            dialog_kind = 0;
            return;
        }
        if (kind == 6)
            platform_reboot();
        else
            platform_poweroff();
        dialog_kind = 0;
        return;
    }
    if (kind >= 1 && kind <= 4) {
        if (!dialog_len) {
            notice("名称不能为空");
            return;
        }
        for (size_t i = 0; i < dialog_len; i++)
            if (dialog[i] == '/') {
                notice("名称不能包含斜杠");
                return;
            }
        path_join(path, folder, dialog);
    }
    if (kind == 1) {
        int idx = vfs_create(path);
        okay = idx >= 0;
        if (okay) {
            selected = idx;
            load_note(idx);
        }
    } else if (kind == 2)
        okay = vfs_mkdir(path);
    else if (kind == 3 && selected >= 0)
        okay = vfs_rename((*vfs_entry(selected)).name, path);
    else if (kind == 4 && selected >= 0)
        okay = vfs_copy((*vfs_entry(selected)).name, path);
    else if (kind == 5 && selected >= 0) {
        int index = selected;
        okay = vfs_remove((*vfs_entry(index)).name);
        if (okay) {
            selected = -1;
        }
    }
    dialog_kind = 0;
    refresh_files();
    if (okay) {
        if (storage_mounted())
            save_now();
        else
            notice("已修改 · 当前为临时内存会话");
    } else
        notice(vfs_error()[0] ? vfs_error() : "操作失败，请检查名称、容量或目录是否为空");
    dirty = true;
}
static void insert(char *s, size_t *len, size_t *cursor, size_t cap, const char *bytes) {
    size_t n = strlen(bytes);
    if (*len + n >= cap) {
        notice("内容已达到容量上限");
        return;
    }
    memmove(s + *cursor + n, s + *cursor, *len - *cursor + 1);
    memcpy(s + *cursor, bytes, n);
    *len += n;
    *cursor += n;
}
static void backspace(char *s, size_t *len, size_t *cursor) {
    if (!*cursor)
        return;
    size_t prev = utf8_prev(s, *cursor);
    memmove(s + prev, s + *cursor, *len - *cursor + 1);
    *len -= *cursor - prev;
    *cursor = prev;
}
static void delete_forward(char *s, size_t *len, size_t *cursor) {
    if (*cursor >= *len)
        return;
    const char *p = s + *cursor;
    utf8_decode(&p);
    size_t n = (size_t)(p - s) - *cursor;
    memmove(s + *cursor, s + *cursor + n, *len - *cursor - n + 1);
    *len -= n;
}
static void insert_input(const char *s) {
    if (session_display_editing()) {
        session_display_insert(s);
        return;
    }
    if (dialog_kind && dialog_kind <= 4) {
        if (dialog_select_all) {
            dialog[0] = 0;
            dialog_len = 0;
            dialog_select_all = false;
        }
        size_t pos = dialog_len;
        insert(dialog, &dialog_len, &pos, sizeof dialog, s);
        dirty = true;
        return;
    }
    int a = focus();
    window_context(a);
    if (window_app(a) == 3 && settings_search_active) {
        size_t pos = settings_search_len;
        insert(settings_search, &settings_search_len, &pos, sizeof settings_search, s);
        dirty = true;
        return;
    }
    if (window_app(a) == 10 && task_search_active) {
        tasks_insert(s);
        return;
    }
    if (thirdparty_app(a)) {
        text_dispatch(s);
        return;
    }
    if (window_app(a) == 0) {
        insert(command, &command_len, &command_cursor, sizeof command, s);
        history_age = -1;
    }
    dirty = true;
}
static void maximize(int a) {
    Window *w = &windows[a];
    if (!w->maxed) {
        w->ox = w->x;
        w->oy = w->y;
        w->ow = w->w;
        w->oh = w->h;
        w->x = 14;
        w->y = 51;
        w->w = sw - 28;
        w->h = max(320, desktop_bottom() - 57);
    } else {
        w->x = w->ox;
        w->y = w->oy;
        w->w = w->ow;
        w->h = w->oh;
    }
    w->maxed = !w->maxed;
    thirdparty_resize(a);
    dirty = true;
}
#include "shell_extra.h"
static void key_event(int k) {
    if (session_key(k))
        return;
    if (permission_key(k))
        return;
    if (ime_key(k))
        return;
    if (topbar_key(k) || settings_key(k) || tasks_key(k) || native_key(k))
        return;
    if (k == 14 && !dialog_kind) {
        new_item();
        return;
    }
    if (k == 20) {
        open_app(10);
        return;
    }
    if (k == 11) {
        session_lock();
        return;
    }
    if (k == KEY_LAUNCHER && !dialog_kind) {
        launcher_toggle();
        return;
    }
    if (launcher_keys(k))
        return;
    if (dialog_kind) {
        if (k == KEY_ESCAPE) {
            dialog_kind = 0;
            dirty = true;
        } else if (k == KEY_ENTER)
            dialog_accept();
        else if (k == 1 && dialog_kind <= 4) {
            dialog_select_all = true;
            dirty = true;
        } else if (k == KEY_BACKSPACE || k == KEY_DELETE) {
            if (dialog_select_all) {
                dialog[0] = 0;
                dialog_len = 0;
                dialog_select_all = false;
            } else if (k == KEY_BACKSPACE) {
                size_t pos = dialog_len;
                backspace(dialog, &dialog_len, &pos);
            }
            dirty = true;
        } else if (k >= 32 && k < 127) {
            char b[2] = {(char)k, 0};
            insert_input(b);
        }
        return;
    }
    if (k >= KEY_F1 && k <= KEY_F4) {
        open_app(k - KEY_F1);
        return;
    }
    int wid = focus(), a = window_app(wid);
    window_context(wid);
    if (k == KEY_TAB && !thirdparty_app(focus())) {
        for (int i = 0; i < WINS; i++)
            if (windows[order[i]].visible) {
                activate(order[i]);
                break;
            }
        return;
    }
    if (k == KEY_ESCAPE) {
        if (a == 3)
            settings_cancel_pointer();
        if (keyboard)
            keyboard_set(false);
        dirty = true;
        return;
    }
    if (thirdparty_app(wid)) {
        thirdparty_key(wid, k);
        return;
    }
    if (k == 19) {
        save_now();
        return;
    }
    if (a == 0) {
        if (k == KEY_ENTER) {
            shell_execute(command);
            package_ui_refresh();
            command_len = command_cursor = 0;
            command[0] = 0;
            history_age = -1;
            term_scroll = 0;
            if (shell_action) {
                int n = shell_action;
                shell_action = 0;
                if (n >= 1 && n < APPS) {
                    if (n == 1)
                        files_open_path(shell_cwd());
                    else
                        open_app(n);
                }
            }
            refresh_files();
        } else if (k == KEY_BACKSPACE)
            backspace(command, &command_len, &command_cursor);
        else if (k == KEY_DELETE)
            delete_forward(command, &command_len, &command_cursor);
        else if (k == KEY_LEFT)
            command_cursor = utf8_prev(command, command_cursor);
        else if (k == KEY_RIGHT && command_cursor < command_len) {
            const char *p = command + command_cursor;
            utf8_decode(&p);
            command_cursor = (size_t)(p - command);
        } else if (k == KEY_UP || k == KEY_DOWN) {
            int n = history_age + (k == KEY_UP ? 1 : -1);
            if (n < 0) {
                history_age = -1;
                command[0] = 0;
                command_len = command_cursor = 0;
            } else {
                const char *p = shell_history_get(n);
                if (p) {
                    history_age = n;
                    strcopy(command, p, sizeof command);
                    command_len = command_cursor = strlen(command);
                }
            }
        } else if (k == KEY_PAGE_UP)
            term_scroll += 5;
        else if (k == KEY_PAGE_DOWN)
            term_scroll = max(0, term_scroll - 5);
        else if (k == 12) {
            shell_execute("clear");
            term_scroll = 0;
        } else if (k == 3) {
            command[0] = 0;
            command_len = command_cursor = 0;
            shell_print("^C");
        } else if (k == KEY_HOME)
            command_cursor = 0;
        else if (k == KEY_END)
            command_cursor = command_len;
        else if (k >= 32 && k < 127) {
            char b[2] = {(char)k, 0};
            insert_input(b);
        }
    } else if (a == 1) {
        refresh_files();
        int row = 0;
        for (int i = 0; i < file_count; i++)
            if (file_list[i] == selected)
                row = i;
        if (k == KEY_UP && file_count) {
            row = max(0, row - 1);
            selected = file_list[row];
        }
        if (k == KEY_DOWN && file_count) {
            row = min(file_count - 1, row + 1);
            selected = file_list[row];
        }
        if (k == KEY_UP || k == KEY_DOWN) {
            int rows = max(1, (app_window(1)->h - 176) / 40);
            if (row < file_scroll)
                file_scroll = row;
            else if (row >= file_scroll + rows)
                file_scroll = row - rows + 1;
        }
        if (k == KEY_ENTER)
            load_note(selected);
        if (k == KEY_BACKSPACE) {
            parent(folder);
            selected = -1;
            file_scroll = 0;
        }
        if (k == KEY_DELETE && selected >= 0)
            dialog_open(5, "");
    }
    dirty = true;
}
static void keyboard_click(int x, int y) {
    int bx = max(10, (sw - 820) / 2), by = sh - 270, w = min(sw - 20, 820);
    if (in(x, y, bx + w - 240, by + 8, 68, 36)) {
        keyboard_upper = !keyboard_upper;
        return;
    }
    if (in(x, y, bx + w - 164, by + 8, 68, 36)) {
        keyboard_symbols = !keyboard_symbols;
        return;
    }
    if (in(x, y, bx + w - 86, by + 8, 64, 36)) {
        keyboard_set(false);
        return;
    }
    if (in(x, y, bx + 18, by + 211, 98, 36)) {
        key_event(9);
        return;
    }
    if (in(x, y, bx + 126, by + 211, w - 364, 36)) {
        key_event(' ');
        return;
    }
    if (in(x, y, bx + w - 228, by + 211, 94, 36)) {
        key_event(KEY_BACKSPACE);
        return;
    }
    if (in(x, y, bx + w - 124, by + 211, 104, 36)) {
        key_event(KEY_ENTER);
        return;
    }
    int kw = (w - 55) / 10;
    for (int row = 0; row < 4; row++) {
        int count = (int)strlen(keyboard_row(row)), xx = bx + (w - count * (kw + 4)) / 2;
        for (int i = 0; i < count; i++)
            if (in(x, y, xx + i * (kw + 4), by + 48 + row * 39, kw, 36))
                key_event(keyboard_char(row, i));
    }
}
static void click_files(Window *w, int x, int y) {
    int bx = w->x + 198, by = w->y + 68;
    char documents[128];
    path_join(documents, user_home, "Documents");
    if (x < w->x + 180) {
        const char *paths[] = {user_home, documents, "/", "/mnt/fat32", "/mnt/ntfs"};
        for (int i = 0; i < 5; i++)
            if (in(x, y, w->x + 11, w->y + 108 + i * 44, 158, 36)) {
                strcopy(folder, paths[i], sizeof folder);
                selected = -1;
                file_scroll = 0;
                refresh_files();
            }
        return;
    }
    int right = w->x + w->w - 178;
    if (in(x, y, bx, by, 36, 36)) {
        parent(folder);
        selected = -1;
        file_scroll = 0;
    } else if (in(x, y, bx + 40, by, 36, 36))
        dialog_open(1, "新建文件.txt");
    else if (in(x, y, bx + 80, by, 36, 36))
        dialog_open(2, "新建文件夹");
    else if (in(x, y, right, by, 36, 36) && selected >= 0)
        dialog_open(3, base(vfs_entry(selected)->name));
    else if (in(x, y, right + 40, by, 36, 36) && selected >= 0) {
        char name[128] = "副本-";
        cat(name, sizeof name, base(vfs_entry(selected)->name));
        dialog_open(4, name);
    } else if (in(x, y, right + 80, by, 36, 36) && selected >= 0)
        dialog_open(5, "");
    else if (in(x, y, right + 120, by, 36, 36))
        load_note(selected);
    else {
        int rows = max(1, (w->h - 176) / 40);
        for (int i = file_scroll; i < min(file_count, file_scroll + rows); i++)
            if (in(x, y, bx - 5, by + 61 + (i - file_scroll) * 40, w->w - 204, 36)) {
                int index = file_list[i];
                bool double_click =
                    file_click_index == index && platform_ticks() - file_click_at < 35;
                selected = index;
                file_click_index = index;
                file_click_at = platform_ticks();
                if (double_click) {
                    file_drag_candidate = false;
                    load_note(index);
                }
            }
    }
    dirty = true;
}
static void click_app(int wid, int x, int y) {
    window_context(wid);
    int a = window_app(wid);
    Window *w = &windows[wid];
    if (thirdparty_app(wid))
        return;
    if (a >= 11) {
        native_click(a, x, y);
        return;
    }
    if (a == 10) {
        tasks_click(x, y);
        return;
    }
    if (a == 1) {
        click_files(w, x, y);
        return;
    }
    if (a == 3)
        settings_pointer(x, y, true);
    else if (a == 4) {
        int bx = w->x + 29, by = w->y + 80;
        if (in(x, y, bx, by + 389, 132, 36))
            dialog_open(6, "");
        else if (in(x, y, bx + 148, by + 389, 108, 36))
            dialog_open(7, "");
    }
    dirty = true;
}
static int pointer_hover(int x, int y) {
    if (dock_app_at(x, y) >= 0)
        return 1;
    for (int i = WINS - 1; i >= 0; i--) {
        int a = order[i];
        Window *w = &windows[a];
        if (w->visible && in(x, y, w->x, w->y, w->w, w->h)) {
            for (int b = 0; b < 3; b++)
                if (in(x, y, w->x + w->w - 142 + b * 44, w->y + 9, 40, 36))
                    return 16 + a * 3 + b;
            break;
        }
    }
    return 0;
}
static int pointer_window_at(int x, int y) {
    for (int i = WINS - 1; i >= 0; i--) {
        int w = order[i];
        Window *v = &windows[w];
        if (v->visible && in(x, y, v->x - 6, v->y - 6, v->w + 12, v->h + 12))
            return w;
    }
    return -1;
}
static void click_begin(void) {
    click_pending = true;
    click_x = mx;
    click_y = my;
    click_window = pointer_window_at(mx, my);
    click_has_region = false;
    bool overlay = dialog_kind || session_overlay_visible() || permission_showing;
    for (unsigned i = click_region_count; i > 0; i--) {
        ClickRegion r = click_regions[i - 1];
        if ((r.owner < 0 || (!overlay && r.owner == click_window)) &&
            in(mx, my, r.x, r.y, r.w, r.h)) {
            click_capture = r;
            click_has_region = true;
            break;
        }
    }
    glass_press_begin();
}
static bool click_release(void) {
    if (!click_pending)
        return false;
    click_pending = false;
    if (click_has_region) {
        if (!in(mx, my, click_capture.x, click_capture.y, click_capture.w, click_capture.h))
            return false;
        if (click_capture.owner >= 0 && pointer_window_at(mx, my) != click_capture.owner)
            return false;
        return true;
    }
    int dx = mx - click_x, dy = my - click_y;
    return dx * dx + dy * dy < 49 && pointer_window_at(mx, my) == click_window;
}
static void pointer_event(Event *e) {
    if (e->type == EV_SCROLL) {
        if (!session_active() || session_overlay_visible() || permission_showing)
            return;
        int a = -1;
        for (int i = WINS - 1; i >= 0; i--) {
            int w = order[i];
            if (windows[w].visible &&
                in(mx, my, windows[w].x, windows[w].y, windows[w].w, windows[w].h)) {
                a = w;
                break;
            }
        }
        if (a < 0)
            return;
        int app = window_app(a), delta = e->dy * 3;
        window_context(a);
        if (app == 3 && session_users_scroll(a, delta)) {
            dirty = true;
            return;
        }
        if (app == 0)
            term_scroll = max(0, term_scroll - delta);
        else if (app == 1)
            file_scroll = clamp(file_scroll + delta, 0, max(0, file_count - 1));
        else if (app == 3) {
            settings_scroll += delta * 24;
            settings_build(app_window(3));
        } else if (app == 10)
            tasks_scroll_by(delta);
        else if (app == 16)
            package_ui_scroll(delta);
        else if (thirdparty_app(a)) {
            ArkEvent scroll = {.type = ARK_EV_SCROLL, .x = e->dx, .y = delta};
            (void)thirdparty_request_event(a, scroll);
        }
        dirty = true;
        return;
    }

    bool touching = e->type == EV_TOUCH, absolute = touching || e->type == EV_POINTER;
    pointer_touch_mode = touching;
    int prevx = mx, prevy = my, hover = pointer_hover(mx, my);
    bool prevtouch = touch_down;
    if (absolute) {
        mx = clamp(e->dx * (sw - 1) / 32767, 0, sw - 1);
        my = clamp(e->dy * (sh - 1) / 32767, 0, sh - 1);
    } else {
        static int remx, remy;
        remx += e->dx * pointer_speed;
        remy += e->dy * pointer_speed;
        mx = clamp(mx + remx / 100, 0, sw - 1);
        my = clamp(my + remy / 100, 0, sh - 1);
        remx %= 100;
        remy %= 100;
    }
    touch_down = touching && (e->buttons & 1);
    bool moved = prevx != mx || prevy != my, down = (e->buttons & 1) != 0,
         pressed = down && !pointer_was_down, released = !down && pointer_was_down;
    if (pressed)
        click_begin();
    pointer_clicked = released && click_release();
    if (moved || pressed || released || prevtouch != touch_down)
        cursor_dirty = true;
    if (session_pointer(pointer_clicked)) {
        pointer_was_down = down;
        return;
    }
    if (permission_pointer(pointer_clicked)) {
        pointer_was_down = down;
        return;
    }
    static bool right_was_down;
    static int right_x, right_y;
    bool right = (e->buttons & 2) != 0;
    if (right && !right_was_down) {
        right_x = mx;
        right_y = my;
    }
    bool right_click = !right && right_was_down && mx - right_x > -7 && mx - right_x < 7 &&
                       my - right_y > -7 && my - right_y < 7;
    right_was_down = right;
    if (transfer_pointer(pressed, down, moved) || ime_pointer(pointer_clicked) ||
        dock_pointer(pressed, down, moved) || native_island_pointer(pointer_clicked) ||
        topbar_pointer(pointer_clicked) ||
        native_pointer(pressed, down, moved, right_click, pointer_clicked)) {
        pointer_was_down = down;
        return;
    }
    if (!pressed && thirdparty_app(focus()) && drag < 0 && resize_app < 0)
        thirdparty_pointer(focus(), mx, my, down);
    int next_hover = pointer_hover(mx, my);
    if (next_hover != hover && (next_hover >= 16 || hover >= 16))
        dirty = true;
    if (!down) {
        drag = -1;
        resize_app = -1;
    }
    if (motion_pointer(pointer_clicked, down, touching)) {
        pointer_was_down = down;
        return;
    }
    if (focused_app() == 3 && !dialog_kind && drag < 0 && resize_app < 0 &&
        (!pressed || pointer_window_at(mx, my) == focus()))
        settings_pointer(mx, my, down);
    if (resize_app >= 0 && down && moved) {
        Window *w = &windows[resize_app];
        int mw = window_app(resize_app) == 10                                 ? 720
                 : window_app(resize_app) == 1 || window_app(resize_app) == 3 ? 640
                                                                              : 360,
            mh = window_app(resize_app) == 3 || window_app(resize_app) == 10 ? 420 : 220;
        if (thirdparty_app(resize_app)) {
            int scale = sw >= 2560 || sh >= 1440 ? 2 : 1;
            mw = max(mw, 480 * scale + 2);
            mh = max(mh, 440 * scale + 55);
        }
        int dx = mx - resize_start_x, dy = my - resize_start_y;
        w->w = clamp(resize_start_w + (resize_edges & 1   ? -dx
                                       : resize_edges & 2 ? dx
                                                          : 0),
                     mw, max(mw, sw));
        w->h = clamp(resize_start_h + (resize_edges & 4   ? -dy
                                       : resize_edges & 8 ? dy
                                                          : 0),
                     mh, max(mh, sh));
        w->x = resize_origin_x + (resize_edges & 1 ? resize_start_w - w->w : 0);
        w->y = resize_origin_y + (resize_edges & 4 ? resize_start_h - w->h : 0);
        thirdparty_resize(resize_app);
        dirty = true;
    }
    if (drag >= 0 && down && moved) {
        Window *w = &windows[drag];
        w->x = mx - dragx;
        w->y = my - dragy;
        dirty = true;
    }
    if (pressed && !dialog_kind && !keyboard && caption_menu < 0) {
        int a = pointer_window_at(mx, my);
        if (a >= 0) {
            activate(a);
            Window *w = &windows[a];
            window_context(a);
            int edges = (mx < w->x + 6           ? 1
                         : mx >= w->x + w->w - 6 ? 2
                                                 : 0) |
                        (my < w->y + 6           ? 4
                         : my >= w->y + w->h - 6 ? 8
                                                 : 0);
            if (!w->maxed && (edges || in(mx, my, w->x + w->w - 22, w->y + w->h - 22, 22, 22))) {
                resize_app = a;
                resize_edges = edges ? edges : 10;
                resize_start_x = mx;
                resize_start_y = my;
                resize_start_w = w->w;
                resize_start_h = w->h;
                resize_origin_x = w->x;
                resize_origin_y = w->y;
                click_pending = false;
            } else if (my < w->y + 54 && !in(mx, my, w->x + w->w - 142, w->y + 9, 128, 36) &&
                       !in(mx, my, w->x + 10, w->y + 9, 60, 36) && !w->maxed) {
                drag_begin(a);
                drag = a;
                dragx = mx - w->x;
                dragy = my - w->y;
                click_pending = false;
            } else if (my >= w->y + 54) {
                if (thirdparty_app(a))
                    thirdparty_pointer(a, mx, my, true);
                else if (window_app(a) == 1) {
                    int row = (my - w->y - 129) / 40 + file_scroll;
                    if (mx >= w->x + 193 && my >= w->y + 129 && my < w->y + w->h - 47 && row >= 0 &&
                        row < file_count) {
                        selected = file_list[row];
                        strcopy(file_drag_path, vfs_entry(selected)->name, 128);
                        file_drag_x = mx;
                        file_drag_y = my;
                        file_drag_candidate = true;
                        dirty = true;
                    }
                }
            }
        }
    }
    if (pointer_clicked) {
        if (caption_menu >= 0) {
            int a = caption_menu;
            Window *w = &windows[a];
            int px = w->x + 10, py = w->y + 51;
            caption_menu = -1;
            if (in(mx, my, px, py, 210, 144)) {
                int action = (my - py) / 36;
                if (action == 0)
                    maximize(a);
                else if (action == 1) {
                    w->maxed = false;
                    w->x = 14;
                    w->y = 49;
                    w->w = (sw - 42) / 2;
                    w->h = sh - 164;
                } else if (action == 2) {
                    w->maxed = false;
                    w->x = sw / 2 + 7;
                    w->y = 49;
                    w->w = (sw - 42) / 2;
                    w->h = sh - 164;
                } else
                    close_window(a);
                thirdparty_resize(a);
            }
            dirty = true;
            pointer_was_down = down;
            return;
        }
        if (keyboard && my >= sh - 270) {
            dirty = true;
            keyboard_click(mx, my);
        } else if (dialog_kind) {
            dirty = true;
            int x = (sw - 480) / 2, y = (sh - 232) / 2;
            if (in(mx, my, x + 234, y + 167, 100, 36))
                dialog_kind = 0;
            else if (in(mx, my, x + 345, y + 167, 110, 36))
                dialog_accept();
            else if (dialog_kind <= 4 && touching && in(mx, my, x + 24, y + 76, 432, 48))
                keyboard_set(true);
        } else if (my < 39) {
            topbar_pointer(true);
        }

        else if (!keyboard && my >= sh - 86) {
            if (launcher_button_at(mx, my))
                launcher_toggle();
            else {
                int a = dock_app_at(mx, my);
                if (a >= 0)
                    open_app(a);
            }
        } else {
            bool hit = false;
            for (int i = WINS - 1; i >= 0; i--) {
                int a = order[i];
                Window *w = &windows[a];
                if (!w->visible || !in(mx, my, w->x - 6, w->y - 6, w->w + 12, w->h + 12))
                    continue;
                activate(a);
                hit = true;
                int edges = (mx < w->x + 6           ? 1
                             : mx >= w->x + w->w - 6 ? 2
                                                     : 0) |
                            (my < w->y + 6           ? 4
                             : my >= w->y + w->h - 6 ? 8
                                                     : 0);
                if (!w->maxed &&
                    (edges || in(mx, my, w->x + w->w - 22, w->y + w->h - 22, 22, 22))) {
                    resize_app = a;
                    resize_edges = edges ? edges : 10;
                    resize_start_x = mx;
                    resize_start_y = my;
                    resize_start_w = w->w;
                    resize_start_h = w->h;
                    resize_origin_x = w->x;
                    resize_origin_y = w->y;
                } else if (my < w->y + 54) {
                    int left = w->x + w->w - 142;
                    if (in(mx, my, left, w->y + 9, 40, 36)) {
                        minimize_window(a);
                    } else if (in(mx, my, left + 44, w->y + 9, 40, 36))
                        maximize(a);
                    else if (in(mx, my, left + 88, w->y + 9, 40, 36)) {
                        close_window(a);
                    } else if (in(mx, my, w->x + 10, w->y + 9, 60, 36)) {
                        caption_menu = a;
                        dirty = true;
                    }

                } else if (!thirdparty_app(a) && window_app(a) != 3)
                    click_app(a, mx, my);
                break;
            }
            (void)hit;
        }
    }
    pointer_was_down = down;
}

#include "desktop_motion.inc"

#include "desktop_packages.inc"
#include "desktop_apps.inc"
#include "desktop_ime.inc"
#include "desktop_tasks.inc"
#include "desktop_session.inc"
#include "desktop_wallpaper.inc"
#include "desktop_native.inc"
#include "desktop_transfer.inc"
#include "desktop_reminders.inc"
#include "desktop_topbar.inc"
#include "desktop_permissions.inc"
#include "desktop_cursors.inc"
static bool desktop_resize(unsigned count) {
    if (count <= (unsigned)APPS)
        return true;
    unsigned old = (unsigned)APPS, total = count + EXTRA_WINDOWS,
             old_total = old ? old + EXTRA_WINDOWS : 0;
#define GROW(name, type)                                                                           \
    do {                                                                                           \
        type *next = ark_realloc(name, total * sizeof(type));                                      \
        if (!next)                                                                                 \
            return false;                                                                          \
        name = next;                                                                               \
        if (old)                                                                                   \
            memmove(name + count, name + old, EXTRA_WINDOWS * sizeof(type));                       \
        else                                                                                       \
            memset(name, 0, total * sizeof(type));                                                 \
        memset(name + old, 0, (count - old) * sizeof(type));                                       \
    } while (0)
    GROW(windows, Window);
    GROW(minimized, bool);
    GROW(titles, const char *);
    GROW(eng_titles, const char *);
    GROW(window_apps, int);
    GROW(native_contexts, NativeContext);
    GROW(keyboard_fit, bool);
    GROW(keyboard_old_y, int);
    GROW(keyboard_old_h, int);
    GROW(cached_slot, int);
    GROW(cached_x, int);
    GROW(cached_y, int);
    GROW(cached_w, int);
    GROW(cached_h, int);
    GROW(cached_valid, bool);
    GROW(thumbnails, uint32_t *);
    GROW(thumbnail_valid, bool);
    GROW(hidden_windows, bool);
    GROW(thirdparty, ThirdpartyState);
#undef GROW
    ArkPackageInfo *packages = ark_realloc(package_entries, count * sizeof *packages);
    if (!packages)
        return false;
    package_entries = packages;
    memset(packages + old, 0, (count - old) * sizeof *packages);
    int *launch = ark_realloc(launcher_apps, count * sizeof *launch);
    if (!launch)
        return false;
    launcher_apps = launch;
    int *next_order = ark_alloc(total * sizeof *next_order);
    if (!next_order)
        return false;
    unsigned gap = count - old;
    for (unsigned i = 0; i < gap; i++)
        next_order[i] = (int)(old + i);
    for (unsigned i = 0; i < old_total; i++)
        next_order[gap + i] = order[i] >= (int)old ? order[i] + (int)gap : order[i];
    if (!old)
        for (unsigned i = count; i < total; i++)
            next_order[i] = (int)i;
    ark_free(order);
    order = next_order;
    for (unsigned i = old; i < count; i++) {
        window_apps[i] = (int)i;
        windows[i] = (Window){130 + (int)(i % 9) * 15,
                              90 + (int)(i % 9) * 12,
                              min(802, sw - 28),
                              min(555, sh - 142),
                              0,
                              0,
                              0,
                              0,
                              false,
                              false};
        titles[i] = i < ARK_PACKAGE_DESKTOP_FIRST ? builtin_titles[i] : "应用";
        eng_titles[i] = i < ARK_PACKAGE_DESKTOP_FIRST ? builtin_eng_titles[i] : "Package";
        thirdparty[i].surface = UINT32_MAX;
    }
    if (!old)
        for (unsigned i = count; i < total; i++) {
            window_apps[i] = -1;
            thirdparty[i].surface = UINT32_MAX;
        }
#define REBASE(index)                                                                              \
    do {                                                                                           \
        if (old && (index) >= (int)old)                                                            \
            (index) += (int)gap;                                                                   \
    } while (0)
    REBASE(context_window);
    REBASE(native_window);
    REBASE(drag);
    REBASE(resize_app);
    REBASE(cached_app);
    REBASE(scene_app);
    REBASE(caption_menu);
    REBASE(context_files_window);
    REBASE(session_users_window);
    REBASE(settings_page.window);
    REBASE(click_window);
    REBASE(click_owner);
    REBASE(click_capture.owner);
    REBASE(glass_press.region.owner);
    for (unsigned i = 0; i < click_region_count; i++)
        REBASE(click_regions[i].owner);
    for (unsigned i = 0; i < WINDOW_CACHE_SLOTS; i++)
        REBASE(cache_owner[i]);
#undef REBASE
    app_count = (int)count;
    for (unsigned i = count; i < total; i++)
        if (window_apps[i] >= 0) {
            titles[i] = titles[window_apps[i]];
            eng_titles[i] = eng_titles[window_apps[i]];
        }
    return true;
}

static void desktop_session_changed(void) {
    if (!session_active()) {
        native_reset();
        ime_reset();
        transfer_reset();
        reminders_reset();
    }
    memset(dock_plate, 0, sizeof dock_plate);
    memset(dock_lift, 0, sizeof dock_lift);
    dock_plate_valid = false;
    context_window = native_window = -1;
    shell_switch(0);
    for (int w = 0; w < WINS; w++) {
        ark_free(native_contexts[w].shell);
        memset(&native_contexts[w], 0, sizeof native_contexts[w]);
        if (w >= APPS)
            window_apps[w] = -1;
    }
    motion_cancel();
    settings_page_reset();
    memset(motion_base, 0, screen_bytes);
    memset(motion_layer, 0, screen_bytes);
    memset(cached_backdrop, 0, screen_bytes);
    for (int i = 0; i < WINDOW_CACHE_SLOTS; i++)
        memset(cached_window[i], 0, screen_bytes);
    for (int i = 0; i < WINS; i++) {
        ark_free(thumbnails[i]);
        thumbnails[i] = 0;
    }
    memset(launcher_layer, 0, screen_bytes);
    cached_app = -1;
    thirdparty_reset();
    permission_showing = false;
    caption_menu = -1;
    keyboard = false;
    dialog_kind = 0;
    settings_initialized = false;
    settings_cancel_pointer();
    settings_account[0] = settings_search[0] = 0;
    settings_search_len = 0;
    settings_search_active = false;
    settings_scroll = 0;
    drag = resize_app = -1;
    memset(command, 0, sizeof command);
    memset(dialog, 0, sizeof dialog);
    command_len = command_cursor = dialog_len = 0;
    selected = -1;
    message_ticks = 0;
    for (int i = 0; i < WINS; i++) {
        windows[i].visible = false;
        minimized[i] = false;
        cached_valid[i] = thumbnail_valid[i] = false;
    }
    vfs_init();
    shell_init(&boot);
    if (session_active()) {
        ArkSystemInfo info = {0};
        ark_info(&info);
        strcopy(user_home, info.home, sizeof user_home);
        strcopy(folder, user_home, sizeof folder);
        shell_set_home(user_home);
        shell_init(&boot);
        frame_rate = 120;
        read_settings();
        native_reset();
        dock_load();
        ime_reset();
        transfer_reset();
        reminders_reset();
        palette();
        make_wallpaper();
        char docs[128];
        path_join(docs, user_home, "Documents");
        vfs_mkdir(docs);
        file_scroll = 0;
        refresh_files();
        windows[1].visible = true;
        activate(1);

        serial_write("[session] desktop unlocked\n");
    }
    memset(canvas, 0, (size_t)sw * sh * 4);
    old_cursor_valid = false;
    dirty = true;
}
int main(void) {
    platform_init(0, 0, &boot);
    gpu_init(&boot);
    sw = (int)boot.width;
    sh = (int)boot.height;
    if (sw < 1024 || sh < 720 || sw > MAX_W || sh > MAX_H) {
        serial_write("[fatal] display must be 1024x720 through 3840x2160\n");
        for (;;)
            platform_idle();
    }
    virtio_input_init();
    screen_bytes = (size_t)sw * sh * 4;
    canvas = ark_memory(screen_bytes);
    wallpaper = ark_memory(screen_bytes);
    motion_base = ark_memory(screen_bytes);
    motion_layer = ark_memory(screen_bytes);
    cached_backdrop = ark_memory(screen_bytes);
    launcher_layer = ark_memory(screen_bytes);
    for (int i = 0; i < WINDOW_CACHE_SLOTS; i++)
        cached_window[i] = ark_memory(screen_bytes);
    if (!canvas || !wallpaper || !motion_base || !motion_layer || !cached_backdrop ||
        !launcher_layer || !cached_window[WINDOW_CACHE_SLOTS - 1]) {
        serial_write("[fatal] display buffers require more RAM; use -m 1024M for 4K\n");
        for (;;)
            platform_idle();
    }
    palette();
    make_wallpaper();
    if (!desktop_resize(ARK_PACKAGE_DESKTOP_FIRST)) {
        serial_write("[fatal] desktop metadata memory exhausted\n");
        for (;;)
            platform_idle();
    }
    for (int i = 0; i < APPS; i++)
        windows[i] = (Window){130 + i * 15, 90 + i * 12, 802, 555, 0, 0, 0, 0, false, false};
    windows[0] = (Window){290, 172, 800, 500, 0, 0, 0, 0, false, false};
    windows[1] = (Window){122, 92, 920, 548, 0, 0, 0, 0, false, false};
    windows[2] = (Window){190, 105, 820, 530, 0, 0, 0, 0, false, false};
    windows[3] = (Window){(sw - 1000) / 2, 45, 1000, 650, 0, 0, 0, 0, false, false};
    windows[4] = (Window){(sw - 500) / 2, 83, 500, 560, 0, 0, 0, 0, false, false};
    windows[5] = (Window){(sw - 660) / 2, 110, 660, 540, 0, 0, 0, 0, false, false};
    windows[6] = (Window){200, 100, 880, 560, 0, 0, 0, 0, false, false};
    windows[16].h = 620;
    windows[10] = (Window){(sw - 1040) / 2, 58, 1040, 642, 0, 0, 0, 0, false, false};
    for (int i = 0; i < APPS; i++) {
        Window *w = &windows[i];
        w->w = min(w->w, sw - 28);
        w->h = min(w->h, sh - 142);
        w->x = clamp(w->x, 14, sw - w->w - 14);
        w->y = clamp(w->y, 45, sh - 104 - w->h);
    }
    mx = sw / 2;
    my = sh / 2;
    render_workers_init();
    session_init();
    serial_write("[arkos] desktop ready 0.13.0+mouse1 ring3\n");
    motion_frame_init(&frame_clock, platform_millis(), (unsigned)frame_rate);
    render();
    uint64_t last = platform_ticks(), last_sync = last;
    for (;;) {
        Event e;
        unsigned count = 0;
        while (count++ < 128 && virtio_input_next_event(&e))
            pointer_event(&e);
        count = 0;
        while (count++ < 128 && platform_next_event(&e)) {
            if (e.type == EV_KEY)
                key_event(e.key);
            else
                pointer_event(&e);
        }
        uint64_t now = platform_ticks();
        session_tick(now);
        if (session_active()) {
            (void)settings_flush(ark_millis(), false);
            topbar_tick(now);
            if (message_ticks > 0) {
                message_ticks -= min((int)(now - last), message_ticks);
                if (!message_ticks)
                    dirty = true;
            }
            if (now - last_sync >= 200) {
                if (!vfs_sync())
                    notice(vfs_error());
                last_sync = now;
            }
            if (!session_overlay_visible()) {
                thirdparty_tick(now);
                ime_tick();
                transfer_refresh();
                reminders_tick(now);
                permission_tick(now);
                tasks_tick(now);
                native_tick(now);
                settings_page_tick(ark_millis());
                glass_press_tick(ark_millis());
                motion_tick(now);
            }
        }
        uint64_t ms = platform_millis();
        if (frame_clock.hz != (unsigned)frame_rate)
            motion_frame_init(&frame_clock, ms, (unsigned)frame_rate);
        last = now;
        if (ms >= frame_clock.deadline) {
            motion_frame_advance(&frame_clock, ms);
            if (dirty)
                render();
            else {
                if (!permission_showing)
                    wallpaper_tick(now);
                if (session_active() && !session_overlay_visible()) {
                    present_caret();
                    dock_tick(ms);
                }
            }
        }
        if (cursor_dirty && !session_boot_visible())
            present_pointer();
        uint64_t after = platform_millis(),
                 wait = frame_clock.deadline > after ? frame_clock.deadline - after : 1;
        if (wait > 10)
            wait = 10;
        (void)ark_wait_ms(wait);
    }
}
