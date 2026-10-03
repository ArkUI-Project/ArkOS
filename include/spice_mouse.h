#ifndef ARK_SPICE_MOUSE_H
#define ARK_SPICE_MOUSE_H
#include "ark.h"
/* Original, mouse-only SPICE agent on a dedicated modern VirtIO-serial PCI
 * controller (max_ports=2, port 1 named com.redhat.spice.0). No host filesystem,
 * clipboard, command execution or account operations are exposed.
 * Initialize before process page tables, poll through virtio_input only. */
bool spice_mouse_init(unsigned width, unsigned height);
bool spice_mouse_next_event(Event *event);
bool spice_mouse_ready(void);
void spice_mouse_discard_events(void);
#endif
