/* Separate, unprivileged native HTML reader. A full engine is not claimed. */
#include "app.h"
#include "drag.h"
#include "text.h"
#include "app_permissions.h"
#include "html.h"
static uint32_t pixels[800 * 500];
static ArkApp app;
static ArkHTML document;
static char url[256] = "ark:home", body[16385], message[128] = "离线欢迎页", history[8][256];
static unsigned history_count, history_at, scroll, size, hit_count;
static size_t length = 8, cursor = 8;
static struct {
    int x, y, w;
    unsigned link;
} hits[256];
static bool editing = true, select_all, held, active, awaiting;
static unsigned state, http_status;
static uint64_t next_poll;
static void home(void) {
    const char *p = "<h1>欢迎使用浏览器</h1><p>在上方输入网站地址，按 Enter "
                    "打开。</p><p>首次访问需要允许网络权限，可在系统设置中修改。</"
                    "p><p>支持阅读网页文字和打开链接，安全网站会检查证书。</p>";
    strcopy(body, p, sizeof body);
    size = (unsigned)strlen(body);
    ark_html_parse(&document, body, size, true);
    scroll = 0;
    state = 0;
    http_status = 0;
    strcopy(message, "完全离线的欢迎页", sizeof message);
}
static void request(void) {
    if (!strcmp(url, "ark:home")) {
        home();
        active = awaiting = false;
        return;
    }
    int64_t permission = app_request_permission(ARK_CAP_NETWORK);
    if (permission < 0) {
        awaiting = permission == -11;
        strcopy(message, awaiting ? "等待系统网络授权…" : "网络权限已拒绝；可在系统设置中修改",
                sizeof message);
        return;
    }
    ArkNetworkRequest q = {0};
    q.op = ARK_NET_HTTP_GET;
    strcopy(q.url, url, sizeof q.url);
    int64_t r = ark_network(&q);
    active = r >= 0;
    awaiting = false;
    size = scroll = 0;
    memset(&document, 0, sizeof document);
    strcopy(message,
            r >= 0         ? "正在请求…"
            : q.message[0] ? q.message
                           : "网络服务拒绝请求",
            sizeof message);
    state = q.state;
    http_status = q.http_status;
    editing = false;
    select_all = false;
    if (r >= 0 && (!history_count || strcmp(history[history_at], url))) {
        if (history_count)
            history_count = history_at + 1;
        if (history_count == 8) {
            memmove(history, history + 1, 7 * 256);
            history_count--;
        }
        history_at = history_count++;
        strcopy(history[history_at], url, 256);
    }
}
static void launch_argument(void) {
    ArkLaunchInfo q = {0};
    if (app_launch_info(&q) >= 0 && q.argument[0]) {
        strcopy(url, q.argument, sizeof url);
        length = cursor = strlen(url);
        request();
    }
}
static void navigate(int step) {
    int n = (int)history_at + step;
    if (n < 0 || n >= (int)history_count)
        return;
    history_at = (unsigned)n;
    strcopy(url, history[n], sizeof url);
    length = cursor = strlen(url);
    request();
}
static void draw(void) {
    int w = app_width(&app), h = app_height(&app), rows = (h - 208) / 24;
    if (rows < 1)
        rows = 1;
    app_clear(&app, 0xf4f7fb);
    app_cursor(&app, 16, 14, w - 260, 38, ARK_CURSOR_TEXT);
    app_round(&app, 16, 14, app_width(&app) - 260, 38, 10, select_all ? 0xd5e5ff : 0xffffff);
    app_clip(&app, 26, 24, url, w - 280, 0x304662);
    app_button(&app, app_width(&app) - 234, 14, 76, 38, "访问", true);
    app_button(&app, app_width(&app) - 150, 14, 40, 38, "<", false);
    app_button(&app, app_width(&app) - 104, 14, 40, 38, ">", false);
    app_button(&app, app_width(&app) - 58, 14, 42, 38, "停", false);
    app_clip(&app, 22, 68, message, w - 44, 0x597692);
    app_round(&app, 16, 104, w - 32, h - 167, 12, 0xffffff);
    int row = 0, col = 0;
    const char *p = document.text, *end = p + document.length;
    hit_count = 0;
    while (p < end) {
        const char *s = p;
        int cp = utf8_decode(&p);
        if (cp == '\r')
            continue;
        if (cp == '\n') {
            row++;
            col = 0;
            continue;
        }
        int adv = cp == '\t' ? 32 : cp < 128 ? 8 : 16;
        if (col + adv > w - 64) {
            row++;
            col = 0;
        }
        if (row >= (int)scroll && row < (int)scroll + rows && cp >= 32) {
            char b[8];
            size_t n = (size_t)(p - s);
            if (n > 7)
                n = 7;
            memcpy(b, s, n);
            b[n] = 0;
            unsigned at = (unsigned)(s - document.text), link = document.link[at];
            int x = 30 + col, y = 120 + (row - (int)scroll) * 24;
            app_text(&app, x, y, b, link || document.style[at] ? 0x2376d6 : 0x304662, 1);
            if (link) {
                app_rect(&app, x, y + 20, adv, 1, 0x2376d6);
                if (hit_count && hits[hit_count - 1].link == link && hits[hit_count - 1].y == y &&
                    hits[hit_count - 1].x + hits[hit_count - 1].w == x)
                    hits[hit_count - 1].w += adv;
                else if (hit_count < 256) {
                    hits[hit_count].x = x;
                    hits[hit_count].y = y;
                    hits[hit_count].w = adv;
                    hits[hit_count++].link = link;
                }
            }
        }
        col += adv;
    }
    if (scroll > (unsigned)(row >= rows ? row - rows + 1 : 0))
        scroll = (unsigned)(row >= rows ? row - rows + 1 : 0);
    char n[24];
    app_text(&app, 22, h - 49, "HTTP", 0x7387a0, 1);
    uint_to_str(http_status, n);
    app_text(&app, 70, h - 49, n, 0x7387a0, 1);
    uint_to_str(size, n);
    app_text(&app, 140, h - 49, n, 0x7387a0, 1);
    app_text(&app, 220, h - 49, "字节 · Ctrl+L 地址 · PageUp/Down 滚动", 0x7387a0, 1);
    app_text(&app, 22, h - 23, "简洁阅读模式 · 显示网页文字与链接", 0x7387a0, 1);
    app_text_input(&app, editing, 26, 24, 20);
    app_present(&app);
}
int main(void) {
    if (!app_open(&app, "Browser", pixels, 800, 500))
        return 1;
    home();
    launch_argument();
    draw();
    app_log("[app] Browser ring3 ready (UI, optional NETWORK; no SYSTEM)\n");
    for (;;) {
        bool dirty = false;
        ArkEvent e;
        while (app_event(&app, &e)) {
            if (e.type == ARK_EV_DROP) {
                ArkDragRequest drop;
                if (app_drop_read(&app, &e, &drop)) {
                    bool ok = drop.kind == ARK_DRAG_TEXT && strlen(drop.data) < sizeof url;
                    if (ok) {
                        strcopy(url, drop.data, sizeof url);
                        length = cursor = strlen(url);
                        editing = true;
                        select_all = false;
                    }
                    app_drop_accept(&app, &drop, ok);
                    dirty = true;
                }
            }
            if (e.type == ARK_EV_OPEN) {
                launch_argument();
                dirty = true;
            }
            if (e.type == ARK_EV_SCROLL) {
                int amount = e.y;
                scroll = amount < 0 ? (scroll > (unsigned)-amount ? scroll + (unsigned)amount : 0)
                                    : scroll + (unsigned)amount;
                dirty = true;
            }
            if (e.type == ARK_EV_TEXT && editing) {
                if (select_all) {
                    url[0] = 0;
                    length = cursor = 0;
                    select_all = false;
                }
                app_codepoint(url, &length, &cursor, sizeof url, (uint32_t)e.key);
                dirty = true;
            }
            if (e.type == ARK_EV_KEY) {
                int k = e.key;
                if (k == 12) {
                    editing = true;
                    select_all = true;
                } else if (k == KEY_ENTER || k == 18)
                    request();
                else if (k == KEY_PAGE_UP || k == KEY_UP)
                    scroll = scroll > 10 ? scroll - 10 : 0;
                else if (k == KEY_PAGE_DOWN || k == KEY_DOWN)
                    scroll += 10;
                else if (editing) {
                    if (k == 1)
                        select_all = true;
                    else {
                        if (select_all &&
                            (k == KEY_BACKSPACE || k == KEY_DELETE || (k >= 32 && k < 127))) {
                            url[0] = 0;
                            length = cursor = 0;
                            select_all = false;
                        }
                        app_edit(url, &length, &cursor, sizeof url, k);
                    }
                }
                dirty = true;
            }
            if (e.type == ARK_EV_POINTER) {
                bool down = (e.buttons & 1) != 0;
                if (app_released(&app, &e)) {
                    if (app_click_hit(&app, &e, 16, 14, app_width(&app) - 260, 38)) {
                        editing = true;
                        select_all = true;
                    } else if (app_click_hit(&app, &e, app_width(&app) - 234, 14, 76, 38))
                        request();
                    else if (app_click_hit(&app, &e, app_width(&app) - 150, 14, 40, 38))
                        navigate(-1);
                    else if (app_click_hit(&app, &e, app_width(&app) - 104, 14, 40, 38))
                        navigate(1);
                    else if (app_click_hit(&app, &e, app_width(&app) - 58, 14, 42, 38)) {
                        ArkNetworkRequest q = {.op = ARK_NET_HTTP_CANCEL};
                        ark_network(&q);
                        active = awaiting = false;
                        strcopy(message, "已停止", sizeof message);
                    } else
                        for (unsigned i = 0; i < hit_count; i++)
                            if (app_click_hit(&app, &e, hits[i].x, hits[i].y, hits[i].w, 24)) {
                                char next[256];
                                if (ark_html_resolve(url, document.href[hits[i].link - 1], next)) {
                                    strcopy(url, next, sizeof url);
                                    length = cursor = strlen(url);
                                    request();
                                }
                                break;
                            }
                    dirty = true;
                }
                held = down;
            }
        }
        uint64_t now = ark_ticks();
        if (now >= next_poll) {
            next_poll = now + 10;
            if (awaiting) {
                int64_t p = app_request_permission(ARK_CAP_NETWORK);
                if (!p) {
                    request();
                    dirty = true;
                } else if (p != -11) {
                    awaiting = false;
                    strcopy(message, "网络访问已拒绝", sizeof message);
                    dirty = true;
                }
            }
            if (active) {
                ArkNetworkRequest q = {.op = ARK_NET_HTTP_STATE};
                if (ark_network(&q) < 0) {
                    active = false;
                    strcopy(message, "网络权限已撤销或请求已取消", sizeof message);
                    dirty = true;
                } else {
                    if (state != q.state) {
                        state = q.state;
                        dirty = true;
                    }
                    http_status = q.http_status;
                    if (q.state == 6 || q.state == 7) {
                        active = false;
                        if (q.state == 6) {
                            ArkNetworkRequest r = {.op = ARK_NET_HTTP_READ,
                                                   .buffer = (uintptr_t)body,
                                                   .capacity = 16384};
                            if (ark_network(&r) >= 0) {
                                size = r.count;
                                body[size] = 0;
                                ark_html_parse(&document, body, size,
                                               !strncmp(q.content_type, "text/html", 9));
                                strcopy(message, "页面已加载", sizeof message);
                                app_log("[browser] Native page received in isolated process\n");
                            }
                        } else
                            strcopy(message, q.message, sizeof message);
                        dirty = true;
                    }
                }
            }
        }
        if (dirty || app.dirty)
            draw();
        app_wait(active || awaiting ? 10 : 100);
    }
}
