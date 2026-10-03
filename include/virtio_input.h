#ifndef ARK_VIRTIO_INPUT_H
#define ARK_VIRTIO_INPUT_H
#include "ark.h"
/* Native virtio 1.0 PCI absolute tablet / multitouch input. No Linux code.
 * Poll from the main loop, never concurrently from an interrupt handler.
 * EV_POINTER is an absolute MOUSE: dx/dy in [0,32767], buttons bit 0/1/2
 * means left/right/middle. It must not activate touch UI or a touch halo.
 * EV_TOUCH is actual touch: same absolute coordinates, buttons bit 0 is
 * contact down, and key is the tracking ID (0 for a single-touch screen).
 * Up to 16 contacts are tracked; the desktop receives one primary contact.
 * One tablet and one touchscreen may be active together. Mouse motion is
 * suppressed during contact; old mouse packets cannot move the touch cursor.
 */
bool virtio_input_init(void);
bool virtio_input_next_event(Event *event);
const char *virtio_input_name(void);
unsigned virtio_input_contacts(void);
bool virtio_input_pointer_ready(void);
/* Platform bridge used by this driver. Drops stale relative mouse packets
 * when absolute-pointer or touch ownership changes; keyboard stays active. */
void platform_set_relative_pointer_enabled(bool enabled);
#endif
