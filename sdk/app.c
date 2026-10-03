#include "app.h"
static ArkApp *open_apps[ARK_MAX_SURFACES];
static unsigned open_count;
static int min(int a, int b) {
    return a < b ? a : b;
}
static int max(int a, int b) {
    return a > b ? a : b;
}
static uint32_t blend(uint32_t a, uint32_t b, unsigned n) {
    unsigned k = 255 - n;
    return (((((a >> 16) & 255) * k + ((b >> 16) & 255) * n) / 255) << 16) |
           (((((a >> 8) & 255) * k + ((b >> 8) & 255) * n) / 255) << 8) |
           (((a & 255) * k + (b & 255) * n) / 255);
}
void app_log(const char *t) {
    size_t n = strlen(t);
    if (n > 4096)
        n = 4096;
    (void)ark_syscall6(ARK_SYS_LOG, (uintptr_t)t, n, 0, 0, 0, 0);
}
bool app_open(ArkApp *a, const char *title, uint32_t *p, int w, int h) {
    if (!a || !p || w < 1 || h < 1 || w > (int)ARK_SURFACE_MAX_W || h > (int)ARK_SURFACE_MAX_H)
        return false;
    ArkSystemInfo info = {0};
    int scale = ark_info(&info) >= 0 && (info.width >= 2560 || info.height >= 1440) ? 2 : 1;
    if (w * scale > (int)ARK_SURFACE_MAX_W || h * scale > (int)ARK_SURFACE_MAX_H)
        scale = 1;
    int width = w * scale, height = h * scale;
    if (scale > 1) {
        p = ark_memory((size_t)width * height * 4);
        if (!p)
            return false;
    }
    ArkSurfaceRequest r = {0};
    r.op = ARK_SURFACE_CREATE;
    r.width = width;
    r.height = height;
    strcopy(r.title, title, sizeof(r.title));
    if (ark_surface(&r) < 0) {
        if (scale > 1)
            (void)ark_memory_release(p, (size_t)width * height * 4);
        app_log(r.error);
        return false;
    }
    memset(a, 0, sizeof *a);
    a->pixels_owned = scale > 1;
    a->id = r.id;
    a->width = width;
    a->height = height;
    a->scale = scale;
    a->pixels = p;
    a->capacity = (size_t)width * height * 4;
    a->dirty = true;
    a->closed = false;
    ArkRegistryRequest theme = {.op = ARK_REG_GET};
    strcopy(theme.key, "/user/appearance/dark", 128);
    a->night = ark_registry(&theme) >= 0 && theme.type == ARK_REG_INTEGER && theme.integer != 0;
    for (unsigned i = 0; i < ARK_MAX_SURFACES; i++)
        if (!open_apps[i]) {
            open_apps[i] = a;
            open_count++;
            break;
        }
    return true;
}
bool app_poll_event(ArkApp *a, ArkEvent *e) {
    if (!a || a->closed)
        return false;
    ArkSurfaceRequest r = {0};
    r.op = ARK_SURFACE_NEXT_EVENT;
    r.id = a->id;
    int64_t n = ark_surface(&r);
    if (n < 0) {
        app_close(a);
        if (!open_count)
            ark_exit(0);
        return false;
    }
    if (!n)
        return false;
    *e = r.event;
    if (e->type == ARK_EV_THEME) {
        a->night = e->key != 0;
        a->dirty = true;
    }
    if (e->type == ARK_EV_RESIZE && e->x > 0 && e->y > 0 && e->x <= (int)ARK_SURFACE_MAX_W &&
        e->y <= (int)ARK_SURFACE_MAX_H) {
        size_t need = (size_t)e->x * e->y * 4;
        if (need > a->capacity) {
            ArkSystemInfo info = {0};
            (void)ark_info(&info);
            size_t reserve = (size_t)max(e->x, (int)info.width) * max(e->y, (int)info.height) * 4;
            uint32_t *pixels = ark_memory(reserve);
            if (!pixels) {
                app_log("[app] resize: insufficient private memory\n");
                return true;
            }
            if (a->pixels_owned)
                (void)ark_memory_release(a->pixels, a->capacity);
            a->pixels = pixels;
            a->pixels_owned = true;
            a->capacity = reserve;
        }
        r.op = ARK_SURFACE_RESIZE;
        r.width = (unsigned)e->x;
        r.height = (unsigned)e->y;
        if (ark_surface(&r) >= 0) {
            a->width = e->x;
            a->height = e->y;
            a->dirty = true;
            char size[24];
            app_log("[app] surface resized ");
            uint_to_str(a->width, size);
            app_log(size);
            app_log("x");
            uint_to_str(a->height, size);
            app_log(size);
            app_log("\n");
        }
    }
    if (e->type == ARK_EV_POINTER || e->type == ARK_EV_TOUCH || e->type == ARK_EV_MOUSE ||
        e->type == ARK_EV_DROP) {
        e->x /= a->scale;
        e->y /= a->scale;
    }
    a->click = false;
    if (e->type == ARK_EV_DRAG_END) {
        a->pointer_down = false;
        a->pointer_cancelled = true;
    }
    if (e->type == ARK_EV_POINTER || e->type == ARK_EV_TOUCH || e->type == ARK_EV_MOUSE) {
        bool down = (e->buttons & 1) != 0;
        if (down && !a->pointer_down) {
            a->press_x = e->x;
            a->press_y = e->y;
            a->pointer_cancelled = false;
        }
        a->click = !down && a->pointer_down && !a->pointer_cancelled;
        a->pointer_down = down;
    }
    return true;
}
bool app_event(ArkApp *a, ArkEvent *e) {
    if (!app_poll_event(a, e))
        return false;
    if (e->type == ARK_EV_CLOSE) {
        app_close(a);
        ark_exit(0);
    }
    return true;
}

