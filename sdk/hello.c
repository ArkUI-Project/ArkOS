/* Build from the project root: sh sdk/build-app.sh sdk/hello.c build/hello.elf */
#include "app.h"
static uint32_t pixels[800 * 500];
int main(void) {
    ArkApp app;
    if (!app_open(&app, "Hello ArkOS", pixels, 800, 500))
        return 1;
    app_clear(&app, 0xf4f7fb);
    app_text(&app, 40, 36, "Hello, ArkOS! 你好，ArkOS！", 0x234d85, 2);
    app_text(&app, 40, 104, "A separate native user process. Click to draw.", 0x61758d, 1);
    app_present(&app);
    for (;;) {
        ArkEvent event;
        while (app_event(&app, &event)) {
            if (app_released(&app, &event))
                app_round(&app, event.x - 8, event.y - 8, 16, 16, 8, 0x4179de);
        }
        app_present(&app);
        app_wait(100);
    }
}
