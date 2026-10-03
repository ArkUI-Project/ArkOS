#!/usr/bin/env python3
"""Native refresh controls, ArkFS CRC readback and a separate guest reboot."""
import json
import os
import struct
import sys
import time
import zlib
from PIL import Image
from fixtures import fresh_data_disk
from vm import ROOT, VM
from motion_vm_test import mouse


def config_from_disk(path):
    raw = path.read_bytes()
    banks = []
    for lba in (8, 2088):
        header = raw[lba*512:(lba+1)*512]
        if header[:8] != b'ARKBANK1':
            continue
        generation, length, crc, count, committed, tag = struct.unpack_from('<Q5I', header, 8)
        if committed != 1 or tag != 0x41524b31:
            continue
        assert zlib.crc32(header[:508]) == struct.unpack_from('<I', header, 508)[0]
        payload = raw[(lba+1)*512:(lba+1)*512+length]
        assert zlib.crc32(payload) == crc
        banks.append((generation, count, payload))
    _, count, payload = max(banks)
    position = 0
    config = None
    for _ in range(count):
        kind, length = struct.unpack_from('<II', payload, position)
        name = payload[position+8:position+136].split(b'\0', 1)[0].decode()
        value = payload[position+136:position+136+length]
        if kind == 1 and name == '/home/ark/.arkcfg':
            config = value.decode()
        position += 136+length
    assert position == len(payload) and config is not None
    return config


def click(vm, x, y):
    mouse(vm, x, y, True)
    time.sleep(.04)
    mouse(vm, x, y, False)
    mouse(vm, 1180, 80)
    time.sleep(.25)


def main(firmware):
    os.environ['ARKOS_MACHINE'] = 'q35'
    os.environ['ARKOS_SMP'] = '4'
    name = 'refresh-settings-'+firmware
    out = ROOT/'build'/('test-'+name)
    out.mkdir(parents=True, exist_ok=True)
    disk = out/'data.img'
    fresh_data_disk(disk)
    vm = VM(name, disk=disk, firmware=firmware, device='virtio-tablet-pci')
    try:
        vm.enroll_test_user()
        vm.screen('00-desktop')
        vm.key('f4')
        time.sleep(.5)
        click(vm, 300, 268)
        for rate, center in ((60, 501), (120, 652), (144, 805), (240, 960)):
            click(vm, center, 386)
            vm.screen('rate-'+str(rate))
            pixels = Image.open(vm.out/('rate-'+str(rate)+'.ppm')).convert('RGB')
            pixels.save(vm.out/('rate-'+str(rate)+'.png'))
            r, g, b = pixels.getpixel((center-40, 386))
            assert b > r+80 and b > g+40, (rate, (r, g, b))
            for _ in range(2):
                checkpoint = len(vm.log.read_text())
                vm.key('f5')
                vm.wait('target_fps='+str(rate), after=checkpoint)
        click(vm, 805, 386)
        vm.key('ctrl-s')
        time.sleep(.4)
    finally:
        vm.close()
    config = config_from_disk(disk)
    assert 'frame_rate=144\n' in config
    # A new VM must load 144 from the disk instead of the default 120.
    reboot = VM(name+'-reboot', disk=disk, firmware=firmware, device='virtio-tablet-pci')
    try:
        reboot.wait('[session] login ready')
        reboot.type('Refresh-Test42!')
        reboot.key('ret')
        reboot.wait('[session] desktop unlocked')
        reboot.screen('desktop')
        checkpoint = len(reboot.log.read_text())
        reboot.key('f5')
        reboot.wait('target_fps=144', after=checkpoint)
    finally:
        reboot.close()
    (out/'result.json').write_text(json.dumps({'passed': True, 'firmware': firmware,
        'targets': [60, 120, 144, 240], 'reboot_target': 144,
        'verified': ['native selected controls', 'actual scene target',
                     'independent ArkFS header/payload CRCs', 'separate guest reboot']}, indent=2)+'\n')
    print('PASS native 60/120/144/240 controls, CRC-verified preferences and reboot', firmware)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'bios')
