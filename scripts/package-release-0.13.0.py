#!/usr/bin/env python3
"""Snapshot native 0.13 sources, fresh boot media and scoped GPU evidence."""
import argparse,hashlib,json,shutil,subprocess,sys,zipfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
VERSION='0.13.0'
DIRECTORIES=('boot','kernel','include','user','sdk','tests','scripts','examples',
             'assets','arm64','runtime','third_party','docs')

def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'build/release/0.13.0-gpu')
    args=parser.parse_args();build=ROOT/'build';iso=build/f'arkos-{VERSION}.iso'
    report=json.loads((build/f'{VERSION}-final-validation.json').read_text())
    assert report['iso_sha256']==sha(iso) and report['kernel_sha256']==sha(build/'kernel.elf')
    assert report['iso_volume']=='ARKOS0130' and len(report['system_packages'])==19
    required={'gpu-pixels-bios','gpu-pixels-uefi','highres-2560x1440','highres-3840x2160',
              'installer-bios','installer-uefi','visible-utm-boot','interaction-bios','interaction-uefi',
              'lifecycle-bios','lifecycle-uefi','vm-demand-swap-bios','vm-demand-swap-uefi','vm-write-eacces'}
    assert required<={row['name'] for row in report['runs']} and all(row['result']=='PASS' for row in report['runs'])
    for row in report['runs']:
        if row['kind'] in ('final-production','final-production-hard-disk'):
            assert row['iso_sha256']==report['iso_sha256'] and row['kernel_sha256']==report['kernel_sha256']
        if row['kind']=='final-kernel-selected-grub-entry':assert row['kernel_sha256']==report['kernel_sha256']
    final=next(row for row in report['measurements'] if row['name']=='preparation')
    assert final['iso_sha256']==report['iso_sha256'] and final['gpu_glass'] and len(final['records'])==8
    for row in report['evidence']:assert sha(ROOT/row['path'])==row['sha256'],row['path']

    parent=args.output.resolve();stage=parent/f'ArkOS-{VERSION}-dev'
    archive=parent/f'ArkOS-{VERSION}-dev-source-and-boot.zip'
    if stage.exists() or archive.exists():parser.error('Refusing to overwrite an existing release snapshot.')
    parent.mkdir(parents=True,exist_ok=True);stage.mkdir()
    def copy(source,target):
        destination=stage/target;destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(source,destination);shutil.copymode(source,destination)
    for directory in DIRECTORIES:
        for source in sorted((ROOT/directory).rglob('*')):
            relative=source.relative_to(ROOT)
            if (not source.is_file() or source.is_symlink() or
                any(part.startswith('.') or part in ('__pycache__','build') for part in relative.parts) or
                source.suffix in ('.img','.iso','.elf','.o','.pyc')):continue
            copy(source,relative)
    for source in [ROOT/name for name in ('AGENTS.md','LICENSE','Makefile','README.md')]+list(ROOT.glob('*.mk')):
        copy(source,source.name)
    copy(iso,iso.name);copy(build/'kernel.elf','kernel.elf')
    assert sha(build/'workspace-013.elf')==report['sdk_example']['elf_sha256']
    assert sha(build/'workspace-013.arkpkg')==report['sdk_example']['sha256']
    copy(build/'workspace-013.elf','examples/packages/workspace.elf')
    copy(build/'workspace-013.arkpkg','examples/packages/workspace-0.13.0.arkpkg')
    copy(build/'workspace-013-inspect.json','examples/packages/workspace-inspect.json')
    for manifest in report['system_packages']:
        source=build/'packages'/(manifest['id']+'.arkpkg');assert sha(source)==manifest['sha256']
        copy(source,Path('examples/packages/system')/source.name)
    # Only a new empty disk is delivered; never copy an enrolled test/user disk.
    subprocess.run([sys.executable,str(ROOT/'scripts/create-disk.py'),str(stage/'arkos-data.img')],check=True)
    evidence=Path(f'docs/verification-{VERSION}')
    copy(build/f'{VERSION}-final-validation.json',evidence/f'{VERSION}-final-validation.json')
    for row in report['evidence']:
        source=ROOT/row['path'];copy(source,evidence/source.relative_to(build))
    (stage/evidence/'README.md').write_text(
        '# 0.13.0 实际验证证据\n\n'
        '每次运行保留实际载荷，最终 GPU ISO、高分辨率启动项、历史 UI 候选、\n'
        '独立 Ring3/GPU/VM 诊断和主机范围分别列在验证 JSON，\n'
        '初次像素与安装失败日志单独列为失败。\n'
        '本机 UTM/M4 同场景 CPU16.19→最终 GPU24.66fps，稳定60/实际120Hz\n'
        '仍未达到。完整原始时间和 GPU 阶段计数保留；不是 Android 真机\n'
        '逐像素或整桌面 GPU 合成认证。实际源组件、许可证与构建脚本随附。\n'
        'JSON 路径中的 build/ 是原开发证据根；本目录按同一相对路径组织。\n'
        'UTM 配置是启动验证时记录，含原开发路径，仅作为证据；在交付根\n'
        '执行 scripts/run-qemu-gpu.sh 会生成当前位置的新配置与独立空白盘。\n'
        '安装与持久化仅针对项目专用盘，交付数据盘重新生成。\n',encoding='utf-8')
    files=sorted(p for p in stage.rglob('*') if p.is_file())
    (stage/'SHA256SUMS.txt').write_text(''.join(sha(p)+'  '+p.relative_to(stage).as_posix()+'\n' for p in files))
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as package:
        for source in sorted(stage.rglob('*')):
            if source.is_file():package.write(source,source.relative_to(parent))
    with zipfile.ZipFile(archive) as package:assert package.testzip() is None
    for line in (stage/'SHA256SUMS.txt').read_text().splitlines():
        expected,name=line.split('  ',1);assert sha(stage/name)==expected,name
    result={'version':VERSION,'kind':'native GPU development increment; 60/120fps acceptance remains open',
        'files':len(files)+1,'iso_sha256':report['iso_sha256'],'kernel_sha256':report['kernel_sha256'],
        'archive':str(archive),'archive_bytes':archive.stat().st_size,'archive_sha256':sha(archive)}
    (parent/f'ArkOS-{VERSION}-dev-package.json').write_text(json.dumps(result,indent=2)+'\n')
    (parent/f'SHA256SUMS-{VERSION}.txt').write_text(report['iso_sha256']+'  '+iso.name+'\n'+result['archive_sha256']+'  '+archive.name+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
