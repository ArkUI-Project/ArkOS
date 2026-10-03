#!/usr/bin/env python3
"""Real-QEMU mouse/touch regression: absolute tracking, clicks, drag and ghosts.

Defaults to standard VGA so input tests are independent of optional GPU paths.
Pass `vmware` to exercise the same test with that GPU. Each run uses its own
freshly generated data disk in its own test directory.
"""
import sys
import time
import json
from PIL import Image, ImageChops
from fixtures import fresh_data_disk
from vm import ROOT, VM


def mouse(vm, x, y, down=None):
    events = [
        {'type': 'abs', 'data': {'axis': 'x', 'value': (x * 32767 + 1278) // 1279}},
        {'type': 'abs', 'data': {'axis': 'y', 'value': (y * 32767 + 798) // 799}},
    ]
    if down is not None:
        events.append({'type': 'btn', 'data': {'button': 'left', 'down': down}})
    vm.q('input-send-event', {'events': events})
    time.sleep(.15)


def capture(vm, name):
    vm.screen(name)
    image = Image.open(vm.out / (name + '.ppm')).convert('RGB')
    assert image.size == (1280, 800)
    image.save(vm.out / (name + '.png'))
    return image


def color_box(image, roi, color):
    points = [(x, y) for y in range(roi[1], roi[3])
              for x in range(roi[0], roi[2]) if image.getpixel((x, y)) == color]
    assert len(points) > 15, (roi, color, len(points))
    return (min(x for x, _ in points), min(y for _, y in points),
            max(x for x, _ in points), max(y for _, y in points))


def same_background(first, second, roi):
    # The live wallpaper may advect a few shades between captures. A stale
    # arrow or touch halo differs by far more than these background shades.
    difference = ImageChops.difference(first.crop(roi), second.crop(roi))
    return max(high for _, high in difference.getextrema()) <= 8


def run(gpu='std', firmware='bios'):
    name = 'pointer-ui-' + gpu + ('-uefi' if firmware == 'uefi' else '')
    target = ROOT / 'build' / ('test-' + name)
    target.mkdir(parents=True, exist_ok=True)
    disk = target / 'pointer-data.img'
    fresh_data_disk(disk)
    vm = VM(name, disk=disk, firmware=firmware,
            device='virtio-multitouch-pci,virtio-tablet-pci', gpu=gpu)
    try:
        vm.enroll_test_user()
        vm.wait('QEMU Virtio Tablet; absolute mouse pointer')
        vm.wait('QEMU Virtio MultiTouch; multitouch')
        pci = vm.q('query-pci')
        (vm.out / 'pci.json').write_text(json.dumps(pci, indent=2) + '\n')
        if firmware == 'uefi':
            high_bars = [region['address'] for bus in pci for dev in bus['devices']
                         if dev.get('id', {}).get('device') == 0x1052
                         for region in dev['regions']
                         if region.get('mem_type_64') and region['address'] >= 2**32]
            assert len(high_bars) == 2, 'UEFI high BARs must remain assigned, not be relocated.'
            vm.wait('[mmio] Firmware high PCI window mapped')
        mouse(vm, 1100, 80)
        baseline = capture(vm, '01-baseline')
        mouse(vm, 1150, 200)
        first = capture(vm, '02-pointer-first')
        a = color_box(first, (1140, 190, 1190, 255), (23, 35, 55))
        tip_offset = (a[0] - 1150, a[1] - 200)
        mouse(vm, 1170, 225)
        second = capture(vm, '03-pointer-moved')
        b = color_box(second, (1160, 215, 1210, 280), (23, 35, 55))
        assert b[0] - a[0] == 20 and b[1] - a[1] == 25, (a, b)
        assert same_background(baseline, second, (1149, 199, 1169, 231)), 'Old cursor was not fully erased.'
        print('PASS absolute 1:1 cursor displacement and old-cursor restoration', flush=True)

        # Exercise the live-wallpaper upload gap as well as stationary UI
        # damage. The initial welcome notice expires during this interval.
        time.sleep(1.2)
        for i in range(3):
            path = vm.out / ('03-wallpaper-pointer-'+str(i)+'.ppm')
            vm.q('screendump', {'filename': str(path)})
            live = Image.open(path).convert('RGB')
            live.save(path.with_suffix('.png'))
            assert color_box(live, (1160, 215, 1210, 280), (23, 35, 55)) == b, \
                'A wallpaper update erased the stationary arrow.'
            time.sleep(.07)
        print('PASS stationary software pointer survives live-wallpaper uploads', flush=True)

        # File window's colored title icon gives a stable pixel landmark.
        icon_color = (34, 143, 231)
        old_icon = color_box(second, (135, 103, 170, 137), icon_color)
        mouse(vm, 400, 118, True)
        mouse(vm, 480, 158)
        mouse(vm, 480, 158, False)
        dragged = capture(vm, '04-mouse-drag')
        new_icon = color_box(dragged, (215, 143, 250, 177), icon_color)
        assert new_icon[0] - old_icon[0] == 80 and new_icon[1] - old_icon[1] == 40
        mouse(vm, 700, 250)  # Released motion must not keep dragging.
        released = capture(vm, '05-drag-released')
        assert color_box(released, (215, 143, 250, 177), icon_color) == new_icon
        print('PASS native mouse press, window drag and release edge', flush=True)

        # A pressed tablet button still draws the ordinary arrow, not a halo.
        mouse(vm, 1200, 500)
        up = capture(vm, '06-mouse-up')
        mouse(vm, 1200, 500, True)
        down = capture(vm, '07-mouse-down')
        assert same_background(up, down, (1180, 480, 1240, 550)), 'Mouse was rendered as touch.'
        mouse(vm, 1200, 500, False)

        vm.touch(1150, 620)
        touch = capture(vm, '08-touch-owns-cursor')
        mouse(vm, 1200, 200)
        suppressed = capture(vm, '09-mouse-hover-suppressed')
        assert same_background(touch, suppressed, (1125, 595, 1175, 645)), 'Mouse displaced active touch.'
        vm.touch(1150, 620, 'end')
        lifted = capture(vm, '10-touch-released-no-jump')
        tip = color_box(lifted, (1138, 608, 1185, 665), (23, 35, 55))
        assert abs(tip[0] - 1150 - tip_offset[0]) <= 1 and \
               abs(tip[1] - 620 - tip_offset[1]) <= 1, tip
        mouse(vm, 1210, 220)
        resumed = capture(vm, '11-mouse-resumed')
        tip = color_box(resumed, (1200, 210, 1245, 260), (23, 35, 55))
        assert tip[0] == 1210 + tip_offset[0] and tip[1] == 220 + tip_offset[1], tip
        print('PASS separate mouse/touch visuals and no stale pointer jump on touch release', flush=True)

        checkpoint = len(vm.log.read_text())
        mouse(vm, 283, 746, True)
        mouse(vm, 283, 746, False)
        vm.wait('[ui] open Terminal', after=checkpoint)
        capture(vm, '12-mouse-click-terminal')
        print('PASS absolute mouse click opens Terminal; evidence:', vm.out, flush=True)
        mouse(vm, 355, 632)
        stationary = capture(vm, '13-pointer-over-caret')
        # The antialiased edge blends with the blinking caret underneath it.
        # Compare the opaque outline, which must survive every partial upload.
        outline = {(x, y) for y in range(632, 662) for x in range(355, 375)
                   if stationary.getpixel((x, y)) == (23, 35, 55)}
        assert len(outline) > 30
        for i in range(3):
            time.sleep(.3)
            blink = capture(vm, '14-caret-refresh-'+str(i))
            assert all(blink.getpixel(point) == (23, 35, 55) for point in outline), \
                'A caret-only update erased the stationary arrow.'
        print('PASS stationary software pointer survives repeated caret-only updates', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    run(sys.argv[1] if len(sys.argv) > 1 else 'std',
        sys.argv[2] if len(sys.argv) > 2 else 'bios')
