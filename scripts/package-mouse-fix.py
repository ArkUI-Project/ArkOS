#!/usr/bin/env python3
"""Snapshot the native mouse revision and its independently scoped evidence."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.13.0+mouse1'
LABEL = '0.13.0-mouse1'
DIRECTORIES = ('boot', 'kernel', 'include', 'user', 'sdk', 'tests', 'scripts',
               'examples', 'assets', 'arm64', 'runtime', 'third_party', 'docs')


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/release' / LABEL)
    args = parser.parse_args()
    build = ROOT / 'build'
    parent = args.output.resolve()
    stage = parent / ('ArkOS-' + LABEL)
    archive = parent / ('ArkOS-' + LABEL + '-source-and-boot.zip')
    if stage.exists() or archive.exists():
        parser.error('Refusing to overwrite an existing release snapshot.')
    iso = build / 'arkos-0.13.0.iso'
    kernel = build / 'kernel.elf'
    iso_hash, kernel_hash = sha(iso), sha(kernel)
    assert VERSION.encode() in kernel.read_bytes()
    runs = []
    evidence = [build / name for name in ('mouse1-build.log', 'mouse1-host.log',
                                         'mouse1-input-host.log', 'mouse1-bios.log',
                                         'mouse1-uefi.log')]
    for firmware in ('bios', 'uefi'):
        directory = build / ('test-spice-mouse-' + firmware)
        result = json.loads((directory / 'results.json').read_text())
        assert result['result'] == 'PASS'
        assert result['iso_sha256'] == iso_hash and result['kernel_sha256'] == kernel_hash
        runs.append({'name': 'native-serial-' + firmware, 'kind': 'final-production', **result})
        evidence.extend(sorted(p for p in directory.iterdir()
                               if p.suffix in ('.png', '.log', '.json')))
    visible = build / 'test-mouse-input'
    assert sha(visible / 'ArkOS-Mouse-QA.utm/Data/arkos-0.13.0.iso') == iso_hash
    for name in ('visible-serial.log', 'original-status.log'):
        log = (visible / name).read_text()
        assert '[arkos] desktop ready ' + VERSION + ' ring3' in log
        assert '[spice-input] Native absolute mouse channel connected.' in log
        assert '[vgpu] GPU blur/lens draw, pixels and fence/readback verified' in log
    assert '[glass] live GPU blur/refraction/dispersion active' in (visible / 'visible-serial.log').read_text()
    assert 'mouse mode 2' in (visible / 'visible-debug.log').read_text()
    assert 'mouse mode 2' in (visible / 'original-status.log').read_text()
    evidence.extend(visible / name for name in (
        'visible-settings.png', 'visible-before-scroll.png', 'visible-after-scroll.png',
        'visible-reverse-scroll.png', 'visible-serial.log', 'visible-debug.log',
        'visible-config.plist', 'original-mouse-click.png', 'original-login-ready.png',
        'original-status.log'))
    trace = build / 'test-spice-mouse-trace'
    evidence.extend(trace / name for name in ('serial.log', 'debug.log', 'payload.json'))
    packages = sorted((build / 'packages').glob('*.arkpkg'))
    assert len(packages) == 19
    report = {
        'version': VERSION, 'recorded_at': datetime.datetime.now().astimezone().isoformat(),
        'iso_sha256': iso_hash, 'kernel_sha256': kernel_hash,
        'runs': runs,
        'host_checks': {'make-check': 'PASS', 'final-input-host-check': 'PASS'},
        'visible_utm': {
            'kind': 'final-production', 'result': 'PASS',
            'environment': 'UTM 4.7.5 / QEMU 10.0.2 / Apple M4',
            'checks': ['actual SPICE mouse mode 2 and native agent connection',
                       'guest pointer movement and complete Dock Settings click',
                       'application permissions list scrolls in both directions',
                       'native GPU shader self-test and live GPU material remain active'],
            'held_drag': 'CUA sent mask=0 on motion; frontend held drag not verified by this tool',
        },
        'original_vm_update': {
            'result': 'PASS', 'data_disk_unchanged_by_update': True,
            'checks': ['graceful shutdown; UTM exited before updating cached configuration',
                       'boot-media update preserves original data disk bytes',
                       'final revision, GPU self-test and actual mouse mode 2 after restart',
                       'pointer movement and login touch-keyboard open/close respond'],
            'handoff': 'original account retained; empty password field on login screen',
        },
        'diagnostics': [{'path': 'build/test-spice-mouse-trace/payload.json',
                         'kind': 'instrumented candidate; CUA packet diagnosis only'}],
        'limits': ['x86-64, dedicated modern VirtIO-serial, desktop display ID 0, vertical wheel',
                   'BIOS/UEFI held-drag checks use an explicit socket protocol peer over real PCI/DMA',
                   'GPU pixel oracle and frame-rate measurements were not repeated in this revision',
                   'stable 60fps and actual 120Hz acceptance remain open'],
        'system_packages': [{'id': p.stem, 'sha256': sha(p)} for p in packages],
        'sdk_example': {'kind': 'unchanged ABI 13 example',
                        'elf_sha256': sha(build / 'workspace-013.elf'),
                        'package_sha256': sha(build / 'workspace-013.arkpkg')},
        'evidence': [{'path': p.relative_to(ROOT).as_posix(), 'sha256': sha(p)}
                     for p in sorted(set(evidence))],
    }
    report_path = build / 'validation-mouse1.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    parent.mkdir(parents=True, exist_ok=True)
    stage.mkdir()

    def copy(source, target):
        destination = stage / target
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        shutil.copymode(source, destination)

    for directory in DIRECTORIES:
        for source in sorted((ROOT / directory).rglob('*')):
            relative = source.relative_to(ROOT)
            if (not source.is_file() or source.is_symlink() or
                any(p.startswith('.') or p in ('__pycache__', 'build') for p in relative.parts) or
                source.suffix in ('.img', '.iso', '.elf', '.o', '.pyc')):
                continue
            copy(source, relative)
    for source in [ROOT / name for name in ('AGENTS.md', 'LICENSE', 'Makefile', 'README.md')] + list(ROOT.glob('*.mk')):
        copy(source, source.name)
    copy(iso, iso.name)
    copy(kernel, 'kernel.elf')
    copy(build / 'workspace-013.elf', 'examples/packages/workspace.elf')
    copy(build / 'workspace-013.arkpkg', 'examples/packages/workspace-0.13.0.arkpkg')
    for source in packages:
        copy(source, Path('examples/packages/system') / source.name)
    # The release disk is freshly created; enrolled test and user disks stay outside.
    subprocess.run([sys.executable, str(ROOT / 'scripts/create-disk.py'),
                    str(stage / 'arkos-data.img')], check=True)
    verification = Path('docs/verification-mouse1')
    copy(report_path, verification / report_path.name)
    for row in report['evidence']:
        source = ROOT / row['path']
        assert sha(source) == row['sha256']
        copy(source, verification / source.relative_to(build))
    (stage / verification / 'README.md').write_text(
        '# mouse1 验证证据\n\n'
        '最终生产镜像的 BIOS/UEFI 原生输入和可见 UTM 结果按实际范围记录。\n'
        '协议套接字夹具、CUA 的 mask=0 拖动限制及诊断候选均单独注明。\n'
        'JSON 中 build/ 是原开发根；本目录按相同相对路径组织证据。\n'
        '可见配置是独立 QA 夹具的记录；启动脚本为当前位置重新生成配置。\n'
        '完整用户盘和已登录测试盘不在交付盘内，数据盘重新创建。\n'
        '载荷摘要、运行范围和协议限制见本目录 validation-mouse1.json。\n')
    files = sorted(p for p in stage.rglob('*') if p.is_file())
    (stage / 'SHA256SUMS.txt').write_text(''.join(
        sha(p) + '  ' + p.relative_to(stage).as_posix() + '\n' for p in files))
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as package:
        for source in sorted(stage.rglob('*')):
            if source.is_file():
                package.write(source, source.relative_to(parent))
    with zipfile.ZipFile(archive) as package:
        assert package.testzip() is None
    result = {'version': VERSION, 'files': len(files) + 1,
              'iso_sha256': iso_hash, 'kernel_sha256': kernel_hash,
              'archive': str(archive), 'archive_bytes': archive.stat().st_size,
              'archive_sha256': sha(archive)}
    (parent / 'package.json').write_text(json.dumps(result, indent=2) + '\n')
    (parent / 'SHA256SUMS.txt').write_text(iso_hash + '  ' + iso.name + '\n' +
                                         result['archive_sha256'] + '  ' + archive.name + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
