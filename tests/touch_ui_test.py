#!/usr/bin/env python3
"""Exercise the actual native virtio touch driver and desktop in the release ISO.

Requires QEMU 8.1+ and Pillow. QEMU_BIN, QEMU_DATA and the usual QEMU module/
library environment overrides are supported by tests/vm.py. This tests virtual
multitouch input; it makes no claim about USB or I2C touchscreen support.
"""
from PIL import Image
import time
from vm import VM


def capture(vm, name):
    # A full Settings composition under TCG takes longer than a cursor update;
    # wait for the bounded scene paint before inspecting framebuffer pixels.
    time.sleep(3)
    vm.screen(name)
    path = vm.out / (name + '.ppm')
    image = Image.open(path).convert('RGB')
    assert image.size == (1280, 800), 'This UI scenario expects 1280x800.'
    image.save(vm.out / (name + '.png'))
    return image


def settings_icon_center(image, box):
    """Find ArkOS 0.3's Settings icon, independent of title text and cursor."""
    points = [(x, y) for y in range(box[1], box[3])
              for x in range(box[0], box[2])
              if image.getpixel((x, y)) == (120, 147, 172)]
    assert len(points) > 50, 'Expected foreground Settings window icon.'
    return ((min(x for x, _ in points) + max(x for x, _ in points)) / 2,
            (min(y for _, y in points) + max(y for _, y in points)) / 2)


def english_key(vm, key):
    rows = ['1234567890', 'qwertyuiop', 'asdfghjkl', 'zxcvbnm']
    for row, letters in enumerate(rows):
        if key in letters:
            # Match the screen's public geometry; tap centers, not boundaries.
            key_width = (820 - 55) // 10
            start = 230 + (820 - len(letters) * (key_width + 4)) // 2
            vm.tap(start + letters.index(key) * (key_width + 4) + key_width // 2,
                   530 + 48 + row * 39 + 18)
            return
    raise ValueError(key)


def main():
    vm = VM('touch-ui', device='virtio-multitouch-pci,virtio-tablet-pci')
    try:
        vm.wait('[input] Native virtio PCI: QEMU Virtio MultiTouch')
        checkpoint = len(vm.log.read_text())
        vm.tap(342, 18)  # System menu, using the actual multitouch device.
        vm.wait('[ui] open Settings', after=checkpoint)
        before = capture(vm, '01-settings-touch')
        old = settings_icon_center(before, (229, 79, 260, 112))

        vm.touch(500, 94)
        vm.touch(550, 123, kind='update')
        vm.touch(550, 123, kind='end')
        after = capture(vm, '02-settings-dragged')
        new = settings_icon_center(after, (278, 108, 312, 142))
        assert 48 <= new[0] - old[0] <= 52, (old, new)
        assert 25 <= new[1] - old[1] <= 30, (old, new)
        print('PASS: native touch opens Settings and drags its actual rendered window.', flush=True)

        # Touch the top-bar keyboard control; function key only selects the app.
        vm.tap(1020, 20)
        vm.terminal()
        capture(vm, '03-english-touch-keyboard')
        checkpoint = len(vm.log.read_text())
        for letter in 'echo':
            english_key(vm, letter)
        vm.tap(584, 759)  # Space.
        for letter in 'touch':
            english_key(vm, letter)
        vm.tap(978, 759)  # Return.
        vm.wait('echo touch', after=checkpoint)
        vm.wait('\ntouch\n', after=checkpoint)
        print('PASS: on-screen English keys execute a terminal command.', flush=True)

        checkpoint = len(vm.log.read_text())
        for letter in 'echo':
            english_key(vm, letter)
        vm.tap(584, 759)
        vm.tap(297, 759)  # Switch to Chinese phrase keyboard.
        vm.tap(324, 599)  # 你好.
        capture(vm, '04-chinese-touch-keyboard')
        vm.tap(978, 759)
        vm.wait('echo 你好', after=checkpoint)
        vm.wait('\n你好\n', after=checkpoint)

        # UTF-8 backspace must remove a full glyph, not one byte.
        checkpoint = len(vm.log.read_text())
        vm.tap(297, 759)  # Back to English; keep this a pure touch-input test.
        for letter in 'echo':
            english_key(vm, letter)
        vm.tap(584, 759)
        vm.tap(297, 759)  # Chinese again.
        vm.tap(324, 599)
        vm.tap(869, 759)  # Backspace: 你好 -> 你.
        vm.tap(978, 759)
        vm.wait('echo 你', after=checkpoint)
        vm.wait('\n你\n', after=checkpoint)
        print('PASS: Chinese touch insertion, terminal output and glyph backspace.', flush=True)

        vm.tap(996, 556)  # Keyboard close control.
        capture(vm, '05-touch-results')
        print('Evidence:', vm.out, flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    main()
