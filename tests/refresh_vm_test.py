#!/usr/bin/env python3
"""Measure completed native scenes without screenshot traffic during motion.

Run baseline and candidate separately with identical QEMU options. Guest frame
counts describe compositor submissions, not a physical monitor's refresh rate.
The host clock also exposes slow guest clocks in older builds.
"""
import argparse
import hashlib
import json
import os
import statistics
import subprocess
import time
from pathlib import Path

from PIL import Image
from vm import ROOT, VM
from motion_vm_test import mouse, motion_events


def keyboard(vm, qcode, down):
    vm.q('input-send-event', {'events': [{'type': 'key', 'data': {
        'down': down, 'key': {'type': 'qcode', 'data': qcode}}}]})


def measure(vm, name, kind, begin, release):
    checkpoint = len(vm.log.read_text())
    begin()
    started = None
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        text = vm.log.read_text()[checkpoint:]
        events = motion_events(text[:text.rfind('\n')+1])
        now = time.monotonic()
        if started is None and any(e['event'] in ('begin', 'reverse') and e['kind'] == kind for e in events):
            started = now
            release()
        ends = [e for e in events if e['event'] == 'end' and e['kind'] == kind]
        if started is not None and ends:
            end = ends[-1]
            wall_ms = (now-started)*1000
            result = {'name': name, **end, 'observed_wall_ms': wall_ms,
                      'guest_submissions_per_second': end['frames']*1000/end['elapsed_ms'],
                      'wall_submissions_per_second': max(0,end['frames']-1)*1000/wall_ms}
            print(json.dumps(result), flush=True)
            time.sleep(.35)  # Settled scene, excluded from measurements.
            return result
        time.sleep(.002)
    raise AssertionError(name+' did not complete\n'+vm.log.read_text()[checkpoint:])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso', type=Path, required=True)
    p.add_argument('--tag', required=True)
    p.add_argument('--firmware', choices=('bios','uefi'), default='bios')
    p.add_argument('--cycles', type=int, default=3)
    p.add_argument('--inspect-seconds', type=int, default=0,
                   help='Keep the completed native display open for up to 45 seconds.')
    args = p.parse_args()
    if (not 1 <= args.cycles <= 5 or not 0 <= args.inspect_seconds <= 45
            or Path(args.tag).name != args.tag):
        p.error('Use 1–5 cycles, 0–45 inspection seconds, and a simple tag.')
    iso = args.iso.resolve()
    os.environ['ARKOS_ISO'] = str(iso)
    vm = VM('refresh-'+args.tag, firmware=args.firmware,
            device='virtio-multitouch-pci,virtio-tablet-pci', gpu='vmware')
    try:
        vm.enroll_test_user()
        vm.q('screendump', {'filename':str(vm.out/'desktop.ppm')})
        mouse(vm,1170,90)
        vm.wait('[gpu] SVGA II FIFO fill/copy verified')
        time.sleep(4.2)
        vm.q('screendump', {'filename':str(vm.out/'desktop.ppm')})
        Image.open(vm.out/'desktop.ppm').save(vm.out/'desktop.png')
        results = []
        for cycle in range(args.cycles):
            results.append(measure(vm,'minimize-'+str(cycle),'window',
                lambda:mouse(vm,920,119,True),lambda:mouse(vm,1170,90,False)))
            results.append(measure(vm,'restore-'+str(cycle),'window',
                lambda:keyboard(vm,'f2',True),lambda:keyboard(vm,'f2',False)))
            for action in ('launcher-open','launcher-close'):
                results.append(measure(vm,action+'-'+str(cycle),'launcher',
                    lambda:keyboard(vm,'f5',True),lambda:keyboard(vm,'f5',False)))
        groups = {}
        for action in ('minimize','restore','launcher-open','launcher-close'):
            items = [r for r in results if r['name'].rsplit('-',1)[0] == action]
            groups[action] = {key: statistics.median(r[key] for r in items) for key in
                ('guest_submissions_per_second','wall_submissions_per_second','elapsed_ms','observed_wall_ms')}
        metadata = {'iso':str(iso),'sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),
                    'qemu_machine':os.environ.get('ARKOS_MACHINE','pc'),
                    'cpus':os.environ.get('ARKOS_SMP','1'),'accelerator':'tcg',
                    'resolution':[1280,800],'firmware':args.firmware,'gpu':'vmware',
                    'display':os.environ.get('ARKOS_DISPLAY','none'),
                    'qemu_version':subprocess.check_output([
                        os.environ.get('QEMU_BIN','qemu-system-x86_64'), '--version'],
                        text=True).splitlines()[0],
                    'clock':'host monotonic; guest clock as logged',
                    'sampling':'serial only during transitions, 2 ms observation interval',
                    'limits':'Submissions are not display scanout FPS; host observation includes serial polling uncertainty.',
                    'median':groups,'scenes':results}
        (vm.out/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
        print(json.dumps(groups,indent=2), flush=True)
        if args.inspect_seconds:
            vm.key('f1')
            mouse(vm,355,632)
            print('Native window ready for inspection.', flush=True)
            time.sleep(args.inspect_seconds)
    finally:
        vm.close()


if __name__ == '__main__':
    main()
