#!/usr/bin/env python3
"""Stage verified ArkOS 0.9 files; exclude all logged-in test disk images."""
from pathlib import Path
import shutil,json,hashlib,zipfile,argparse
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,default=root/'build/release');args=p.parse_args()
evidence=root/'build/verification-0.9';evidence.mkdir(parents=True,exist_ok=True)
# Invocation logs are copied by the release operator before this packaging step.
for name in ['test-v9-wasm-bios','test-v9-wasm-uefi','test-v9-persist-bios','test-v9-persist-uefi','test-v8-desktop-bios','test-v8-desktop-uefi','test-v8-persist-bios','test-v8-persist-uefi','test-native-runtime','test-v8-capture-uefi','test-process-smp','test-process-api','test-installed-boot-bios','test-installed-boot-uefi','test-panic-bios','test-panic-uefi']:
 source=root/'build'/name
 if not source.is_dir():continue
 target=evidence/name;target.mkdir(exist_ok=True)
 for item in source.iterdir():
  if item.is_file() and item.suffix in ('.log','.json','.png'):shutil.copyfile(item,target/item.name)
shutil.copyfile(root/'build/programs.json',evidence/'programs.json')
(evidence/'README.md').write_text('''# 0.9 验证证据

production ISO: WASM/输入/授权/文件/网络/录屏/安装。专用诊断镜像：Ring3/SMP/API/panic。ARM64 为独立串口开发 ELF。

BIOS 桌面验证九个独立应用同时运行，UEFI 回归原有八应用；两种固件均另有完整 WASM 磁盘及重启验证。未宣称完整 ARM64 桌面、完整 WASI 或全部上游 WASM 规范测试通过。

截图为真实帧缓冲；GIF 为客体原生编码并写入磁盘后原样提取。日志中的模型、设备及能力均限定为测试环境。没有打包已登录测试盘。
''')
parent=args.output.resolve();parent.mkdir(parents=True,exist_ok=True);stage=parent/'ArkOS-0.9.0'
if stage.exists():raise SystemExit('Refusing to overwrite existing staged release: '+str(stage))
def ignore(directory,names):return [n for n in names if n.startswith('.') or n in ['build','deliverables','toolroot','__pycache__','SHA256SUMS.txt'] or Path(n).suffix in ('.img','.qcow2','.utm','.iso','.elf','.o','.pyc','.arkpkg') or n.startswith('verification-')]
shutil.copytree(root,stage,ignore=ignore)
shutil.copytree(evidence,stage/'docs/verification-0.9')
for src,dst in [('build/arkos-0.9.0.iso','arkos-0.9.0.iso'),('build/kernel.elf','kernel.elf'),('build/arm64/arkos-arm64.elf','arkos-arm64.elf'),('build/parallel.elf','examples/parallel.elf'),('build/test-v9-wasm-uefi/01-native-wasm.png','ArkOS-0.9-WASM.png')]:
 target=stage/dst;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/src,target)
media=json.loads((root/'build/test-v8-capture-uefi/results.json').read_text());gif=next(n for n in media['native_files'] if n.endswith('.gif'));shutil.copyfile(root/'build/test-v8-capture-uefi'/gif,stage/'ArkOS-0.9-native-recording.gif')
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
files=sorted(p for p in stage.rglob('*') if p.is_file());(stage/'SHA256SUMS.txt').write_text(''.join(sha(p)+'  '+p.relative_to(stage).as_posix()+'\n' for p in files))
archive=parent/'ArkOS-0.9.0-source-and-boot.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
 for p in sorted(stage.rglob('*')):
  if p.is_file():z.write(p,p.relative_to(parent))
with zipfile.ZipFile(archive) as z:assert z.testzip() is None
for line in (stage/'SHA256SUMS.txt').read_text().splitlines():digest,name=line.split('  ',1);assert sha(stage/name)==digest,name
result={'files':len(files)+1,'archive_bytes':archive.stat().st_size,'archive_sha256':sha(archive),'iso_bytes':(stage/'arkos-0.9.0.iso').stat().st_size,'iso_sha256':sha(stage/'arkos-0.9.0.iso')};(parent/'ArkOS-0.9-package.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
