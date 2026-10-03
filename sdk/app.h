#ifndef ARK_SDK_APP_H
#define ARK_SDK_APP_H
#include "ark_api.h"
#include "ark.h"
#include "unicode.h"
#include "raster.h"
typedef struct {
    uint32_t id;
    int width, height, scale;
    uint32_t *pixels;
    size_t capacity;
    bool dirty, night, closed, pointer_down, click, pointer_cancelled, pixels_owned;
    int press_x, press_y;
    ArkCursorRegion cursors[32];
    unsigned cursor_count;
} ArkApp;
bool app_open(ArkApp *, const char *title, uint32_t *pixels, int width, int height);
bool app_event(ArkApp *, ArkEvent *);
/* Raw CLOSE delivery for editors that must preserve unsaved changes. */
bool app_poll_event(ArkApp *, ArkEvent *);
void app_present(ArkApp *);
void app_close(ArkApp *);
void app_text_input(ArkApp *, bool enabled, int x, int y, int height);
/* Cursor regions are rebuilt with each app_clear; later regions take priority. */
void app_cursor(ArkApp *, int x, int y, int width, int height, unsigned shape);
void app_clear(ArkApp *, uint32_t color);
void app_rect(ArkApp *, int x, int y, int w, int h, uint32_t color);
void app_rect_raw(ArkApp *, int x, int y, int w, int h, uint32_t color);
uint32_t app_color(ArkApp *, uint32_t light_color);
static inline int app_width(const ArkApp *a) {
    return a->width / a->scale;
}
static inline int app_height(const ArkApp *a) {
    return a->height / a->scale;
}
void app_round(ArkApp *, int x, int y, int w, int h, int radius, uint32_t color);
void app_text(ArkApp *, int x, int y, const char *, uint32_t color, int scale);
void app_line(ArkApp *, int x, int y, int ex, int ey, uint32_t color, int thickness);
void app_button(ArkApp *, int x, int y, int w, int h, const char *label, bool selected);
bool app_hit(int x, int y, int bx, int by, int bw, int bh);
/* Activation occurs on release, inside the same control as the press. */
bool app_released(const ArkApp *, const ArkEvent *);
bool app_click_hit(const ArkApp *, const ArkEvent *, int x, int y, int width, int height);
int app_read_text(const char *path, char *buffer, unsigned capacity, char error[128]);
bool app_write_text(const char *path, const char *text, char error[128]);
void app_log(const char *text);
static inline void app_wait(unsigned ticks) {
    ArkThreadRequest r = {0};
    r.op = ARK_THREAD_WAIT;
    r.ticks = ticks;
    if (ark_thread(&r) < 0)
        ark_yield();
}
#endif
