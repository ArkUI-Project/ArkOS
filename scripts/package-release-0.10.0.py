#!/usr/bin/env python3
"""Package the verified 0.10.0 development increment with a fresh blank disk."""
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
VERSION = '0.10.0'
DIRECTORIES = ('boot', 'kernel', 'include', 'user', 'sdk', 'tests', 'scripts',
               'examples', 'assets', 'arm64', 'runtime', 'third_party', 'docs')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/release')
    args = parser.parse_args()
    build = ROOT/'build'
    iso = build/f'arkos-{VERSION}.iso'
    digest = sha(iso)
    report = json.loads((build/'0.10.0-final-validation.json').read_text())
    required = {f'{suite}-{fw}' for suite in ('packages', 'motion', 'pointer', 'installer')
                for fw in ('bios', 'uefi')}
    assert report['iso_sha256'] == digest and report['iso_volume'] == 'ARKOS0100'
    assert {r['name'] for r in report['runs']} == required
    assert all(r['exit_code'] == 0 for r in report['runs'])
    assert report['kernel_sha256'] == sha(build/'kernel.elf')
    assert all(row['kernel_sha256'] == report['kernel_sha256'] for row in report['runs'])
    for fw in ('bios', 'uefi'):
        package = json.loads((build/f'test-package-{fw}/result.json').read_text())
        motion = json.loads((build/f'test-motion-vmware-{fw}/metadata.json').read_text())
        package_run = next(row for row in report['runs'] if row['name'] == f'packages-{fw}')
        assert package['iso_sha256'] == package_run['iso_sha256']
        motion_run = next(row for row in report['runs'] if row['name'] == f'motion-{fw}')
        assert motion['iso_sha256'] == motion_run['iso_sha256']
        assert package['result'] == 'PASS'
        assert json.loads((build/f'test-motion-vmware-{fw}/result.json').read_text())['passed']
    for suite in ('test-process-smp', 'test-process-api'):
        assert all(row['result'] == 'PASS' for row in json.loads((build/suite/'results.json').read_text()))

    parent = args.output.resolve()
    stage = parent/f'ArkOS-{VERSION}-dev'
    archive = parent/f'ArkOS-{VERSION}-dev-source-and-boot.zip'
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
    copy(build/'package-hello.elf', 'examples/packages/hello.elf')
    for version in ('1.0.0', '1.1.0'):
        copy(build/f'hello-{version}.arkpkg', f'examples/packages/hello-{version}.arkpkg')
    subprocess.run([sys.executable, str(ROOT/'scripts/create-disk.py'), str(stage/'arkos-data.img')], check=True)

    evidence = Path('docs/verification-0.10.0')
    for name in ('0.10.0-final-validation.json', '0.10.0-build.log',
                 '0.10.0-gpt-build.log', '0.10.0-runtime-kernel.json',
                 '0.10.0-efi-build.log', '0.10.0-pre-efi-validation.json',
                 '0.10.0-pre-efi-installer-uefi.log',
                 '0.10.0-pre-gpt-validation.json', '0.10.0-pre-gpt-installer-bios.log',
                 '0.10.0-pre-gpt-installer-uefi.log',
                 '0.10.0-host-validation.log', '0.10.0-package-host-validation.log',
                 '0.10.0-blob-host-validation.log', '0.10.0-font-validation.log',
                 '0.10.0-service-vm-validation.log', '0.10.0-package-build.log',
                 '0.10.0-package-inspect.json'):
        copy(build/name, evidence/name)
    for run in report['runs']:
        source = Path(run['log'])
        copy(source, evidence/source.name)
    for suite in ('test-process-smp', 'test-process-api'):
        for name in ('results.json', 'bios.log', 'uefi.log'):
            copy(build/suite/name, evidence/suite/name)
    for fw in ('bios', 'uefi'):
        package = build/f'test-package-{fw}'
        for name in ('result.json', 'serial.log', 'qemu.log', '01-idle-desktop.png',
                     '04-installed-package.png', '08-expanded-island.png', '10-finished-activity.png'):
            copy(package/name, evidence/package.name/name)
        persisted = build/f'test-package-persist-{fw}'
        for name in ('serial.log', '11-persistent-package-app.png'):
            copy(persisted/name, evidence/persisted.name/name)
        scene = package/'07-island-expand'
        for name in ('frames.csv', 'actions.json', 'summary.json', 'capture.mp4'):
            copy(scene/name, evidence/package.name/scene.name/name)
        motion = build/f'test-motion-vmware-{fw}'
        for name in ('metadata.json', 'result.json', 'serial.log', 'qemu.log', 'reference-sequence.mp4'):
            copy(motion/name, evidence/motion.name/name)
        for scene_name in ('01-window-minimize', '04-window-restore',
                           '04b-window-midflight-reverse', '05-launcher-open',
                           '06-launcher-close', '07-launcher-midflight-reverse'):
            for name in ('frames.csv', 'actions.json', 'summary.json'):
                copy(motion/scene_name/name, evidence/motion.name/scene_name/name)
        scene = motion/'04-window-restore'
        rows = list(csv.DictReader((scene/'frames.csv').open(newline='')))
        action = json.loads((scene/'actions.json').read_text())[0]
        sample = min(rows, key=lambda row: abs(float(row['started_s'])-(action['finished_s']+.12)))
        copy(Path(sample['path']), evidence/motion.name/'advanced-window.png')
        pointer = build/('test-pointer-ui-std'+('-uefi' if fw == 'uefi' else ''))
        for name in ('serial.log', 'pci.json', '03-wallpaper-pointer-0.png', '14-caret-refresh-2.png'):
            copy(pointer/name, evidence/pointer.name/name)
        for suite in ('install', 'installed-boot'):
            directory = build/f'test-{suite}-{fw}'
            copy(directory/'serial.log', evidence/directory.name/'serial.log')
        image = build/f'test-installed-boot-{fw}/03-harddisk-boot.png'
        copy(image, evidence/image.parent.name/image.name)
    (stage/evidence/'README.md').write_text(
        '# 0.10.0 精选验证证据\n\n'
        'final-validation 逐项记录 ISO 与内核 SHA-256；安装及独立启动验证最终 EFI/GPT ISO。\n'
        '包、动效、软件指针在此前候选 ISO 验证，三者的 /boot/kernel.elf 与最终镜像逐字节一致。\n'
        'pre-gpt-validation 保留初次安装失败：本机包装器缺少 GPT，已修正并实测安装。\n'
        'pre-efi-validation 保留 EFI 配置路径失败；最终两种固件都完成独立硬盘启动。\n'
        'process-smp/process-api 是独立诊断客体，检查 Ring3、权限和内存保护。\n'
        'host-validation 最后一次包测试曾报告主机模型的指针算式 UB；模型修正后的\n'
        '干净结果在 package-host-validation。其他主机检查与字体结果分别保留原日志。\n'
        '实际载荷、验证范围与限制见本目录 0.10.0-final-validation.json。\n'
        'PNG 来自真实客体帧缓冲，MP4 按 CSV 宿主时间戳编码；不是高刷帧率证明。\n'
        '完整帧序列保留在本机 build；CSV 的 path 记录采样时原始绝对路径。\n'
        'QMP 截图不含 VMware 独立硬件指针平面。所有测试使用临时账号与独立测试盘。\n', encoding='utf-8')
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
    result = {'version': VERSION, 'kind': 'development increment; local native packages',
              'files': len(files)+1, 'iso_sha256': digest, 'archive': str(archive),
              'archive_bytes': archive.stat().st_size, 'archive_sha256': sha(archive)}
    (parent/f'ArkOS-{VERSION}-dev-package.json').write_text(json.dumps(result, indent=2)+'\n')
    (parent/f'SHA256SUMS-{VERSION}.txt').write_text(digest+'  '+iso.name+'\n'+result['archive_sha256']+'  '+archive.name+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
