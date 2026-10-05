#!/usr/bin/env python3
"""Production kernel at native 1440p/4K, with SDK 2x surface text and real input."""
import hashlib,json,os,shutil,subprocess,sys,time
from PIL import Image
from fixtures import fresh_data_disk
from vm import ROOT,VM
from motion_vm_test import mouse
OUT=ROOT/'build/test-highres-013';OUT.mkdir(exist_ok=True);results=[]
for width,height,firmware in [(2560,1440,'bios'),(3840,2160,'uefi')]:
 name=str(width)+'x'+str(height);tree=OUT/name/'iso';(tree/'boot/grub').mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/'build/kernel.elf',tree/'boot/kernel.elf')
 # Exercise the production entry's gfxpayload selection; only the default differs.
 cfg=(ROOT/'boot/grub.cfg').read_text().replace('set default=0','set default='+('2' if width==2560 else '3')).replace('set timeout=1','set timeout=0');(tree/'boot/grub/grub.cfg').write_text(cfg)
 iso=OUT/(name+'.iso')
 with (OUT/(name+'-build.log')).open('w') as log:subprocess.run(['python3',str(ROOT/'scripts/mkiso.py'),str(tree),str(iso),'ARKOS0130'],check=True,stdout=log,stderr=log)
 os.environ['ARKOS_ISO']=str(iso);os.environ['ARKOS_MEMORY']='1024M';os.environ['ARKOS_GEOMETRY']=name;os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4'
 disk=OUT/(name+'.img');fresh_data_disk(disk);v=VM('highres-013-'+name,disk,firmware=firmware,device='virtio-multitouch-pci,virtio-tablet-pci')
 try:
  v.enroll_test_user();v.terminal();v.command('pkg run ark.todo');v.wait('[app] Todo ring3 ready',60);v.wait('[permission] Consent requested by todo');v.key('ret');v.wait('[permission] Allowed todo');time.sleep(.7)
  v.screen('01-native-2x-todo');im=Image.open(v.out/'01-native-2x-todo.ppm');assert im.size==(width,height);im.save(v.out/'01-native-2x-todo.png')
  # Window starts at (325,246); content is exactly 1600x1000 after the first surface.
  v.tap(325+240,246+54+145);v.type('Native high resolution');v.key('ret');v.wait('[todo] Native file saved');v.screen('02-native-input');Image.open(v.out/'02-native-input.ppm').save(v.out/'02-native-input.png')
  mouse(v,1926,1298,True,width,height);time.sleep(.15);mouse(v,2026,1368,None,width,height);time.sleep(.3);mouse(v,2026,1368,False,width,height);v.wait('[app] surface resized');v.screen('02b-native-resize');Image.open(v.out/'02b-native-resize.ppm').save(v.out/'02b-native-resize.png');v.key('ctrl-w');v.key('f4');time.sleep(.5);v.screen('03-settings');Image.open(v.out/'03-settings.ppm').save(v.out/'03-settings.png');assert f'Framebuffer {width}x{height}x32' in v.log.read_text() and 'User fault' not in v.log.read_text()
  results.append({'geometry':name,'firmware':firmware,'memory_mib':1024,'surface':'1600x1000 / logical 800x500, outline text at 2x','resize':'native RESIZE event and repainted 2x layout','result':'PASS'});print('PASS '+name+' '+firmware,flush=True)
 finally:v.close()
(OUT/'results.json').write_text(json.dumps({'kernel_sha256':hashlib.sha256((ROOT/'build/kernel.elf').read_bytes()).hexdigest(),'results':results},indent=2)+'\n')