void app_present(ArkApp *a) {
    if (!a->dirty)
        return;
    ArkSurfaceRequest r = {.op = ARK_SURFACE_CURSORS,
                           .id = a->id,
                           .capacity = a->cursor_count,
                           .pixels = (uintptr_t)a->cursors};
    (void)ark_surface(&r);
    r = (ArkSurfaceRequest){0};
    r.op = ARK_SURFACE_PRESENT;
    r.id = a->id;
    r.width = a->width;
    r.height = a->height;
    r.stride = a->width;
    r.flags = ARK_PRESENT_FULL;
    r.pixels = (uintptr_t)a->pixels;
    if (ark_surface(&r) < 0) {
        app_log(r.error);
        app_close(a);
        if (!open_count)
            ark_exit(1);
        return;
    }
    a->dirty = false;
}
void app_close(ArkApp *a) {
    if (!a || a->closed)
        return;
    a->closed = true;
    for (unsigned i = 0; i < ARK_MAX_SURFACES; i++)
        if (open_apps[i] == a) {
            open_apps[i] = 0;
            open_count--;
            break;
        }
    ArkSurfaceRequest r = {0};
    r.op = ARK_SURFACE_CLOSE;
    r.id = a->id;
    (void)ark_surface(&r);
    if (a->pixels_owned) {
        (void)ark_memory_release(a->pixels, a->capacity);
        a->pixels = 0;
        a->pixels_owned = false;
    }
}
void app_text_input(ArkApp *a, bool enabled, int x, int y, int h) {
    ArkSurfaceRequest r = {.op = ARK_SURFACE_INPUT,
                           .id = a->id,
                           .flags = enabled ? ARK_SURFACE_TEXT_INPUT : 0,
                           .damage = {x * a->scale, y * a->scale, 2 * a->scale, h * a->scale}};
    (void)ark_surface(&r);
}
void app_rect_raw(ArkApp *a, int x, int y, int w, int h, uint32_t c) {
    x *= a->scale;
    y *= a->scale;
    w *= a->scale;
    h *= a->scale;
    for (int yy = max(0, y); yy < min(a->height, y + h); yy++)
        for (int xx = max(0, x); xx < min(a->width, x + w); xx++)
            a->pixels[(size_t)yy * a->width + xx] = c & 0xffffff;
    a->dirty = true;
}
uint32_t app_color(ArkApp *a, uint32_t c) {
    if (!a->night)
        return c;
    switch (c) {
    case 0xffffff:
        return 0x243044;
    case 0xf4f7fb:
    case 0xf1f5fa:
    case 0xf0f5fb:
    case 0xf2f5f9:
    case 0xf4f6fb:
        return 0x172234;
    case 0xd5e5ff:
    case 0xe3eeff:
    case 0xe5eefc:
    case 0xe5edf6:
    case 0xedf2f8:
    case 0xd8e6ff:
        return 0x30405b;
    case 0xc3d2e8:
    case 0xc6d4e3:
    case 0xdce4ef:
        return 0x42536b;
    case 0x343164:
    case 0x19233a:
    case 0x253851:
    case 0x293f5d:
    case 0x304662:
    case 0x273c58:
    case 0x273b55:
    case 0x253b58:
    case 0x293e58:
    case 0x283d57:
    case 0x314660:
        return 0xe4ebf5;
    case 0x3b526e:
    case 0x455971:
    case 0x45617d:
    case 0x4b5c79:
    case 0x597692:
        return 0xc0cedf;
    case 0x7387a0:
    case 0x74869b:
    case 0x6c8098:
    case 0x75879e:
    case 0x7d8fa5:
    case 0x73849b:
    case 0x6b7e97:
    case 0x6c829b:
    case 0x73869e:
    case 0x6b8199:
    case 0x8196ac:
    case 0x697b94:
    case 0x79899f:
        return 0xa2b3c9;
    case 0x36684e:
        return 0x97d4a8;
    case 0x245fb8:
    case 0x245aaf:
    case 0x245a9d:
    case 0x2469ce:
    case 0x2b6ac2:
        return 0x7ab1ff;
    default:
        return c;
    }
}
void app_rect(ArkApp *a, int x, int y, int w, int h, uint32_t c) {
    app_rect_raw(a, x, y, w, h, app_color(a, c));
}
void app_clear(ArkApp *a, uint32_t c) {
    a->cursor_count = 0;
    app_rect(a, 0, 0, a->width / a->scale, a->height / a->scale, c);
}
void app_cursor(ArkApp *a, int x, int y, int w, int h, unsigned shape) {
    if (shape >= ARK_CURSOR_COUNT || a->cursor_count == 32)
        return;
    int x0 = max(0, x * a->scale), y0 = max(0, y * a->scale),
        x1 = min(a->width, (x + w) * a->scale), y1 = min(a->height, (y + h) * a->scale);
    if (x0 >= x1 || y0 >= y1)
        return;
    a->cursors[a->cursor_count++] =
        (ArkCursorRegion){.rect = {x0, y0, x1 - x0, y1 - y0}, .shape = shape};
}
void app_round(ArkApp *a, int x, int y, int w, int h, int r, uint32_t c) {
    c = app_color(a, c);
    x *= a->scale;
    y *= a->scale;
    w *= a->scale;
    h *= a->scale;
    r *= a->scale;
    for (int yy = max(0, y); yy < min(a->height, y + h); yy++)
        for (int xx = max(0, x); xx < min(a->width, x + w); xx++) {
            unsigned n = raster_round_coverage(xx - x, yy - y, w, h, r);
            if (n) {
                uint32_t *p = a->pixels + (size_t)yy * a->width + xx;
                *p = n == 255 ? c : blend(*p, c, n);
            }
        }
    a->dirty = true;
}
void app_text(ArkApp *a, int x, int y, const char *t, uint32_t c, int scale) {
    unicode_draw(a->pixels, a->width, a->width, a->height, x * a->scale, y * a->scale, t,
                 c == 0xffffff ? c : app_color(a, c), scale * a->scale);
    a->dirty = true;
}
bool app_hit(int x, int y, int bx, int by, int bw, int bh) {
    return x >= bx && y >= by && x < bx + bw && y < by + bh;
}
bool app_released(const ArkApp *a, const ArkEvent *e) {
    return a->click &&
           (e->type == ARK_EV_POINTER || e->type == ARK_EV_MOUSE || e->type == ARK_EV_TOUCH);
}
bool app_click_hit(const ArkApp *a, const ArkEvent *e, int x, int y, int w, int h) {
    return app_released(a, e) && app_hit(a->press_x, a->press_y, x, y, w, h) &&
           app_hit(e->x, e->y, x, y, w, h);
}
void app_line(ArkApp *a, int x, int y, int ex, int ey, uint32_t c, int thickness) {
    int dx = ex > x ? ex - x : x - ex, sx = x < ex ? 1 : -1, dy = -(ey > y ? ey - y : y - ey),
        sy = y < ey ? 1 : -1, e = dx + dy;
    for (;;) {
        app_rect_raw(a, x - thickness / 2, y - thickness / 2, thickness, thickness, c);
        if (x == ex && y == ey)
            break;
        int n = e * 2;
        if (n >= dy) {
            e += dy;
            x += sx;
        }
        if (n <= dx) {
            e += dx;
            y += sy;
        }
    }
}
void app_button(ArkApp *a, int x, int y, int w, int h, const char *t, bool selected) {
    app_cursor(a, x, y, w, h, ARK_CURSOR_POINTER);
    app_round(a, x, y, w, h, 10, selected ? 0x2867d8 : 0xe5edf6);
    app_text(a, x + (w - utf8_width(t)) / 2, y + (h - 20) / 2, t, selected ? 0xffffff : 0x304662,
             1);
}
int app_read_text(const char *p, char *b, unsigned cap, char error[128]) {
    ArkFileRequest r = {0};
    r.op = ARK_FILE_READ;
    strcopy(r.path, p, sizeof(r.path));
    r.buffer = (uintptr_t)b;
    r.capacity = cap ? cap - 1 : 0;
    if (!cap || ark_file(&r) < 0) {
        strcopy(error, r.error, 128);
        return -1;
    }
    if (r.count >= cap)
        return -1;
    b[r.count] = 0;
    error[0] = 0;
    return (int)r.count;
}
bool app_write_text(const char *p, const char *t, char error[128]) {
    ArkFileRequest r = {0};
    r.op = ARK_FILE_CREATE;
    strcopy(r.path, p, sizeof(r.path));
    if (ark_file(&r) < 0) {
        strcopy(error, r.error, 128);
        return false;
    }
    memset(&r, 0, sizeof(r));
    r.op = ARK_FILE_WRITE;
    strcopy(r.path, p, sizeof(r.path));
    r.buffer = (uintptr_t)t;
    r.capacity = (uint32_t)strlen(t);
    if (ark_file(&r) < 0) {
        strcopy(error, r.error, 128);
        return false;
    }
    memset(&r, 0, sizeof(r));
    r.op = ARK_FILE_SYNC;
    if (ark_file(&r) < 0) {
        strcopy(error, r.error, 128);
        return false;
    }
    error[0] = 0;
    return true;
}
