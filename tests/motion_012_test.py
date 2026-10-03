#!/usr/bin/env python3
"""Bounded native-QEMU motion capture, with measured rather than assumed FPS.

The guest runs under the VM helper's TCG CPU and native virtual display driver.
MP4 is a presentation of timestamped screen samples, not proof of host-GPU use.
Input and screenshots deliberately bypass VM.key/touch/screen's fixed sleeps.
"""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time

from PIL import Image, ImageChops

from vm import ROOT, VM


def mouse(vm, x, y, down=None, width=1280, height=800):
    """Submit one absolute-pointer report without delaying the capture clock."""
    x, y = max(0, min(width-1, x)), max(0, min(height-1, y))
    events = [
        {'type': 'abs', 'data': {'axis': 'x', 'value': (x*32767+width-2)//(width-1)}},
        {'type': 'abs', 'data': {'axis': 'y', 'value': (y*32767+height-2)//(height-1)}},
    ]
    if down is not None:
        events.append({'type': 'btn', 'data': {'button': 'left', 'down': down}})
    vm.q('input-send-event', {'events': events})


def key(vm, qcode):
    vm.q('human-monitor-command', {'command-line': 'sendkey '+qcode+' 25'})


def click_actions(vm, x, y, at=0.1, label='click'):
    return [(at, label+' down', lambda: mouse(vm, x, y, True)),
            (at+0.05, label+' up', lambda: mouse(vm, x, y, False))]


def percentile(values, fraction):
    return sorted(values)[min(len(values)-1, int((len(values)-1)*fraction))]


class TimedCapture:
    """Capture to bounded files on absolute monotonic deadlines.

    Processing, PNG compression, hashing and encoding happen after collection.
    Late deadlines are skipped instead of producing an artificial catch-up burst.
    The CSV retains planned and actual times and every sampled frame's hash.
    """
    def __init__(self, vm, fps=30, video=True):
        if not 1 <= fps <= 60:
            raise ValueError('Capture FPS must be between 1 and 60.')
        self.vm, self.fps, self.video = vm, fps, video

    def run(self, name, seconds=2.0, actions=(), roi=(100, 65, 1100, 690), triggers=()):
        if not 0.2 <= seconds <= 12:
            raise ValueError('Each capture must be bounded to 0.2–12 seconds.')
        if Path(name).name != name:
            raise ValueError('Capture name must be a single directory name.')
        actions = sorted(actions, key=lambda action: action[0])
        if any(not 0 <= action[0] < seconds for action in actions):
            raise ValueError('All actions must occur inside the capture interval.')
        out = self.vm.out / name
        out.mkdir(parents=True, exist_ok=True)
        checkpoint = len(self.vm.log.read_text())
        started_ns = time.monotonic_ns()
        start = time.monotonic()
        index = action_index = 0
        frames, action_log = [], []
        conditional = [{'label': label, 'predicate': predicate, 'delay': delay,
                        'callback': callback, 'due': None, 'done': False}
                       for label, predicate, delay, callback in triggers]
        while True:
            for item in conditional:
                if item['due'] is None and item['predicate']():
                    item['due'] = time.monotonic()-start+item['delay']
            deadline = index/self.fps
            if deadline >= seconds:
                break
            next_action = actions[action_index][0] if action_index < len(actions) else math.inf
            next_trigger = min((item['due'] for item in conditional
                                if item['due'] is not None and not item['done']), default=math.inf)
            target = min(deadline, next_action, next_trigger)
            remaining = start+target-time.monotonic()
            if remaining > 0:
                time.sleep(remaining)
            while action_index < len(actions) and actions[action_index][0] <= time.monotonic()-start:
                scheduled, label, callback = actions[action_index]
                before = time.monotonic()-start
                callback()
                action_log.append({'label': label, 'scheduled_s': scheduled,
                                   'started_s': before, 'finished_s': time.monotonic()-start})
                action_index += 1
            for item in conditional:
                if not item['done'] and item['due'] is not None and item['due'] <= time.monotonic()-start:
                    before = time.monotonic()-start
                    item['callback']()
                    action_log.append({'label': item['label'], 'scheduled_s': item['due'],
                                       'started_s': before, 'finished_s': time.monotonic()-start})
                    item['done'] = True
            if time.monotonic() < start+deadline:
                continue
            if time.monotonic()-start >= seconds:
                break
            path = out / ('frame-%04d.ppm' % len(frames))
            before = time.monotonic()-start
            self.vm.q('screendump', {'filename': str(path)})
            after = time.monotonic()-start
            frames.append({'sample': len(frames), 'deadline_s': deadline,
                           'started_s': before, 'finished_s': after,
                           'capture_ms': (after-before)*1000,
                           'lateness_ms': max(0, before-deadline)*1000,
                           'path': path})
            # Do not replay deadlines missed while QMP/display was busy.
            index = max(index+1, int((time.monotonic()-start)*self.fps)+1)
        serial = self.vm.log.read_text()[checkpoint:]
        (out/'serial.log').write_text(serial)
        (out/'actions.json').write_text(json.dumps(action_log, indent=2)+'\n')
        assert len(frames) >= 2, 'Too few samples; QMP/display may be blocked.'
        previous = None
        for frame in frames:
            image = Image.open(frame['path']).convert('RGB')
            cropped = image.crop(roi)
            frame['sha256'] = hashlib.sha256(image.tobytes()).hexdigest()
            frame['roi_sha256'] = hashlib.sha256(cropped.tobytes()).hexdigest()
            frame['roi_changed_pixels'] = 0
            frame['roi_change_box'] = ''
            if previous is not None:
                difference = ImageChops.difference(previous, cropped)
                channels = difference.split()
                mask = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
                frame['roi_changed_pixels'] = cropped.width*cropped.height-mask.histogram()[0]
                frame['roi_change_box'] = json.dumps(mask.getbbox())
            previous = cropped
            png = frame['path'].with_suffix('.png')
            image.save(png, compress_level=3)
            frame['path'].unlink()
            frame['path'] = str(png)
        with (out/'frames.csv').open('w', newline='') as handle:
            writer = csv.DictWriter(handle, fieldnames=frames[0].keys())
            writer.writeheader()
            writer.writerows(frames)
        intervals = [b['started_s']-a['started_s'] for a, b in zip(frames, frames[1:])]
        changes = [b['roi_sha256'] != a['roi_sha256'] for a, b in zip(frames, frames[1:])]
        holds, held_since = [], frames[0]['started_s']
        for before, after in zip(frames, frames[1:]):
            if before['roi_sha256'] != after['roi_sha256']:
                holds.append(after['started_s']-held_since)
                held_since = after['started_s']
        holds.append(frames[-1]['finished_s']-held_since)
        span = frames[-1]['started_s']-frames[0]['started_s']
        change_times = [after['started_s'] for before, after in zip(frames, frames[1:])
                        if before['roi_sha256'] != after['roi_sha256']]
        change_intervals = [b-a for a, b in zip(change_times, change_times[1:])]
        summary = {
            'name': name, 'sampling_clock': 'host monotonic', 'started_monotonic_ns': started_ns,
            'target_fps': self.fps, 'requested_seconds': seconds, 'roi': roi,
            'sample_count': len(frames), 'actual_sample_fps': (len(frames)-1)/span,
            'unique_rgb_hashes': len({f['sha256'] for f in frames}),
            'unique_roi_hashes': len({f['roi_sha256'] for f in frames}),
            'roi_changed_samples_per_second': sum(changes)/span,
            'max_unchanged_roi_ms': max(holds)*1000,
            'unchanged_interval_note': 'Includes intentional settled/idle parts of the clip.',
            'first_roi_change_s': change_times[0] if change_times else None,
            'last_roi_change_s': change_times[-1] if change_times else None,
            'first_action_to_first_roi_change_ms': (change_times[0]-action_log[0]['finished_s'])*1000
                if change_times and action_log else None,
            'max_gap_between_roi_changes_ms': max(change_intervals)*1000 if change_intervals else None,
            'between_changes_rate_hz': len(change_intervals)/(change_times[-1]-change_times[0]) if change_intervals else None,
            'capture_p50_ms': statistics.median(f['capture_ms'] for f in frames),
            'capture_p95_ms': percentile([f['capture_ms'] for f in frames], .95),
            'sample_interval_p95_ms': percentile(intervals, .95)*1000,
            'max_sample_gap_ms': max(intervals)*1000,
            'actions_completed': len(action_log),
            'metric_caveat': 'Observed screen samples, including possible partial uploads; not guest scene FPS or host-GPU performance.',
        }
        assert action_index == len(actions), 'Capture ended before all scheduled actions were sent.'
        assert all(item['done'] for item in conditional), 'A conditional action never became ready.'
        if self.video:
            self.encode(out, frames, summary)
        (out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
        print(json.dumps(summary), flush=True)
        return {'directory': out, 'summary': summary, 'frames': frames,
                'actions': action_log, 'serial': serial}

    @staticmethod
    def encode(out, frames, summary):
        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg:
            summary['video'] = 'ffmpeg unavailable; lossless PNG and timing CSV retained.'
            return
        lines = ['ffconcat version 1.0']
        for index, frame in enumerate(frames):
            # Generated simple filenames avoid concat path quoting ambiguities.
            lines.extend(["file '"+Path(frame['path']).name+"'", 'option framerate 1000'])
            tail = statistics.median(b['started_s']-a['started_s'] for a, b in zip(frames, frames[1:]))
            next_time = frames[index+1]['started_s'] if index+1 < len(frames) else frame['finished_s']+tail
            lines.append('duration %.9f' % (next_time-frame['started_s']))
        lines.extend(["file '"+Path(frames[-1]['path']).name+"'", 'option framerate 1000'])
        manifest = out/'timeline.ffconcat'
        manifest.write_text('\n'.join(lines)+'\n')
        command = [ffmpeg, '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', str(manifest),
                   '-fps_mode', 'vfr', '-c:v', 'libx264', '-crf', '19', '-pix_fmt', 'yuv420p',
                   '-video_track_timescale', '90000', str(out/'capture.mp4')]
        try:
            completed = subprocess.run(command, capture_output=True, text=True, timeout=45)
            summary['video'] = 'capture.mp4' if completed.returncode == 0 else 'encode failed; PNG/CSV retained'
            if completed.returncode:
                (out/'ffmpeg.log').write_text(completed.stderr)
        except subprocess.TimeoutExpired:
            summary['video'] = 'encode timeout; PNG/CSV retained'


def warmup(vm, gpu):
    vm.enroll_test_user()
    # Headless VMware must receive a real display refresh to commit its mode.
    vm.q('screendump', {'filename': str(vm.out/'warmup.ppm')})
    mouse(vm, 1170, 90)
    if gpu == 'vmware':
        vm.wait('[gpu] SVGA II FIFO fill/copy verified', timeout=12)
    time.sleep(4.2)  # Allow the initial four-second welcome notice to expire.


def motion_events(text):
    result = []
    for match in re.finditer(r'\[motion\] (begin|reverse|end) ([^\r\n]+)', text):
        fields = dict(re.findall(r'([a-z][a-z0-9_]*)=([^ ]+)', match.group(2)))
        result.append({'event': match.group(1), **{
            key: int(value) if value.isdigit() else value for key, value in fields.items()}})
    return result


def require_motion(capture, kind, target, event='begin', minimum_frames=3):
    events = motion_events(capture['serial'])
    starts = [item for item in events if item['kind'] == kind and item['event'] == event and item.get('to') == target]
    ends = [item for item in events if item['kind'] == kind and item['event'] == 'end' and item.get('value') == target]
    assert starts and ends, (capture['directory'].name, kind, target, events)
    assert ends[-1]['frames'] >= minimum_frames, ('Transition skipped intermediate guest frames', ends[-1])
    assert capture['summary']['unique_roi_hashes'] >= 3, 'No visible intermediate animation was captured.'
    return starts[-1], ends[-1]


def frame_image(capture, index=-1):
    return Image.open(capture['frames'][index]['path']).convert('RGB')


def file_icon_visible(image):
    crop = image.crop((136, 104, 165, 134))
    return sum((lambda rgb: rgb[0] < 120 and rgb[1] > 100 and rgb[2] > 170)(crop.getpixel((x, y)))
               for y in range(crop.height) for x in range(crop.width)) > 80


def assert_same_crop(first, second, rectangle):
    # Live material changes with the actual wallpaper. Compare strong foreground
    # ink topology and final geometry instead of demanding a frozen backdrop.
    def ink(im):
        mask = Image.new('1', im.size)
        mask.putdata([r < 70 and g < 95 and b < 130 for r,g,b in im.convert('RGB').getdata()])
        return mask
    a,b=ink(first.crop(rectangle)),ink(second.crop(rectangle))
    changed=sum(bool(pixel) for pixel in ImageChops.difference(a,b).getdata())
    assert changed <= 64, ('Restored foreground changed positions',changed)


def window_midflight_reverse(vm, capture, name='04b-window-midflight-reverse'):
    checkpoint = len(vm.log.read_text())
    restoring = [False]
    def restore():
        mouse(vm, 127, 746, True)
        restoring[0] = True
    result = capture.run(name, 2.2, click_actions(vm, 920, 119),
        roi=(130, 140, 1040, 700), triggers=[
            ('restore during minimize',
             lambda: '[motion] begin kind=window' in vm.log.read_text()[checkpoint:],
             .075, restore),
            ('release and leave Dock', lambda: restoring[0], .04,
             lambda: mouse(vm, 1160, 90, False))])
    reverse, _ = require_motion(result, 'window', 65536, event='reverse', minimum_frames=2)
    assert 0 < reverse['from'] < 65536, ('Window reversal jumped to an endpoint', reverse)
    assert file_icon_visible(frame_image(result))
    assert_same_crop(frame_image(result, 0), frame_image(result), (140, 150, 1030, 630))
    print('PASS: window minimize reverses continuously into restore.', flush=True)
    return result


def reference_sequence(vm, capture):
    results = []
    def record(*args, **kwargs):
        result = capture.run(*args, **kwargs)
        results.append(result)
        return result

    minimized = record('01-window-minimize', 2.4, click_actions(vm, 920, 119),
                       roi=(130, 140, 1040, 700))
    require_motion(minimized, 'window', 0)
    assert file_icon_visible(frame_image(minimized, 0))
    assert not file_icon_visible(frame_image(minimized))

    preview = record('02-dock-preview', 1.7,
                     [(0.1, 'hover Files Dock', lambda: mouse(vm, 127, 746))],
                     roi=(80, 518, 326, 714))
    require_motion(preview, 'preview', 65536)
    assert preview['frames'][0]['roi_sha256'] != preview['frames'][-1]['roi_sha256']

    dismiss = record('03-preview-dismiss', 1.4,
                     [(0.1, 'leave preview', lambda: mouse(vm, 1160, 90))],
                     roi=(80, 518, 326, 714))
    require_motion(dismiss, 'preview', 0, event='reverse')

    restored = record('04-window-restore', 2.4,
                      click_actions(vm, 127, 746)+[(0.28, 'leave Dock', lambda: mouse(vm, 1160, 90))],
                      roi=(130, 140, 1040, 700))
    require_motion(restored, 'window', 65536)
    assert file_icon_visible(frame_image(restored))
    window_reversed = window_midflight_reverse(vm, capture)
    results.append(window_reversed)

    opened = record('05-launcher-open', 2.5,
                    [(0.1, 'F5 launcher open', lambda: key(vm, 'f5'))], roi=(130, 100, 1150, 680))
    require_motion(opened, 'launcher', 65536)
    closed = record('06-launcher-close', 1.7,
                    [(0.1, 'F5 launcher close', lambda: key(vm, 'f5'))], roi=(130, 100, 1150, 680))
    require_motion(closed, 'launcher', 0, event='reverse')
    assert_same_crop(frame_image(opened, 0), frame_image(closed), (140, 150, 1030, 630))

    checkpoint = len(vm.log.read_text())
    reversed_capture = record('07-launcher-midflight-reverse', 2.5,
        [(0.1, 'F5 launcher open', lambda: key(vm, 'f5'))], roi=(130, 100, 1150, 680),
        triggers=[('reverse after native animation begins',
                   lambda: '[motion] begin kind=launcher' in vm.log.read_text()[checkpoint:],
                   .055, lambda: key(vm, 'f5'))])
    reverse, _ = require_motion(reversed_capture, 'launcher', 0, event='reverse', minimum_frames=2)
    assert 0 < reverse['from'] < 65536, ('Reversal restarted at an endpoint', reverse)
    begins = [item for item in motion_events(reversed_capture['serial']) if item['event'] == 'begin' and item['kind'] == 'launcher']
    assert len(begins) == 1 and reverse['start_ms'] > begins[0]['start_ms']
    assert_same_crop(frame_image(reversed_capture, 0), frame_image(reversed_capture), (140, 150, 1030, 630))

    # The launcher is native and opens the real application selected by search.
    ready = record('08-launcher-search-open', 2.2,
                   [(0.1, 'open launcher for search', lambda: key(vm, 'f5'))],
                   roi=(130, 100, 1150, 680))
    require_motion(ready, 'launcher', 65536)
    search_actions = [(0.1+i*.12, 'search '+letter, lambda letter=letter: key(vm, letter))
                      for i, letter in enumerate('calc')]
    search_actions.append((.85, 'launch real Calculator', lambda: key(vm, 'ret')))
    searched = record('09-launcher-search-select', 2.5, search_actions, roi=(400, 90, 900, 680))
    assert '[ui] launcher open Calculator' in searched['serial'], searched['serial']

    aggregate = {'passed': True, 'scenarios': [result['summary'] for result in results],
                 'native_motion_events': motion_events(vm.log.read_text()),
                 'reversal_from_q16': reverse['from'],
                 'window_reversal_from_q16': [event['from'] for event in motion_events(window_reversed['serial'])
                                               if event['event'] == 'reverse' and event['kind'] == 'window'][-1],
                 'reversal_check': 'Native retarget starts at an interior progress value; captured final geometry and foreground return while the live material continues changing.'}
    if capture.video and shutil.which('ffmpeg'):
        movies = [result['directory']/'capture.mp4' for result in results]
        if all(movie.is_file() for movie in movies):
            manifest = vm.out/'sequence.ffconcat'
            manifest.write_text('ffconcat version 1.0\n'+''.join(
                "file '"+movie.parent.name+"/capture.mp4'\n" for movie in movies))
            try:
                encoded = subprocess.run([shutil.which('ffmpeg'), '-v', 'error', '-y',
                    '-f', 'concat', '-safe', '0', '-i', str(manifest), '-c', 'copy',
                    str(vm.out/'reference-sequence.mp4')], capture_output=True, text=True, timeout=30)
                if encoded.returncode == 0:
                    aggregate['sequence_video'] = 'reference-sequence.mp4'
                else:
                    (vm.out/'sequence-ffmpeg.log').write_text(encoded.stderr)
            except subprocess.TimeoutExpired:
                aggregate['sequence_video'] = 'Joining clips timed out; individual captures retained.'
    (vm.out/'result.json').write_text(json.dumps(aggregate, indent=2)+'\n')
    print('PASS: native minimize/restore, Dock preview, launcher transitions, continuous retarget and real app launch.', flush=True)
    return aggregate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', choices=('bios', 'uefi'), default='bios')
    parser.add_argument('--gpu', choices=('vmware', 'std'), default='vmware')
    parser.add_argument('--fps', type=int, default=30)
    parser.add_argument('--no-video', action='store_true')
    parser.add_argument('--iso', type=Path)
    parser.add_argument('--scenario', choices=('idle', 'reference', 'window-reverse'), default='reference')
    args = parser.parse_args()
    if args.iso:
        os.environ['ARKOS_ISO'] = str(args.iso.resolve())
    vm = VM('motion-012-'+args.gpu+'-'+args.firmware, firmware=args.firmware,
            device='virtio-multitouch-pci,virtio-tablet-pci', gpu=args.gpu)
    try:
        iso = Path(os.environ.get('ARKOS_ISO', str(ROOT/'build/arkos-0.12.0.iso')))
        metadata = {'firmware': args.firmware, 'gpu': args.gpu, 'cpu_accelerator': 'tcg',
                    'iso': str(iso), 'iso_sha256': hashlib.sha256(iso.read_bytes()).hexdigest(),
                    'devices': 'virtio-multitouch-pci,virtio-tablet-pci'}
        (vm.out/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
        warmup(vm, args.gpu)
        capture = TimedCapture(vm, args.fps, not args.no_video)
        if args.scenario == 'reference':
            reference_sequence(vm, capture)
        elif args.scenario == 'window-reverse':
            window_midflight_reverse(vm, capture)
        else:
            capture.run('idle', seconds=1)
    finally:
        vm.close()


if __name__ == '__main__':
    main()
