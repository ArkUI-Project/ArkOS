/* Runs the actual three native app main loops with copied-surface/file services.
 * This tests application behavior, not CPU isolation (covered by QEMU probes). */
#include "app.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
static jmp_buf exited;
static const ArkEvent *events;
static unsigned event_count, frame, present_count, close_count, create_count, write_count,
    sync_count;
static bool delivered;
static int exit_code;
static bool fail_read;
static unsigned drop_receipts, drop_action;
static char saved_path[128], saved_text[16384];
static uint32_t snapshot[800 * 500];
int clock_main(void);
int paint_main(void);
int markdown_main(void);
void strcopy(char *d, const char *s, size_t cap) {
    if (!cap)
        return;
    size_t n = strlen(s);
    if (n >= cap)
        n = cap - 1;
    memmove(d, s, n);
    d[n] = 0;
}
void uint_to_str(uint64_t n, char *out) {
    snprintf(out, 24, "%llu", (unsigned long long)n);
}
int64_t ark_test_syscall6(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                          uint64_t f) {
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    if (n == ARK_SYS_THREAD) {
        ArkThreadRequest *q = (ArkThreadRequest *)(uintptr_t)a;
        assert(b == sizeof *q && q->op == ARK_THREAD_WAIT);
        assert(frame < 100);
        frame++;
        delivered = false;
        return 0;
    }
    if (n == ARK_SYS_TICKS)
        return (int64_t)frame * 100;
    if (n == ARK_SYS_YIELD) {
        assert(frame < 100);
        frame++;
        delivered = false;
        return 0;
    }
    if (n == ARK_SYS_EXIT) {
        exit_code = (int)a;
        longjmp(exited, 1);
    }
    if (n == ARK_SYS_LOG) {
        assert(b <= 4096);
        return 0;
    }
    if (n == ARK_SYS_INFO) {
        assert(b == sizeof(ArkSystemInfo));
        ArkSystemInfo *r = (void *)(uintptr_t)a;
        memset(r, 0, sizeof(*r));
        r->abi = 1;
        r->hour = 12;
        r->minute = 34;
        r->second = 56;
        return 0;
    }
    if (n == ARK_SYS_REGISTRY)
        return -2;
    if (n == ARK_SYS_DRAG) {
        ArkDragRequest *r = (void *)(uintptr_t)a;
        assert(b == sizeof *r && r->target == 3 && r->token == 17);
        if (r->op == ARK_DRAG_READ) {
            r->kind = ARK_DRAG_FILE;
            strcopy(r->data, "Dropped.md", sizeof r->data);
            r->length = (unsigned)strlen(r->data);
            return 0;
        }
        assert(r->op == ARK_DRAG_ACCEPT);
        drop_receipts++;
        drop_action = r->action;
        return 0;
    }
    if (n == ARK_SYS_SURFACE) {
        assert(b == sizeof(ArkSurfaceRequest));
        ArkSurfaceRequest *r = (void *)(uintptr_t)a;
        if (r->op == ARK_SURFACE_CREATE) {
            assert(r->width == 800 && r->height == 500);
            r->id = 3;
            create_count++;
            return 0;
        }
        assert(r->id == 3);
        if (r->op == ARK_SURFACE_PRESENT) {
            assert(r->width == 800 && r->height == 500 && r->stride == 800);
            assert(r->flags == ARK_PRESENT_FULL);
            memcpy(snapshot, (void *)(uintptr_t)r->pixels, sizeof(snapshot));
            present_count++;
            return 0;
        }
        if (r->op == ARK_SURFACE_NEXT_EVENT) {
            if (delivered)
                return 0;
            delivered = true;
            if (frame >= event_count) {
                r->event = (ArkEvent){.type = ARK_EV_CLOSE};
                return 1;
            }
            r->event = events[frame];
            return r->event.type ? 1 : 0;
        }
        if (r->op == ARK_SURFACE_INPUT)
            return 0;
        if (r->op == ARK_SURFACE_CURSORS) {
            assert(r->capacity <= 32);
            ArkCursorRegion *regions = (void *)(uintptr_t)r->pixels;
            for (unsigned i = 0; i < r->capacity; i++) {
                assert(regions[i].shape < ARK_CURSOR_COUNT);
                assert(regions[i].rect.x >= 0 && regions[i].rect.y >= 0 &&
                       regions[i].rect.x + regions[i].rect.w <= 800 &&
                       regions[i].rect.y + regions[i].rect.h <= 500);
            }
            return 0;
        }
        if (r->op == ARK_SURFACE_CLOSE) {
            close_count++;
            return 0;
        }
        assert(!"unexpected surface request");
    }
    if (n == ARK_SYS_FILE) {
        assert(b == sizeof(ArkFileRequest));
        ArkFileRequest *r = (void *)(uintptr_t)a;
        if (r->op == ARK_FILE_CREATE)
            return 0;
        if (r->op == ARK_FILE_WRITE) {
            assert(r->capacity <= ARK_FILE_MAX);
            strcopy(saved_path, r->path, sizeof(saved_path));
            memcpy(saved_text, (void *)(uintptr_t)r->buffer, r->capacity);
            saved_text[r->capacity] = 0;
            write_count++;
            return 0;
        }
        if (r->op == ARK_FILE_SYNC) {
            sync_count++;
            return 0;
        }
        if (r->op == ARK_FILE_READ) {
            if (fail_read) {
                memset((void *)(uintptr_t)r->buffer, '?', r->capacity);
                strcopy(r->error, "Unable to read document", sizeof r->error);
                return -5;
            }
            const char *t = "# Loaded from disk\n\n- native file service\n";
            size_t z = strlen(t);
            assert(z <= r->capacity);
            memcpy((void *)(uintptr_t)r->buffer, t, z);
            r->count = (uint32_t)z;
            return 0;
        }
        assert(!"unexpected file request");
    }
    assert(!"unexpected syscall");
    return -1;
}
static void run(int (*entry)(void), const ArkEvent *script, unsigned count) {
    events = script;
    event_count = count;
    frame = present_count = close_count = create_count = write_count = sync_count = drop_receipts =
        drop_action = 0;
    delivered = false;
    exit_code = -1;
    saved_path[0] = saved_text[0] = 0;
    memset(snapshot, 0, sizeof(snapshot));
    if (!setjmp(exited)) {
        (void)entry();
        assert(!"application unexpectedly returned");
    }
    assert(exit_code == 0 && create_count == 1 && close_count == 1 && present_count >= 2);
    assert(snapshot[0] != 0); /* a rendered surface, not a static launch placeholder */
}
int main(void) {
    const ArkEvent clock_events[] = {{.type = ARK_EV_KEY, .key = ' '},
                                     {0},
                                     {.type = ARK_EV_KEY, .key = 'r'},
                                     {.type = ARK_EV_KEY, .key = 'u'}};
    run(clock_main, clock_events, sizeof(clock_events) / sizeof(*clock_events));
    assert(!write_count);
    const ArkEvent paint_events[] = {{.type = ARK_EV_POINTER, .x = 100, .y = 150, .buttons = 1},
                                     {.type = ARK_EV_POINTER, .x = 140, .y = 180, .buttons = 1},
                                     {.type = ARK_EV_POINTER, .x = 180, .y = 180, .buttons = 1},
                                     {.type = ARK_EV_POINTER, .x = 180, .y = 180},
                                     {.type = ARK_EV_KEY, .key = 's'}};
    run(paint_main, paint_events, sizeof(paint_events) / sizeof(*paint_events));
    assert(write_count == 1 && sync_count == 1);
    assert(!strcmp(saved_path, "Paint.svg"));
    assert(strstr(saved_text, "<svg") && strstr(saved_text, "<path") &&
           strstr(saved_text, "</svg>"));
    const ArkEvent markdown_events[] = {{.type = ARK_EV_KEY, .key = 'o'},
                                        {.type = ARK_EV_KEY, .key = KEY_DOWN},
                                        {.type = ARK_EV_KEY, .key = 19}};
    run(markdown_main, markdown_events, sizeof(markdown_events) / sizeof(*markdown_events));
    assert(write_count == 1 && sync_count == 1);
    assert(!strcmp(saved_path, "Welcome.md"));
    assert(strstr(saved_text, "Loaded from disk"));
    const ArkEvent markdown_drop[] = {{.type = ARK_EV_DROP, .key = 17},
                                      {.type = ARK_EV_KEY, .key = 19}};
    fail_read = true;
    run(markdown_main, markdown_drop, 2);
    assert(drop_receipts == 1 && drop_action == 0);
    assert(!strcmp(saved_path, "Welcome.md") && strstr(saved_text, "# 欢迎"));
    fail_read = false;
    run(markdown_main, markdown_drop, 2);
    assert(drop_receipts == 1 && drop_action == ARK_DRAG_COPY);
    assert(!strcmp(saved_path, "Dropped.md") && strstr(saved_text, "Loaded from disk"));
    struct {
        uint32_t before, pixels[32 * 32], after;
    } guard = {.before = 0xcafebabe, .after = 0xdeadbeef};
    ArkApp a = {.width = 32, .height = 32, .scale = 1, .pixels = guard.pixels};
    app_clear(&a, 0xf3f4f5);
    app_rect(&a, -20, -20, 30, 30, 0xff335577);
    app_round(&a, 25, 20, 20, 20, 10, 0x456789);
    app_text(&a, -4, 20, "中文 clipped", 0x123456, 1);
    app_line(&a, -10, -10, 45, 45, 0x445566, 3);
    assert(guard.before == 0xcafebabe && guard.after == 0xdeadbeef);
    const ArkEvent edge_events[] = {{.type = ARK_EV_POINTER, .x = 5, .y = 5, .buttons = 1},
                                    {.type = ARK_EV_POINTER, .x = 15, .y = 5},
                                    {.type = ARK_EV_POINTER, .x = 5, .y = 5, .buttons = 1},
                                    {.type = ARK_EV_POINTER, .x = 5, .y = 5},
                                    {.type = ARK_EV_POINTER, .x = 5, .y = 5, .buttons = 1},
                                    {.type = ARK_EV_DRAG_END},
                                    {.type = ARK_EV_POINTER, .x = 5, .y = 5}};
    events = edge_events;
    event_count = 7;
    a.id = 3;
    a.closed = false;
    frame = 0;
    delivered = false;
    ArkEvent e;
    assert(app_poll_event(&a, &e) && !app_released(&a, &e));
    frame++;
    delivered = false;
    assert(app_poll_event(&a, &e) && !app_click_hit(&a, &e, 0, 0, 10, 10));
    frame++;
    delivered = false;
    assert(app_poll_event(&a, &e) && !app_released(&a, &e));
    frame++;
    delivered = false;
    assert(app_poll_event(&a, &e) && app_click_hit(&a, &e, 0, 0, 10, 10));
    frame++;
    delivered = false;
    assert(app_poll_event(&a, &e));
    frame++;
    delivered = false;
    assert(app_poll_event(&a, &e));
    frame++;
    delivered = false;
    assert(app_poll_event(&a, &e) && !app_released(&a, &e));

    puts("Native user apps PASS: Clock events, Paint pointer SVG export+sync, Markdown read/save "
         "and drop success/failure preserving prior content, copied surfaces, close exit and "
         "clipped SDK rendering");
    return 0;
}
