#include "app.h"
#include "text.h"
static ArkApp app;
static uint32_t pixels[800 * 500];
static char text[80], result[80] = "0";
static size_t length, cursor;
static bool held, error;
static unsigned depth;
static const char *p;
static int64_t expression(void);
static int64_t factor(void) {
    while (*p == ' ')
        p++;
    if (++depth > 32) {
        error = true;
        return 0;
    }
    bool neg = false;
    if (*p == '-' || *p == '+') {
        neg = *p == '-';
        p++;
    }
    int64_t n = 0;
    if (*p == '(') {
        p++;
        n = expression();
        if (*p != ')')
            error = true;
        else
            p++;
    } else {
        if (*p < '0' || *p > '9')
            error = true;
        while (*p >= '0' && *p <= '9') {
            int64_t t;
            if (__builtin_mul_overflow(n, (int64_t)10, &t) ||
                __builtin_add_overflow(t, (int64_t)(*p - '0'), &n)) {
                error = true;
                break;
            }
            p++;
        }
    }
    depth--;
    if (neg && __builtin_sub_overflow((int64_t)0, n, &n))
        error = true;
    return n;
}
static int64_t product(void) {
    int64_t n = factor();
    while (!error && (*p == '*' || *p == '/')) {
        char op = *p++;
        int64_t b = factor();
        if (op == '*') {
            if (__builtin_mul_overflow(n, b, &n))
                error = true;
        } else if (!b || (n == INT64_MIN && b == -1))
            error = true;
        else
            n /= b;
    }
    return n;
}
static int64_t expression(void) {
    int64_t n = product();
    while (!error && (*p == '+' || *p == '-')) {
        char op = *p++;
        int64_t b = product();
        if (op == '+' ? __builtin_add_overflow(n, b, &n) : __builtin_sub_overflow(n, b, &n))
            error = true;
    }
    return n;
}
static void eval(void) {
    p = text;
    depth = 0;
    error = false;
    int64_t n = expression();
    while (*p == ' ')
        p++;
    if (error || *p)
        strcopy(result, "表达式错误或整数溢出", sizeof result);
    else if (n < 0) {
        result[0] = '-';
        uint_to_str((uint64_t)(-(n + 1)) + 1, result + 1);
    } else
        uint_to_str((uint64_t)n, result);
}
static void key(int k) {
    if (k == KEY_ENTER || k == '=')
        eval();
    else if (k == 'c' || k == 'C') {
        text[0] = 0;
        length = cursor = 0;
        strcopy(result, "0", sizeof result);
    } else if ((k >= '0' && k <= '9') || k == '+' || k == '-' || k == '*' || k == '/' || k == '(' ||
               k == ')' || k == KEY_BACKSPACE || k == KEY_DELETE || k == KEY_LEFT || k == KEY_RIGHT)
        app_edit(text, &length, &cursor, sizeof text, k);
}
static const char *keys[] = {"7", "8", "9", "/", "4", "5", "6", "*",
                             "1", "2", "3", "-", "C", "0", "=", "+"};
static void draw(void) {
    int w = app_width(&app), h = app_height(&app), cw = (w - 48) / 4, ch = (h - 192) / 4;
    app_clear(&app, 0xf4f7fb);
    app_round(&app, 24, 20, w - 48, 110, 16, 0xffffff);
    app_clip(&app, 45, 40, text, w - 94, 0x74869b);
    app_clip(&app, 45, 84, result, w - 94, 0x245fb8);
    for (int i = 0; i < 16; i++)
        app_button(&app, 24 + (i % 4) * cw, 154 + (i / 4) * ch, cw - 12, ch - 12, keys[i],
                   i % 4 == 3 || i == 14);
    app_text(&app, 26, h - 30, "支持整数计算与括号 · 超出范围时会提示", 0x74869b, 1);
    app_present(&app);
}
int main(void) {
    if (!app_open(&app, "Calculator", pixels, 800, 500))
        return 1;
    draw();
    app_log("[app] Calculator ring3 ready\n");
    for (;;) {
        ArkEvent e;
        bool dirty = false;
        while (app_event(&app, &e)) {
            if (e.type == ARK_EV_KEY) {
                key(e.key);
                dirty = true;
            }
            if (e.type == ARK_EV_POINTER) {
                bool down = (e.buttons & 1) != 0;
                if (app_released(&app, &e)) {
                    int cw = (app_width(&app) - 48) / 4, ch = (app_height(&app) - 192) / 4;
                    for (int i = 0; i < 16; i++)
                        if (app_click_hit(&app, &e, 24 + (i % 4) * cw, 154 + (i / 4) * ch, cw - 12,
                                          ch - 12)) {
                            key(keys[i][0]);
                            dirty = true;
                        }
                }
                held = down;
            }
        }
        if (dirty || app.dirty)
            draw();
        app_wait(100);
    }
}
