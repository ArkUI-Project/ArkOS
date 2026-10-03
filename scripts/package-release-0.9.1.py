#!/usr/bin/env python3
"""Package the verified 0.9.1 development increment and a fresh blank disk."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.9.1'
DIRECTORIES = ('boot', 'kernel', 'include', 'user', 'sdk', 'tests', 'scripts',
               'examples', 'assets', 'arm64', 'runtime', 'third_party', 'docs')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/release')
    args = parser.parse_args()
    build = ROOT/'build'
    iso = build/('arkos-'+VERSION+'.iso')
    digest = sha(iso)
    report = json.loads((build/'final-quality-validation.json').read_text())
    required = {'gpu-stream', 'pointer-bios', 'pointer-uefi', 'settings-bios',
                'settings-uefi', 'motion-bios', 'motion-uefi', 'protection'}
    assert report['iso_sha256'] == digest
    assert {r['name'] for r in report['runs']} == required
    assert all(r['exit_code'] == 0 for r in report['runs'])
    perf = build/'test-refresh-preserved-quality-final/result.json'
    assert json.loads(perf.read_text())['sha256'] == digest
    for firmware in ('bios', 'uefi'):
        metadata = json.loads((build/('test-motion-vmware-'+firmware)/'metadata.json').read_text())
        assert metadata['iso_sha256'] == digest

    parent = args.output.resolve()
    stage = parent/'ArkOS-0.9.1-dev'
    archive = parent/'ArkOS-0.9.1-dev-source-and-boot.zip'
    if stage.exists() or archive.exists():
        parser.error('Refusing to overwrite an existing development package.')
    parent.mkdir(parents=True, exist_ok=True)
    stage.mkdir()

    def copy(source, target):
        destination = stage/target
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        shutil.copymode(source, destination)

    for directory in DIRECTORIES:
        for source in sorted((ROOT/directory).rglob('*')):
            relative = source.relative_to(ROOT)
            if (not source.is_file() or source.is_symlink() or
                    any(part.startswith('.') or part == '__pycache__' for part in relative.parts) or
                    source.suffix in ('.img', '.iso', '.elf', '.o', '.pyc')):
                continue
            copy(source, relative)
    for source in [ROOT/n for n in ('AGENTS.md', 'LICENSE', 'Makefile', 'README.md')]+list(ROOT.glob('*.mk')):
        copy(source, source.name)
    copy(iso, iso.name)
    copy(build/'kernel.elf', 'kernel.elf')
    copy(build/'parallel.elf', 'examples/parallel.elf')
    subprocess.run([sys.executable, str(ROOT/'scripts/create-disk.py'),
                    str(stage/'arkos-data.img')], check=True)

    evidence = Path('docs/verification-0.9.1')
    for source in [build/'final-quality-validation.json',
                   build/'host-final-quality-validation.log',
                   build/'service-final-quality-validation.log',
                   build/'build-pointer-overlay-final.log',
                   build/'sdk-final-quality-build.log']:
        copy(source, evidence/source.name)
    for run in report['runs']:
        source = Path(run['log'])
        copy(source, evidence/source.name)
    for folder in ('test-process-smp', 'test-process-api', 'test-gpu'):
        for name in ('results.json', 'bios.log', 'uefi.log', 'vmware-bios.log', 'std-bios.log', 'vmware-uefi.log'):
            source = build/folder/name
            if source.is_file():
                copy(source, evidence/folder/name)
    for tag in ('before-preserved-quality', 'preserved-quality-final'):
        source = build/('test-refresh-'+tag)
        for name in ('result.json', 'serial.log', 'qemu.log', 'desktop.png'):
            copy(source/name, evidence/source.name/name)
    for firmware in ('bios', 'uefi'):
        source = build/('test-motion-vmware-'+firmware)
        for name in ('metadata.json', 'result.json', 'serial.log', 'qemu.log'):
            copy(source/name, evidence/source.name/name)
        for scene in ('01-window-minimize', '04-window-restore', '05-launcher-open', '06-launcher-close'):
            for name in ('frames.csv', 'actions.json', 'summary.json'):
                copy(source/scene/name, evidence/source.name/scene/name)
    # Selection is explicit: stale frames left by earlier captures are never
    # admitted by globbing an existing capture directory.
    source = build/'test-motion-vmware-bios/04-window-restore'
    rows = list(csv.DictReader((source/'frames.csv').open(newline='')))
    actions = json.loads((source/'actions.json').read_text())
    target_time = actions[0]['finished_s'] + .12
    row = min(rows, key=lambda item: abs(float(item['started_s'])-target_time))
    copy(Path(row['path']), evidence/'advanced-window.png')
    copy(build/'test-refresh-preserved-quality-final/desktop.png', evidence/'desktop.png')
    for name in ('capture.mp4', 'timeline.ffconcat'):
        path = source/name
        if path.is_file():
            copy(path, evidence/name)
    for firmware, folder in (('bios', 'test-pointer-ui-std'), ('uefi', 'test-pointer-ui-std-uefi')):
        for name in ('03-wallpaper-pointer-0.png', '14-caret-refresh-2.png', 'pci.json'):
            copy(build/folder/name, evidence/('pointer-'+firmware)/name)
        source = build/('test-refresh-settings-'+firmware)
        for name in ('result.json', 'serial.log', 'rate-144.png'):
            copy(source/name, evidence/source.name/name)
    (stage/evidence/'README.md').write_text(
        '# 0.9.1 精选验证证据\n\n'
        '生产 ISO 的 SHA-256 见 final-quality-validation.json；保护/GPU 使用独立诊断镜像。\n'
        '完整高级动效的前后比较为 serial-only 测量，不能当作显示器扫描帧率。\n'
        '截图为真实客体帧缓冲。视频按 CSV 的宿主时间戳编码；精选图片只取当次 CSV 列出的帧。\n'
        '原始 PNG 序列保留在本机 build/。QMP 不包含 VMware 独立硬件指针平面。\n'
        'host-final-quality-validation.log 含 Darwin 工具适配的末尾链接失败；刷新适配后，\n'
        '相同服务测试的成功结果见 service-final-quality-validation.log。其他主机套件已通过。\n'
        '验收范围以本目录 final-quality-validation.json 和各次日志为准。\n', encoding='utf-8')
    files = sorted(p for p in stage.rglob('*') if p.is_file())
    (stage/'SHA256SUMS.txt').write_text(''.join(sha(p)+'  '+p.relative_to(stage).as_posix()+'\n' for p in files))
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as package:
        for source in sorted(stage.rglob('*')):
            if source.is_file():
                package.write(source, source.relative_to(parent))
    with zipfile.ZipFile(archive) as package:
        assert package.testzip() is None
    for line in (stage/'SHA256SUMS.txt').read_text().splitlines():
        expected, name = line.split('  ', 1)
        assert sha(stage/name) == expected, name
    result = {'version': VERSION, 'kind': 'development increment; FPS acceptance incomplete',
              'files': len(files)+1, 'iso_sha256': digest, 'archive': str(archive),
              'archive_bytes': archive.stat().st_size, 'archive_sha256': sha(archive)}
    (parent/'ArkOS-0.9.1-dev-package.json').write_text(json.dumps(result, indent=2)+'\n')
    (parent/'SHA256SUMS.txt').write_text(result['archive_sha256']+'  '+archive.name+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
