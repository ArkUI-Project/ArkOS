#include "app.h"
#include "app_permissions.h"
static ArkApp app;
static uint32_t pixels[800 * 500];
static bool running, held;
static uint64_t remaining = 30000, deadline;
static unsigned last_seconds = ~0u;
static void publish(void) {
    (void)app_activity(running, deadline);
}
static void toggle(void) {
    uint64_t now = ark_ticks();
    if (running) {
        remaining = deadline > now ? deadline - now : 0;
        running = false;
    } else if (remaining) {
        deadline = now + remaining;
        running = true;
    }
    publish();
}
static void draw(void) {
    int w = app_width(&app), h = app_height(&app), step = (w - 72) / 3;
    uint64_t now = ark_ticks(), ticks = running ? (deadline > now ? deadline - now : 0) : remaining;
    char n[24];
    app_clear(&app, 0xf4f7fb);
    app_text(&app, 30, 26, "专注计时器", 0x293e58, 2);
    app_round(&app, 24, 100, w - 48, h - 332, 18, 0xffffff);
    uint_to_str((ticks + 99) / 100 / 60, n);
    app_text(&app, 48, 125, n, 0x2676d2, 3);
    app_text(&app, w / 2 - 14, 144, "分", 0x6b8199, 1);
    uint_to_str((ticks + 99) / 100 % 60, n);
    app_text(&app, w / 2 + 26, 125, n, 0x2676d2, 3);
    app_text(&app, w - 66, 144, "秒", 0x6b8199, 1);
    app_button(&app, 36, app_height(&app) - 204, (app_width(&app) - 88) / 2, 48,
               running ? "暂停" : "开始", true);
    app_button(&app, 44 + (app_width(&app) - 88) / 2, app_height(&app) - 204,
               (app_width(&app) - 88) / 2, 48, "重置", false);
    const char *labels[] = {"1 分钟", "5 分钟", "25 分钟"};
    for (int i = 0; i < 3; i++)
        app_button(&app, 36 + i * step, h - 130, step - 10, 44, labels[i], false);
    app_text(&app, 32, h - 38, "Space 开始或暂停 · R 重置 · 计时期间可查看顶部提示", 0x6b8199, 1);
    app_present(&app);
}
int main(void) {
    if (!app_open(&app, "Timer", pixels, 800, 500))
        return 1;
    app_log("[app] Timer ring3 ready\n");
    draw();
    for (;;) {
        bool dirty = false;
        ArkEvent e;
        while (app_event(&app, &e)) {
            if (e.type == ARK_EV_KEY) {
                if (e.key == ' ')
                    toggle();
                else if (e.key == 'r' || e.key == 'R') {
                    running = false;
                    remaining = 30000;
                    publish();
                }
                dirty = true;
            }
            if (e.type == ARK_EV_POINTER) {
                bool down = (e.buttons & 1) != 0;
                if (app_released(&app, &e)) {
                    if (app_click_hit(&app, &e, 36, app_height(&app) - 204,
                                      (app_width(&app) - 88) / 2, 48))
                        toggle();
                    else if (app_click_hit(&app, &e, 44 + (app_width(&app) - 88) / 2,
                                           app_height(&app) - 204, (app_width(&app) - 88) / 2,
                                           48)) {
                        running = false;
                        remaining = 30000;
                        publish();
                    } else
                        for (int i = 0; i < 3; i++)
                            if (app_click_hit(&app, &e, 36 + i * (app_width(&app) - 72) / 3,
                                              app_height(&app) - 130,
                                              (app_width(&app) - 72) / 3 - 10, 44)) {
                                running = false;
                                remaining = (i == 0 ? 1 : i == 1 ? 5 : 25) * 6000;
                                publish();
                            }
                    dirty = true;
                }
                held = down;
            }
        }
        uint64_t now = ark_ticks();
        if (running && now >= deadline) {
            running = false;
            remaining = 0;
            publish();
            app_log("[timer] Native countdown complete\n");
            dirty = true;
        }
        unsigned seconds = (unsigned)((running ? deadline - now : remaining) / 100);
        if (running && seconds != last_seconds)
            dirty = true;
        last_seconds = seconds;
        if (dirty || app.dirty)
            draw();
        app_wait(running ? 10 : 100);
    }
}
