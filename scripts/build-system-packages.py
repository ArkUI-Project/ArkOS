#!/usr/bin/env python3
"""Package every shipped app; font bytes stay shared in the boot container.

Native apps have independent ELF payloads. System tools select an entry in the
trusted desktop engine, whose executable payload is included in each exported
system bundle. Privilege is granted only to manifests compiled into the kernel.
"""
from pathlib import Path
import argparse,hashlib,json,struct,zlib
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output-dir',type=Path,default=Path('build'));a=p.parse_args();root=a.output_dir
report=json.loads((root/'programs.json').read_text());archive=(root/'programs.pack').read_bytes()
images={Path(i['source']).stem:i for i in report['images']}
apps=[('terminal','终端'),('files','文件'),('notes','文本编辑'),('settings','系统设置'),('about','关于 ArkOS'),('calculator','计算器'),('browser','浏览器'),('clock','时钟'),('paint','画板'),('markdown','Markdown'),('tasks','任务管理器'),('capture','屏幕录制'),('installer','安装 ArkOS'),('todo','待办事项'),('timer','计时器'),('wasm','应用运行器'),('packages','包管理器'),('calendar','日历'),('reminders','提醒事项')]
caps={'notes':6,'browser':12,'paint':6,'markdown':6,'todo':6,'timer':36,'wasm':6,'clock':4,'calculator':4,'calendar':4,'reminders':36}
headers=bytearray();packages=[];(root/'packages').mkdir(exist_ok=True)
for desktop,(name,title) in enumerate(apps):
 native=name in caps;image=images[name if native else 'desktop'];payload=archive[image['start']:]
 h=bytearray(256);h[:8]=b'ARKPKG1\0';permissions=caps[name] if native else 31;flags=1 if native else 2|(desktop<<8)
 struct.pack_into('<HH9I',h,8,1,62,256,256+len(payload),1,permissions,0,13,0,len(payload),flags)
 for first,last,value in [(48,80,'ark.'+name),(80,144,title),(144,208,'随 ArkOS 提供的应用')]:
  data=value.encode();assert len(data)<last-first;h[first:first+len(data)]=data
 h[208:240]=hashlib.sha256(payload).digest();struct.pack_into('<I',h,252,zlib.crc32(h[:252]));headers.extend(h)
 data=h+payload;dest=root/'packages'/f'ark.{name}.arkpkg';dest.write_bytes(data)
 packages.append({'id':'ark.'+name,'desktop':desktop,'runtime':'isolated-elf' if native else 'trusted-desktop-entry','bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'payload_sha256':hashlib.sha256(payload).hexdigest()})
(root/'system-package-headers.bin').write_bytes(headers)
with (root/'programs.S').open('a') as f:f.write('\n.section .rodata.system_packages,"a",@progbits\n.balign 16\n.global ark_system_package_headers\nark_system_package_headers:\n.incbin "'+str(root/'system-package-headers.bin')+'"\n.section .note.GNU-stack,"",@progbits\n')
(root/'system-packages.json').write_text(json.dumps({'format':'ArkPkg v1','packages':packages},ensure_ascii=False,indent=2)+'\n')
print(f'Packaged {len(packages)} shipped apps with genuine executable payloads')
