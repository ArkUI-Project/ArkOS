#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/spice_mouse.c"
void serial_write(const char *s) {
    (void)s;
}
static void setup(unsigned w, unsigned h) {
    screen_width = w;
    screen_height = h;
    event_read = event_write = 0;
    last_mouse = delivered_mouse = (Event){EV_POINTER, 0, 16384, 16384, 0};
    memset(&parser, 0, sizeof(parser));
}
static unsigned message(uint8_t *p, unsigned port, unsigned type, const uint8_t *body,
                        unsigned size) {
    memset(p, 0, 28);
    put32(p, port);
    put32(p + 4, 20 + size);
    put32(p + 8, 1);
    put32(p + 12, type);
    put32(p + 24, size);
    if (size)
        memcpy(p + 28, body, size);
    return 28 + size;
}
static unsigned mouse(uint8_t *p, unsigned x, unsigned y, unsigned buttons, unsigned display) {
    uint8_t body[13];
    put32(body, x);
    put32(body + 4, y);
    put32(body + 8, buttons);
    body[12] = (uint8_t)display;
    return message(p, 2, 1, body, 13);
}
static Event take(void) {
    Event e;
    assert(spice_mouse_next_event(&e));
    return e;
}
static void empty(void) {
    Event e;
    assert(!spice_mouse_next_event(&e));
}
static void location(Event e, unsigned x, unsigned y, unsigned w, unsigned h) {
    assert(e.type == EV_POINTER);
    assert((uint64_t)e.dx * (w - 1) / 32767 == x);
    assert((uint64_t)e.dy * (h - 1) / 32767 == y);
}
int main(void) {
    uint8_t p[512], body[8] = {0};
    unsigned size;
    Event e;
    setup(1280, 800);
    size = mouse(p, 820, 130, 0, 0);
    for (unsigned i = 0; i < size; i++)
        assert(receive_bytes(p + i, 1));
    e = take();
    location(e, 820, 130, 1280, 800);
    empty();
    /* All byte splits, including a split mouse body and chunk/header. */
    for (unsigned split = 0; split <= 41; split++) {
        setup(3840, 2160);
        size = mouse(p, 3839, 2159, 2, 0);
        assert(receive_bytes(p, split));
        assert(receive_bytes(p + split, size - split));
        e = take();
        location(e, 3839, 2159, 3840, 2160);
        assert(e.buttons == 1);
        empty();
    }
    setup(1280, 800);
    unsigned used = 0;
    used += mouse(p + used, 80, 90, 2, 0);
    used += mouse(p + used, 180, 190, 2, 0);
    used += mouse(p + used, 180, 190, 0, 0);
    assert(receive_bytes(p, used));
    e = take();
    assert(e.buttons == 1);
    location(e, 80, 90, 1280, 800);
    e = take();
    assert(e.buttons == 1);
    location(e, 180, 190, 1280, 800);
    e = take();
    assert(!e.buttons);
    empty();
    /* Left/right/middle, wheel, and forward/back buttons retain their edges. */
    const unsigned masks[] = {8, 4, 64, 128}, buttons[] = {2, 4, 8, 16};
    for (unsigned i = 0; i < 4; i++) {
        size = mouse(p, 180, 190, masks[i], 0);
        assert(receive_bytes(p, size));
        assert(take().buttons == buttons[i]);
        size = mouse(p, 180, 190, 0, 0);
        assert(receive_bytes(p, size));
        assert(!take().buttons);
        empty();
    }
    size = mouse(p, 180, 190, 16, 0);
    assert(receive_bytes(p, size));
    e = take();
    assert(e.type == EV_SCROLL && e.dy == -1);
    size = mouse(p, 180, 190, 32, 0);
    assert(receive_bytes(p, size));
    e = take();
    assert(e.type == EV_SCROLL && e.dy == 1);
    empty();
    size = mouse(p, 100, 110, 2, 0);
    assert(receive_bytes(p, size));
    assert(take().buttons == 1);
    /* Disconnect releases a delivered drag even if its release was queued. */
    size = mouse(p, 100, 110, 0, 0);
    assert(receive_bytes(p, size));
    disconnect();
    e = take();
    assert(!e.buttons);
    empty();
    setup(1280, 800);
    size = mouse(p, 20, 30, 2, 0);
    assert(receive_bytes(p, size));
    assert(take().buttons == 1);
    size = message(p, 2, 13, 0, 0);
    assert(receive_bytes(p, size));
    assert(!take().buttons);
    empty();
    assert(!parser.header_used && !parser.remaining);
    /* Client and server messages are independently fragmented/interleaved. */
    setup(1280, 800);
    uint8_t server[41];
    mouse(server, 300, 400, 0, 0);
    put32(p, 2);
    put32(p + 4, 12);
    memcpy(p + 8, server + 8, 12);
    assert(receive_bytes(p, 20));
    put32(body, 1);
    size = message(p, 1, 6, body, 8);
    assert(receive_bytes(p, size));
    assert(caps_pending && !caps_request);
    put32(p, 2);
    put32(p + 4, 21);
    memcpy(p + 8, server + 20, 21);
    assert(receive_bytes(p, 29));
    e = take();
    location(e, 300, 400, 1280, 800);
    empty();
    /* Other display and clipboard/control bodies never become input. */
    size = mouse(p, 30, 40, 2, 1);
    assert(receive_bytes(p, size));
    empty();
    size = message(p, 1, 4, body, 8);
    assert(receive_bytes(p, size));
    empty();
    size = mouse(p, UINT32_MAX, UINT32_MAX, 0, 0);
    assert(receive_bytes(p, size));
    location(take(), 1279, 799, 1280, 800);
    empty();
    /* Malformed host lengths are rejected with bounded, allocation-free state. */
    setup(1280, 800);
    size = mouse(p, 10, 20, 1, 0);
    assert(!receive_bytes(p, size));
    setup(1280, 800);
    size = mouse(p, 10, 20, 0, 0);
    put32(p + 8, 2);
    assert(!receive_bytes(p, size));
    setup(1280, 800);
    size = mouse(p, 10, 20, 0, 0);
    put32(p, 3);
    assert(!receive_bytes(p, size));
    setup(1280, 800);
    put32(p, 2);
    put32(p + 4, CHUNK_CAP + 1);
    assert(!receive_bytes(p, 8));
    setup(1280, 800);
    size = message(p, 2, 1, 0, 0);
    assert(!receive_bytes(p, size));
    setup(1280, 800);
    size = message(p, 1, 4, 0, 0);
    put32(p + 24, MESSAGE_CAP + 1);
    assert(!receive_bytes(p, size));
    puts("PASS SPICE byte fragmentation, client/server interleaving, pixel mapping at 4K, "
         "button/drag edges, scroll, disconnect release and malformed lengths");
}
