#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/virtio_input.c"
static bool relative_enabled;
static Event spice_events[8];
static unsigned spice_read, spice_write;
bool spice_mouse_next_event(Event *event) {
    if (spice_read == spice_write)
        return false;
    *event = spice_events[spice_read++];
    return true;
}
bool spice_mouse_ready(void) {
    return false;
}
void spice_mouse_discard_events(void) {
    spice_read = spice_write;
}
static void spice_event(int x, int y, unsigned buttons) {
    assert(spice_write < 8);
    spice_events[spice_write++] = (Event){EV_POINTER, 0, x, y, (uint8_t)buttons};
}
void serial_write(const char *s) {
    (void)s;
}
void platform_set_relative_pointer_enabled(bool enabled) {
    relative_enabled = enabled;
}
void strcopy(char *d, const char *s, size_t cap) {
    if (!cap)
        return;
    size_t n = strlen(s);
    if (n >= cap)
        n = cap - 1;
    memcpy(d, s, n);
    d[n] = 0;
}
static uint8_t fake_common[2][64], fake_isr[2];
static uint16_t fake_notify[2];
static void setup(void) {
    memset(devices, 0, sizeof(devices));
    device_count = 2;
    memset(&spice_pointer, 0, sizeof(spice_pointer));
    spice_read = spice_write = 0;
    output_read = output_write = 0;
    last_source = 0;
    touch_priority = false;
    memset(&last_output, 0, sizeof(last_output));
    for (unsigned i = 0; i < 2; i++) {
        InputDevice *d = &devices[i];
        d->ready = true;
        d->primary_slot = -1;
        d->event_type = i ? EV_POINTER : EV_TOUCH;
        d->multitouch = i == 0;
        d->x_axis = d->y_axis = (Axis){0, 32767};
        d->common.address = fake_common[i];
        d->isr.address = &fake_isr[i];
        d->queues[0].size = QUEUE_CAP;
        d->queues[0].notify = &fake_notify[i];
        d->last_event = (Event){d->event_type, 0, 0, 0, 0};
    }
    refresh_priority();
    assert(!relative_enabled);
}
static void feed(unsigned which, unsigned type, unsigned code, int value) {
    Queue *q = &devices[which].queues[0];
    unsigned slot = q->used.index % QUEUE_CAP;
    q->events[slot] = (InputEvent){(uint16_t)type, (uint16_t)code, value};
    q->used.ring[slot] = (UsedElement){slot, 8};
    q->used.index++;
}
static void sync(unsigned which) {
    feed(which, 0, 0, 0);
}
static void pointer(int x, int y, int button, int down) {
    feed(1, 3, 0, x);
    feed(1, 3, 1, y);
    if (button >= 0)
        feed(1, 1, 0x110 + (unsigned)button, down);
    sync(1);
}
static void touch(int id, int x, int y) {
    feed(0, 3, 0x2f, 0);
    feed(0, 3, 0x39, id);
    if (id >= 0) {
        feed(0, 3, 0x35, x);
        feed(0, 3, 0x36, y);
    }
    sync(0);
}
static Event take(void) {
    Event e;
    assert(virtio_input_next_event(&e));
    return e;
}
static void empty(void) {
    Event e;
    assert(!virtio_input_next_event(&e));
}
int main(void) {
    setup();
    pointer(5000, 6000, -1, 0);
    Event e = take();
    assert(e.type == EV_POINTER && e.dx == 5000 && e.dy == 6000 && !e.buttons);
    empty();
    pointer(7000, 8000, 0, 1);
    pointer(7100, 8100, -1, 0);
    pointer(7200, 8200, 0, 0);
    e = take();
    assert(e.type == EV_POINTER && e.dx == 7000 && e.buttons == 1);
    e = take();
    assert(e.dx == 7100 && e.buttons == 1);
    e = take();
    assert(e.dx == 7200 && e.buttons == 0);
    empty();
    pointer(7500, 8500, 1, 1);
    e = take();
    assert(e.buttons == 2);
    pointer(7500, 8500, 2, 1);
    e = take();
    assert(e.buttons == 6);
    pointer(7500, 8500, 1, 0);
    e = take();
    assert(e.buttons == 4);
    pointer(7500, 8500, 2, 0);
    e = take();
    assert(e.buttons == 0);
    empty();
    pointer(7500, 8500, 3, 1);
    e = take();
    assert(e.buttons == 8);
    pointer(7500, 8500, 4, 1);
    e = take();
    assert(e.buttons == 24);
    feed(1, 2, 8, 1);
    sync(1);
    e = take();
    assert(e.type == EV_SCROLL && e.dx == 0 && e.dy == -1 && e.buttons == 24);
    empty();
    feed(1, 2, 6, -999);
    sync(1);
    e = take();
    assert(e.type == EV_SCROLL && e.dx == -127 && e.dy == 0);
    empty();
    pointer(7500, 8500, 3, 0);
    e = take();
    assert(e.buttons == 16);
    pointer(7500, 8500, 4, 0);
    e = take();
    assert(e.buttons == 0);
    empty();

    /* A held mouse drag is explicitly released before touch takes over. */
    pointer(10000, 11000, 0, 1);
    assert(take().buttons == 1);
    touch(7, 20000, 21000);
    e = take();
    assert(e.type == EV_POINTER && e.buttons == 0);
    e = take();
    assert(e.type == EV_TOUCH && e.dx == 20000 && e.buttons == 1);
    pointer(1000, 2000, -1, 0);
    empty(); /* Mouse hover cannot displace the finger. */
    feed(1, 2, 8, -1);
    sync(1);
    empty(); /* Wheel cannot scroll underneath an active touch. */
    touch(-1, 0, 0);
    e = take();
    assert(e.type == EV_TOUCH && !e.buttons);
    empty();
    pointer(3000, 4000, -1, 0);
    empty(); /* Held mouse cannot begin a new drag. */
    pointer(3000, 4000, 0, 0);
    e = take();
    assert(e.type == EV_POINTER && !e.buttons);
    pointer(9000, 10000, -1, 0);
    e = take();
    assert(e.dx == 9000 && !e.buttons);
    empty();

    setup();
    touch(2, 16000, 17000);
    e = take();
    assert(e.type == EV_TOUCH);
    /* Mouse frames queued before lift must be discarded at the boundary. */
    pointer(100, 200, -1, 0);
    pointer(300, 400, -1, 0);
    touch(-1, 0, 0);
    e = take();
    assert(e.type == EV_TOUCH && !e.buttons);
    empty();
    pointer(12000, 13000, -1, 0);
    e = take();
    assert(e.type == EV_POINTER && e.dx == 12000);
    empty();
    assert(virtio_input_contacts() == 0 && virtio_input_pointer_ready());
    setup();
    pointer(2000, 3000, 0, 1);
    assert(take().buttons == 1);
    spice_event(12000, 13000, 0);
    e = take();
    assert(!e.buttons && e.dx == 2000);
    e = take();
    assert(e.type == EV_POINTER && e.dx == 12000 && !e.buttons);
    empty();
    spice_event(14000, 15000, 1);
    assert(take().buttons == 1);
    touch(8, 20000, 21000);
    e = take();
    assert(e.type == EV_POINTER && !e.buttons);
    e = take();
    assert(e.type == EV_TOUCH && e.key == 8 && e.buttons == 1);
    spice_event(1, 2, 1);
    empty();
    touch(-1, 0, 0);
    assert(take().type == EV_TOUCH);
    empty();
    spice_event(10000, 11000, 1);
    empty();
    spice_event(10000, 11000, 0);
    e = take();
    assert(e.type == EV_POINTER && !e.buttons && e.dx == 10000);
    empty();
    puts("PASS separate mouse/touch events, five buttons, bounded wheel axes, press locations, "
         "takeover releases and stale-frame suppression");
}
