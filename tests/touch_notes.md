# Native touch input validation

The guest driver is original ArkOS code in `kernel/virtio_input.c`. It uses the
VIRTIO PCI protocol directly and contains no Linux kernel. Linux-style evdev
event numbers are part of the VIRTIO input wire protocol, not a Linux dependency.

## Scope

- Modern PCI virtio input device `1af4:1052`, split event and status queues.
- QEMU `virtio-multitouch-pci`: track up to 16 contacts by slot and tracking ID.
- QEMU `virtio-tablet-pci`: absolute pointer press, move and release.
- The desktop receives the primary contact as `EV_TOUCH`; an absolute tablet
  mouse emits the distinct `EV_POINTER`. Both axes are scaled from the device's
  reported minimum/maximum into 0–32767.
- For touch, `Event.buttons & 1` is contact down and `Event.key` is the tracking
  ID. For mouse, button bits 0/1/2 are left/right/middle; the ordinary arrow is
  retained and mouse presses do not activate touch-specific UI.
- `virtio_input_contacts()` returns the current number of tracked contacts.
- On primary-finger release, an up event precedes any new primary-finger down
  event. This avoids continuing the original drag at another finger's location.
- DMA rings reside in the kernel's identity-mapped memory. Firmware-assigned
  64-bit PCI BARs can be above 4 GiB: the MMIO mapper installs uncached mappings
  on demand instead of moving device resources. This includes OVMF/QEMU's
  768 GiB virtio BAR placement.
- This is **not a USB HID, I²C HID, or physical touchscreen controller driver**.
  Multi-contact tracking does not itself implement pinch/rotate gestures.
- One tablet and one touchscreen may be active together. The driver initializes
  the tablet last so QEMU routes normal mouse button events to it, regardless of
  PCI order. During an active contact, touchscreen movement owns the cursor.
  Queued mouse motion is discarded at takeover/release to prevent a cursor jump.
- PS/2 keyboard stays active. Relative PS/2 mouse input is suppressed and its old
  packets purged while a native tablet is available or touch owns the cursor.
  Without a tablet, PS/2 remains a 1:1 relative fallback with 200 Hz sampling.

Initialize once after `platform_init`, and poll `virtio_input_next_event()` from
the main event loop. The 100 Hz timer wakes the CPU so an input IRQ is not needed.
The driver is not reentrant; do not also call it from an interrupt handler.

## ArkOS 0.3 mouse regression checks

Run `sh tests/test-input.sh` for the hosted input tests and
`python3 tests/pointer_ui_test.py` for the real ISO's standard-VGA mouse checks.
The latter uses its own copy of `build/arkos-data.img`. A `vmware` argument selects
that GPU for the same UI scenario.

The following checks passed with the native driver in QEMU 8.2.2:

- Simultaneous tablet and multitouch, in both PCI enumeration orders.
- Mouse event identity and all three button bits; exact drag/release ordering.
- PS/2 fallback accumulated +200/-150 movement without sign reversal.
- Mouse/touch ownership, cancellation of a prior mouse drag, discard of old
  hover reports, and resumption only after an already-held mouse button releases.
- The real desktop's absolute cursor moved exactly +20/+25 pixels, fully erased
  its old position, and dragged the Files window exactly +80/+40 pixels.
- Mouse down kept the arrow instead of showing a touch halo; lifting a finger
  did not jump back to old mouse coordinates; a fresh mouse click opened Terminal.

Hosted checks additionally cover 9-bit packet signs, overflow saturation,
partial-packet timeout/parity recovery, directional motion coalescing, retention
of press/release locations under a movement backlog, and deferred state recovery
when the bounded queue is saturated by control events. No finite queue can retain
unbounded input while its consumer is stalled; recovery preserves the latest
button state so this extreme case cannot leave a stuck drag.

## Actual checks performed

An isolated diagnostic ISO with the real ArkOS boot code, platform, library and
this driver was booted in QEMU **8.2.2** using its PC BIOS machine and TCG. The
test placed an unrelated `virtio-keyboard-pci` ahead of the target device to
verify capability filtering. The diagnostic ISO was separate from the release.

1. Modern PCI discovery, feature negotiation and both queues initialized.
2. Multitouch coordinates and primary drag reached the guest through DMA.
3. Two contacts were recognized simultaneously (`count=2`).
4. Lifting the primary emitted a release, then selected the remaining contact.
5. Lifting the last contact emitted a release with zero active contacts.
6. 160 additional press/release cycles passed, reusing descriptors across many
   event-ring wraps without losing the final release.
7. A second VM with `virtio-tablet-pci` passed absolute press, drag and release.
8. The driver compiled with the project's freestanding `-Wall -Wextra -Werror`.

Representative diagnostic serial output:

```text
[input] Native virtio PCI: QEMU Virtio MultiTouch; multitouch slots (primary pointer)
[touch] x=8000 y=12000 down=1 id=7 count=1
[touch] x=12000 y=14000 down=1 id=7 count=1
[touch] x=12500 y=14000 down=1 id=7 count=2
[touch] x=12500 y=14000 down=0 id=7 count=1
[touch] x=22000 y=20000 down=1 id=9 count=1
[touch] x=22000 y=20000 down=0 id=9 count=0
```

## QMP test input

Start QEMU with the normal ArkOS ISO arguments plus:

```sh
-device virtio-multitouch-pci -qmp stdio
```

Do not use `-serial stdio` together with `-qmp stdio`; use a serial file instead.
Send `{"execute":"qmp_capabilities"}` first. Then send one complete JSON object
per line. Each `input-send-event` call terminates the group with `SYN_REPORT`.

Finger 7 goes down at `(8000,12000)`:

```json
{"execute":"input-send-event","arguments":{"events":[{"type":"mtt","data":{"type":"begin","slot":0,"tracking-id":7,"axis":"x","value":0}},{"type":"mtt","data":{"type":"data","slot":0,"tracking-id":7,"axis":"x","value":8000}},{"type":"mtt","data":{"type":"data","slot":0,"tracking-id":7,"axis":"y","value":12000}}]}}
```

Move it to `(12000,14000)`:

```json
{"execute":"input-send-event","arguments":{"events":[{"type":"mtt","data":{"type":"update","slot":0,"tracking-id":7,"axis":"x","value":0}},{"type":"mtt","data":{"type":"data","slot":0,"tracking-id":7,"axis":"x","value":12000}},{"type":"mtt","data":{"type":"data","slot":0,"tracking-id":7,"axis":"y","value":14000}}]}}
```

Release it:

```json
{"execute":"input-send-event","arguments":{"events":[{"type":"mtt","data":{"type":"end","slot":0,"tracking-id":-1,"axis":"x","value":0}}]}}
```

**QEMU 8.2 detail:** an `end` event must supply `tracking-id: -1`. The device
forwards the supplied ID; merely changing `type` to `end` does not release a
positive tracking ID. Also, `data` updates the currently selected slot. Precede
a finger's coordinate data with `begin` or `update` for that slot. Repeat with
slot 1 and a different positive ID to test simultaneous contacts.

For `virtio-tablet-pci`, use `abs` events with axis `x`/`y` and `btn` events with
button `left`; values are in 0–32767.

## Primary references

- [OASIS VIRTIO 1.2](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html),
  sections 2.7, 3.1, 4.1 and 5.8.
- [QEMU QMP reference](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html),
  `InputMultiTouchEvent` and `input-send-event`.
- [QEMU upstream multitouch implementation patch](https://lists.gnu.org/archive/html/qemu-devel/2023-05/msg07080.html).
- The QMP event schema was also queried directly from the tested QEMU binary.
