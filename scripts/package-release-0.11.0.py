#!/usr/bin/env python3
"""Package the 0.11.0 source, final boot image and scoped validation evidence."""
import argparse,hashlib,json,shutil,subprocess,sys,zipfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
VERSION='0.11.0'
DIRECTORIES=('boot','kernel','include','user','sdk','tests','scripts','examples',
             'assets','arm64','runtime','third_party','docs')
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,default=ROOT/'build/release');args=parser.parse_args()
 build=ROOT/'build';iso=build/f'arkos-{VERSION}.iso';report=json.loads((build/f'{VERSION}-final-validation.json').read_text())
 assert report['iso_sha256']==sha(iso) and report['kernel_sha256']==sha(build/'kernel.elf') and report['iso_volume']=='ARKOS0110'
 required={'desktop-bios','desktop-uefi','motion-bios','motion-uefi','surface-final','package-count','tls-bios','tls-uefi','public-https',
           'highres-2560x1440','highres-3840x2160','test-process-smp-bios','test-process-smp-uefi','test-process-api-bios','test-process-api-uefi','installer-bios','installer-uefi'}
 assert required<={row['name'] for row in report['runs']} and all(row['result']=='PASS' for row in report['runs'])
 assert len(report['system_packages'])==17
 for row in report['runs']:
  if row['kind']=='final-production':assert row['iso_sha256']==report['iso_sha256'] and row['kernel_sha256']==report['kernel_sha256']
 for row in report['evidence']:assert sha(ROOT/row['path'])==row['sha256'],row['path']
 parent=args.output.resolve();stage=parent/f'ArkOS-{VERSION}-dev';archive=parent/f'ArkOS-{VERSION}-dev-source-and-boot.zip'
 if stage.exists() or archive.exists():parser.error('Refusing to overwrite an existing release snapshot.')
 parent.mkdir(parents=True,exist_ok=True);stage.mkdir()
 def copy(source,target):
  destination=stage/target;destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,destination);shutil.copymode(source,destination)
 for directory in DIRECTORIES:
  for source in sorted((ROOT/directory).rglob('*')):
   relative=source.relative_to(ROOT)
   if (not source.is_file() or source.is_symlink() or
       any(part.startswith('.') or part in ('__pycache__','build') for part in relative.parts) or
       source.suffix in ('.img','.iso','.elf','.o','.pyc')):continue
   copy(source,relative)
 for source in [ROOT/name for name in ('AGENTS.md','LICENSE','Makefile','README.md')]+list(ROOT.glob('*.mk')):copy(source,source.name)
 copy(iso,iso.name);copy(build/'kernel.elf','kernel.elf')
 copy(build/'package-hello-011.elf','examples/packages/hello.elf');copy(build/'hello-011.arkpkg','examples/packages/hello-1.0.0.arkpkg')
 copy(build/'hello-upgrade-011.arkpkg','examples/packages/hello-1.1.0.arkpkg')
 copy(build/'0.11.0-package-upgrade-inspect.json','examples/packages/hello-1.1.0-inspect.json')
 for manifest in report['system_packages']:
  source=build/'packages'/(manifest['id']+'.arkpkg');assert sha(source)==manifest['sha256'];copy(source,Path('examples/packages/system')/source.name)
 subprocess.run([sys.executable,str(ROOT/'scripts/create-disk.py'),str(stage/'arkos-data.img')],check=True)
 evidence=Path(f'docs/verification-{VERSION}')
 copy(build/f'{VERSION}-final-validation.json',evidence/f'{VERSION}-final-validation.json')
 for row in report['evidence']:
  source=ROOT/row['path'];relative=source.relative_to(build);copy(source,evidence/relative)
 (stage/evidence/'README.md').write_text(
  '# 0.11.0 验证记录\n\n'
  'final-validation 记录交付 ISO/内核及每次运行的实际载荷摘要。最终桌面、\n'
  '高级动效和小 surface 使用交付镜像；高分辨率、安装、公共 HTTPS 使用此前\n'
  '候选，保护、API、数量及证书失败路径使用独立诊断。候选未重标为最终镜像。\n'
  'TLS 诊断使用临时 CA 与 DNS55353；生产浏览器使用公共根及标准 DNS。\n'
  '48 包测试含实际安装、重启恢复与独立磁盘解码，最终桌面也恢复65个应用。\n'
  'host-checks 中 make check 通过后，下一个包目标最初编译失败；host-boundaries\n'
  '中包/存储/账户通过后，旧服务主机模型越界。修正后的服务结果为518项、0失败。\n'
  '原日志保留失败及已通过范围，不能把这些合并日志整体视为成功退出。\n'
  'PNG 来自真实客体；MP4/CSV 以宿主时间采样，只证明动画变化与连续反向。\n'
  '稳定60帧和实际120Hz显示未认证，桌面 chrome 仍为100%尺寸。\n'
  '全部持久化检查使用临时测试账户与项目测试盘。交付数据盘由创建器重新生成。\n'
  '构建与测试脚本随源代码提供，接口、许可及边界见项目文档。\n',encoding='utf-8')
 files=sorted(p for p in stage.rglob('*') if p.is_file())
 (stage/'SHA256SUMS.txt').write_text(''.join(sha(p)+'  '+p.relative_to(stage).as_posix()+'\n' for p in files))
 with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as package:
  for source in sorted(stage.rglob('*')):
   if source.is_file():package.write(source,source.relative_to(parent))
 with zipfile.ZipFile(archive) as package:assert package.testzip() is None
 for line in (stage/'SHA256SUMS.txt').read_text().splitlines():
  expected,name=line.split('  ',1);assert sha(stage/name)==expected,name
 result={'version':VERSION,'kind':'native development increment; scoped validation and documented limits',
  'files':len(files)+1,'iso_sha256':report['iso_sha256'],'kernel_sha256':report['kernel_sha256'],
  'archive':str(archive),'archive_bytes':archive.stat().st_size,'archive_sha256':sha(archive)}
 (parent/f'ArkOS-{VERSION}-dev-package.json').write_text(json.dumps(result,indent=2)+'\n')
 (parent/f'SHA256SUMS-{VERSION}.txt').write_text(report['iso_sha256']+'  '+iso.name+'\n'+result['archive_sha256']+'  '+archive.name+'\n')
 print(json.dumps(result,indent=2))
if __name__=='__main__':main()
