#include <assert.h>
#include <stdio.h>
#define ARK_INPUT_TEST 1
#include "../kernel/platform.c"

static void reset_input(void) {
    event_head = event_tail = 0;
    mouse_at = 0;
    mouse_length = 3;
    mouse_id = 0;
    ticks = mouse_byte_tick = 0;
    last_mouse_buttons = 0;
    relative_pointer_enabled = true;
    mouse_wait_release = overflow_mouse_pending = false;
}
static void packet(unsigned flags, unsigned x, unsigned y) {
    controller_byte(0x21, (uint8_t)flags);
    controller_byte(0x21, (uint8_t)x);
    controller_byte(0x21, (uint8_t)y);
}
static Event take(void) {
    Event e;
    assert(platform_next_event(&e));
    return e;
}
static void empty(void) {
    Event e;
    assert(!platform_next_event(&e));
}
int main(void) {
    reset_input();
    memset(keyboards, 0, sizeof keyboards);
    caps_lock = false;
    keyboard_byte(0x27);
    assert(take().key == ';');
    keyboard_byte(0x2a);
    keyboard_byte(0x27);
    assert(take().key == ':');
    keyboard_byte(0x36);
    keyboard_byte(0xaa);
    keyboard_byte(0x27);
    assert(take().key == ':');
    keyboard_byte(0xb6);
    keyboard_byte(0x27);
    assert(take().key == ';');
    /* Reproducer: a second keyboard releases the same left Shift. */
    platform_keyboard_device_input(1, 42, 1);
    keyboard_byte(0x2a);
    keyboard_byte(0xaa);
    platform_keyboard_device_input(1, 39, 1);
    assert(take().key == ':');
    platform_keyboard_device_input(2, 42, 1);
    platform_keyboard_device_input(1, 42, 0);
    platform_keyboard_device_input(2, 39, 2);
    assert(take().key == ':');
    platform_keyboard_device_input(2, 42, 0);
    platform_keyboard_device_input(2, 39, 1);
    assert(take().key == ';');
    keyboard_byte(0x3a);
    keyboard_byte(0x3a);
    keyboard_byte(0xba);
    keyboard_byte(0x27);
    assert(take().key == ';');
    keyboard_byte(0x1e);
    assert(take().key == 'A');
    keyboard_byte(0x2a);
    keyboard_byte(0x27);
    assert(take().key == ':');
    keyboard_byte(0xaa);
    memset(keyboards, 0, sizeof keyboards);
    caps_lock = false;
    empty();
    puts("PASS colon/semicolon, left/right Shift, CapsLock repeat and independent PS/2 + two "
         "VirtIO keyboards");
    reset_input();
    packet(0x08, 200, 128);
    Event e = take();
    assert(e.dx == 200 && e.dy == -128 && e.buttons == 0);
    empty();
    packet(0x38, 200, 128);
    e = take();
    assert(e.dx == -56 && e.dy == 128);
    empty();
    packet(0xc8, 0, 0);
    e = take();
    assert(e.dx == 255 && e.dy == -255);
    empty();
    packet(0xf8, 0, 0);
    e = take();
    assert(e.dx == -256 && e.dy == 256);
    empty();

    reset_input();
    controller_byte(0x21, 0);
    controller_byte(0x21, 0x08);
    controller_byte(0xe1, 0);
    packet(0x09, 12, 34);
    e = take();
    assert(e.dx == 12 && e.dy == -34 && e.buttons == 1);
    empty();
    reset_input();
    controller_byte(0x21, 0x08);
    controller_byte(0x21, 17);
    ticks = 5;
    packet(0x09, 12, 34);
    e = take();
    assert(e.dx == 12 && e.dy == -34 && e.buttons == 1);
    empty();

    reset_input();
    for (int i = 0; i < 6000; i++)
        enqueue((Event){EV_MOUSE, 0, 1, 0, 0});
    enqueue((Event){EV_MOUSE, 0, 0, 0, 1});
    for (int i = 0; i < 2000; i++)
        enqueue((Event){EV_MOUSE, 0, 1, 0, 1});
    enqueue((Event){EV_MOUSE, 0, 0, 0, 0});
    e = take();
    assert(e.dx == 6000 && e.buttons == 0);
    e = take();
    assert(e.dx == 0 && e.buttons == 1); /* Press location unchanged. */
    e = take();
    assert(e.dx == 2000 && e.buttons == 1);
    e = take();
    assert(e.dx == 0 && e.buttons == 0);
    empty();
    enqueue((Event){EV_MOUSE, 0, 8, 0, 0});
    enqueue((Event){EV_MOUSE, 0, -8, 0, 0});
    assert(take().dx == 8);
    assert(take().dx == -8);
    empty();

    reset_input();
    for (unsigned i = 0; i < EVENT_CAP - 1; i++)
        enqueue((Event){EV_MOUSE, 0, i & 1 ? 1 : -1, 0, 0});
    enqueue((Event){EV_MOUSE, 0, 0, 0, 1});
    enqueue((Event){EV_MOUSE, 0, 0, 0, 0});
    unsigned transitions = 0;
    uint8_t state = 0;
    while (platform_next_event(&e))
        if (e.buttons != state) {
            state = e.buttons;
            transitions++;
        }
    assert(transitions == 2 && state == 0);

    /* Saturate controls, then recover while a deferred press is followed by
     * an admitted release: the deferred state must never re-press it. */
    reset_input();
    for (unsigned i = 0; i < EVENT_CAP - 1; i++)
        enqueue((Event){EV_KEY, 'a', 0, 0, 0});
    enqueue((Event){EV_MOUSE, 0, 0, 0, 1});
    assert(take().type == EV_KEY);
    enqueue((Event){EV_MOUSE, 0, 0, 0, 0});
    while (platform_next_event(&e))
        if (e.type == EV_MOUSE)
            assert(e.buttons == 0);

    reset_input();
    enqueue((Event){EV_MOUSE, 0, 90, 0, 0});
    enqueue((Event){EV_KEY, 'a', 0, 0, 0});
    platform_set_relative_pointer_enabled(false);
    packet(0x09, 40, 40);
    e = take();
    assert(e.type == EV_KEY && e.key == 'a');
    empty();
    platform_set_relative_pointer_enabled(true);
    packet(0x09, 50, 50);
    empty();
    packet(0x08, 0, 0);
    empty();
    packet(0x08, 1, 0);
    assert(take().dx == 1);
    empty();
    reset_input();
    mouse_length = 4;
    mouse_id = 4;
    packet(0x08, 0, 0);
    controller_byte(0x21, 0x1f);
    e = take();
    assert(e.type == EV_MOUSE && e.buttons == 8);
    e = take();
    assert(e.type == EV_SCROLL && e.dy == -1 && e.buttons == 8);
    empty();
    packet(0x08, 0, 0);
    controller_byte(0x21, 0x01);
    e = take();
    assert(e.type == EV_MOUSE && e.buttons == 0);
    e = take();
    assert(e.type == EV_SCROLL && e.dy == 1);
    empty();
    reset_input();
    mouse_length = 4;
    mouse_id = 3;
    packet(0x08, 0, 0);
    controller_byte(0x21, 0xfe);
    e = take();
    assert(e.type == EV_SCROLL && e.dy == -2);
    empty();
    puts("PASS PS/2 wheel/5-button extensions and 9-bit signs, saturation, parity/gap resync, "
         "motion coalescing, click edges and source suppression");
}
