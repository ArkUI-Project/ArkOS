#include "app.h"
static uint32_t pixels[800 * 500];
static ArkApp app;
static bool running, utc, pressed;
static uint64_t started, elapsed;
static void digits(char out[9], unsigned hours, unsigned minutes, unsigned seconds) {
    out[0] = '0' + hours / 10;
    out[1] = '0' + hours % 10;
    out[2] = ':';
    out[3] = '0' + minutes / 10;
    out[4] = '0' + minutes % 10;
    out[5] = ':';
    out[6] = '0' + seconds / 10;
    out[7] = '0' + seconds % 10;
    out[8] = 0;
}
static void draw(void) {
    int w = app_width(&app), h = app_height(&app), split = h / 2 + 34;
    ArkSystemInfo info = {0};
    (void)ark_info(&info);
    uint64_t now = ark_ticks(), span = elapsed + (running ? now - started : 0);
    int hour = (info.hour + (utc ? 0 : 8)) % 24;
    char clock[9], watch[9];
    digits(clock, (unsigned)hour, (unsigned)info.minute, (unsigned)info.second);
    digits(watch, (unsigned)(span / 360000) % 100, (unsigned)(span / 6000) % 60,
           (unsigned)(span / 100) % 60);
    app_clear(&app, 0xf1f5fa);
    app_text(&app, 30, 24, "时钟", 0x253851, 2);
    app_text(&app, 31, 65, "世界时间与秒表", 0x75879e, 1);
    app_round(&app, 24, 101, w - 48, split - 118, 18, 0xffffff);
    app_text(&app, 51, 120, utc ? "UTC" : "上海 · UTC+8", 0x6c8098, 1);
    app_text(&app, 49, 152, clock, 0x245aaf, w < 600 ? 2 : 4);
    app_button(&app, app_width(&app) - 197, split - 57, 173, 44, utc ? "上海" : "UTC", false);
    app_round(&app, 24, split, w - 48, h - split - 65, 18, 0xffffff);
    app_text(&app, 49, split + 20, "秒表", 0x6c8098, 1);
    app_text(&app, 49, split + 56, watch, 0x293f5d, w < 600 ? 2 : 3);
    app_button(&app, app_width(&app) - 318, app_height(&app) - 157, 122, 46,
               running ? "暂停" : "开始", true);
    app_button(&app, app_width(&app) - 181, app_height(&app) - 157, 119, 46, "重置", false);
    app_text(&app, 30, h - 37, "Space 开始或暂停 · R 重置 · U 切换时区", 0x7d8fa5, 1);
    app_present(&app);
}
static void toggle(void) {
    uint64_t now = ark_ticks();
    if (running)
        elapsed += now - started;
    else
        started = now;
    running = !running;
}
int main(void) {
    if (!app_open(&app, "时钟", pixels, 800, 500))
        return 1;
    draw();
    app_log("[app] Clock ring3 ready\n");
    uint64_t last = ark_ticks();
    for (;;) {
        ArkEvent e;
        bool dirty = false;
        while (app_event(&app, &e)) {
            if (e.type == ARK_EV_KEY) {
                if (e.key == ' ')
                    toggle();
                else if (e.key == 'r' || e.key == 'R') {
                    elapsed = 0;
                    started = ark_ticks();
                } else if (e.key == 'u' || e.key == 'U')
                    utc = !utc;
                dirty = true;
            } else if (e.type == ARK_EV_POINTER || e.type == ARK_EV_MOUSE ||
                       e.type == ARK_EV_TOUCH) {
                bool down = (e.buttons & 1) != 0;
                if (app_released(&app, &e)) {
                    if (app_click_hit(&app, &e, app_width(&app) - 318, app_height(&app) - 157, 122,
                                      46))
                        toggle();
                    else if (app_click_hit(&app, &e, app_width(&app) - 181, app_height(&app) - 157,
                                           119, 46)) {
                        elapsed = 0;
                        started = ark_ticks();
                    } else if (app_click_hit(&app, &e, app_width(&app) - 197,
                                             app_height(&app) / 2 - 23, 173, 44))
                        utc = !utc;
                    dirty = true;
                }
                pressed = down;
            }
        }
        uint64_t now = ark_ticks();
        if (dirty || app.dirty || now / (running ? 10 : 100) != last / (running ? 10 : 100))
            draw();
        last = now;
        app_wait(running ? 10 : 100);
    }
}
