#!/usr/bin/env python3
"""Package native sources, fresh boot/data media and scoped 0.12 evidence."""
import argparse,hashlib,json,shutil,subprocess,sys,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
VERSION='0.12.0'
DIRECTORIES=('boot','kernel','include','user','sdk','tests','scripts','examples',
             'assets','arm64','runtime','third_party','docs')
def sha(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
 return h.hexdigest()
def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--output',type=Path,default=ROOT/'build/release/0.12.0-final')
 args=parser.parse_args();build=ROOT/'build';iso=build/f'arkos-{VERSION}.iso'
 report=json.loads((build/f'{VERSION}-final-validation.json').read_text())
 assert report['iso_sha256']==sha(iso) and report['kernel_sha256']==sha(build/'kernel.elf') and report['iso_volume']=='ARKOS0120'
 required={'drop-failure-bios','drop-failure-uefi','desktop-bios','desktop-uefi',
 'files-wheel-new','feature-gestures-themes-reminder','motion-bios','motion-uefi',
 'highres-2560x1440','highres-3840x2160','installer-bios','installer-uefi',
 'test-process-smp-bios','test-process-smp-uefi','test-process-api-bios','test-process-api-uefi'}
 assert required<={row['name'] for row in report['runs']} and all(row['result']=='PASS' for row in report['runs'])
 assert len(report['system_packages'])==19 and len(report['measurements'])==5
 for row in report['runs']:
  if row['kind']=='final-production':assert row['iso_sha256']==report['iso_sha256'] and row['kernel_sha256']==report['kernel_sha256']
 for row in report['evidence']:assert sha(ROOT/row['path'])==row['sha256'],row['path']
 parent=args.output.resolve();stage=parent/f'ArkOS-{VERSION}-dev';archive=parent/f'ArkOS-{VERSION}-dev-source-and-boot.zip'
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
 for source in [ROOT/name for name in ('AGENTS.md','LICENSE','Makefile','README.md')]+list(ROOT.glob('*.mk')):copy(source,source.name)
 copy(iso,iso.name);copy(build/'kernel.elf','kernel.elf')
 assert sha(build/'workspace-012.elf')==report['sdk_example']['elf_sha256']
 assert sha(build/'workspace-012.arkpkg')==report['sdk_example']['sha256']
 copy(build/'workspace-012.elf','examples/packages/workspace.elf')
 copy(build/'workspace-012.arkpkg','examples/packages/workspace-0.12.0.arkpkg')
 copy(build/'workspace-012-inspect.json','examples/packages/workspace-inspect.json')
 for manifest in report['system_packages']:
  source=build/'packages'/(manifest['id']+'.arkpkg');assert sha(source)==manifest['sha256']
  copy(source,Path('examples/packages/system')/source.name)
 # A new empty image, never a copy of enrolled fixtures or user data.
 subprocess.run([sys.executable,str(ROOT/'scripts/create-disk.py'),str(stage/'arkos-data.img')],check=True)
 evidence=Path(f'docs/verification-{VERSION}')
 copy(build/f'{VERSION}-final-validation.json',evidence/f'{VERSION}-final-validation.json')
 for row in report['evidence']:
  source=ROOT/row['path'];copy(source,evidence/source.relative_to(build))
 (stage/evidence/'README.md').write_text(
 '# 0.12.0 实际验证证据\n\n'
 '报告区分最终交付、生产候选、独立诊断及主机已检查范围。候选保持原始摘要；\n'
 '完整验证范围与资源边界见本目录的验证 JSON 和各次日志。\n'
 '最终交付的 BIOS/UEFI 真实文件拖放与失败恢复通过；桌面/高分辨率、\n'
 '安装/动效及定时提醒分别使用报告中记录的候选。\n'
 '动效保留完整时间 CSV、动作、串口和每帧哈希，以及五个代表 PNG；\n'
 '原采样 PNG 全部保留于开发构建目录，交付仅含所列代表帧。CSV 中开发机绝对\n'
 '路径是原采样位置，阅读时按场景目录/文件名对应。30fps 采样不认证显示帧率。\n'
 '性能初版18.85、优化候选31.25fps；旧版缓存73.35fps，功能改变造成真实退步。\n'
 '稳定60fps与实际120Hz尚未达到。早期部分串口目录复用，完整计时原记录在JSON。\n'
 '混合主机日志保留后续失败，报告只列其实际通过范围与修正后的独立检查。\n'
 '所有安装/持久化测试使用项目创建的专用盘；该目录不含测试盘或真实账户运行数据。\n'
 '启动数据盘重新生成，第三方固定源码/许可和重新构建工具随源码保留。\n',encoding='utf-8')
 files=sorted(p for p in stage.rglob('*') if p.is_file())
 (stage/'SHA256SUMS.txt').write_text(''.join(sha(p)+'  '+p.relative_to(stage).as_posix()+'\n' for p in files))
 with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as package:
  for source in sorted(stage.rglob('*')):
   if source.is_file():package.write(source,source.relative_to(parent))
 with zipfile.ZipFile(archive) as package:assert package.testzip() is None
 for line in (stage/'SHA256SUMS.txt').read_text().splitlines():
  expected,name=line.split('  ',1);assert sha(stage/name)==expected,name
 result={'version':VERSION,'kind':'native development increment with scoped validation; 60/120fps acceptance remains open',
 'files':len(files)+1,'iso_sha256':report['iso_sha256'],'kernel_sha256':report['kernel_sha256'],
 'archive':str(archive),'archive_bytes':archive.stat().st_size,'archive_sha256':sha(archive)}
 (parent/f'ArkOS-{VERSION}-dev-package.json').write_text(json.dumps(result,indent=2)+'\n')
 (parent/f'SHA256SUMS-{VERSION}.txt').write_text(report['iso_sha256']+'  '+iso.name+'\n'+result['archive_sha256']+'  '+archive.name+'\n')
 print(json.dumps(result,indent=2))
if __name__=='__main__':main()
