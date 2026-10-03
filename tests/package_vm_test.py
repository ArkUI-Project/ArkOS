#!/usr/bin/env python3
"""Production ISO: native ArkPkg lifecycle, menu bar and activity interaction."""
from pathlib import Path
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
import zlib
from PIL import Image
from fixtures import fresh_data_disk, fresh_compat_disk
from vm import ROOT, VM
from motion_vm_test import TimedCapture, mouse, click_actions

os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4'
firmware=sys.argv[1] if len(sys.argv)>1 else 'bios'
out=ROOT/'build'/('test-package-'+firmware);out.mkdir(exist_ok=True)
disk=out/'data.img';external=out/'compat.img'
# Fixtures are generated from source for this test run.
fresh_data_disk(disk);fresh_compat_disk(external)
vol=str(external)+'@@'+str(2048*512)
env={**os.environ,'MTOOLS_SKIP_CHECK':'1'}
for name,source in [('hello1.arkpkg','hello-1.0.0.arkpkg'),('hello2.arkpkg','hello-1.1.0.arkpkg')]:
 subprocess.run(['mcopy','-o','-i',vol,str(ROOT/'build'/source),'::/'+name],env=env,check=True)
bad=bytearray((ROOT/'build/hello-1.0.0.arkpkg').read_bytes());bad[-1]^=1
(out/'bad.arkpkg').write_bytes(bad)
subprocess.run(['mcopy','-o','-i',vol,str(out/'bad.arkpkg'),'::/bad.arkpkg'],env=env,check=True)
iso=Path(os.environ.get('ARKOS_ISO',ROOT/'build/arkos-0.10.0.iso'))
result={'firmware':firmware,'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'runtime_host_bridge':False,'checks':[]}
def command(v,text,expected=None):
 before=len(v.log.read_text());v.command(text);v.wait(text+'\n',after=before)
 if expected:v.wait(expected,timeout=60,after=before)
 return v.log.read_text()[before:]
def screen(v,name):
 v.screen(name);image=Image.open(v.out/(name+'.ppm')).convert('RGB');image.save(v.out/(name+'.png'));return image
def installed_from_disk():
 raw=disk.read_bytes();banks=[]
 for at in (8192*512,8200*512):
  b=raw[at:at+4096]
  if b[:8]==b'ARKBLOB1' and zlib.crc32(b[:4092])==struct.unpack_from('<I',b,4092)[0]:banks.append(b)
 b=max(banks,key=lambda p:struct.unpack_from('<Q',p,8)[0])
 for i in range(16):
  record=b[32+i*96:32+(i+1)*96];uid,lba,size,crc=struct.unpack_from('<4I',record)
  if uid==1000 and record[24:88].split(b'\0')[0]==b'@pkg.hello':
   data=raw[lba*512:lba*512+size];assert zlib.crc32(data)==crc and data[:8]==b'ARKINST1'
   slot,grants,length=struct.unpack_from('<3I',data,8);assert length==len(data)-32
   package=data[32:];assert package[:8]==b'ARKPKG1\0' and hashlib.sha256(package[256:]).digest()==package[208:240]
   return {'slot':slot,'grants':grants,'version':list(struct.unpack_from('<3I',package,28)),'sha256':hashlib.sha256(package).hexdigest()}
 raise AssertionError('package not committed')

v=VM('package-'+firmware,disk,firmware=firmware,external=external,device='virtio-multitouch-pci,virtio-tablet-pci,virtio-keyboard-pci')
try:
 v.enroll_test_user();image=screen(v,'01-idle-desktop')
 assert image.getpixel((803,16))!=(158,231,210)
 # Native menu: ArkOS symbol opens the menu, package item opens real manager.
 v.tap(22,16);screen(v,'02-system-menu');v.tap(100,86);v.wait('[ui] open Packages')
 v.tap(450,206);v.type('/mnt/fat32/hello1.arkpkg');v.key('ret');screen(v,'03-inspected-package')
 v.tap(470,315);v.wait('[package] Installed hello',60);screen(v,'04-installed-package')
 result['checks'].append('GUI inspect/install from FAT32; idle capsule absent; native top menu')
 v.terminal();command(v,'pkg list','hello 1.0.0  Hello ArkOS')
 command(v,'pkg grant hello 6','permissions=6 declared=14')
 command(v,'pkg run hello');v.wait('name=pkg.hello',60);time.sleep(.8);screen(v,'05-isolated-package-app')
 log=v.log.read_text();assert re.search(r'Spawn pid=\d+ uid=1000 name=pkg\.hello bytes=\d+ CR3=',log)
 v.key('ctrl-w');time.sleep(.5);v.terminal()
 command(v,'pkg upgrade /mnt/fat32/hello2.arkpkg','[package] Upgraded hello')
 command(v,'pkg list','hello 1.1.0  Hello ArkOS')
 command(v,'pkg upgrade /mnt/fat32/hello1.arkpkg','Upgrade must keep the ID and increase the version')
 command(v,'pkg install /mnt/fat32/bad.arkpkg','Invalid ArkPkg manifest, SHA-256 or isolated ELF')
 command(v,'blobs');log=v.log.read_text();assert '@pkg.hello' not in log
 result['checks'].append('native Ring3 launch; grant preservation on upgrade; downgrade and corrupt package refusal; sealed store')
 # Timer reports native activity through its own Ring3 syscall.
 command(v,'run timer');v.wait('[app] Timer ring3 ready');v.key('spc');time.sleep(1.2)
 image=screen(v,'06-activity-dot');assert image.getpixel((803,16))==(158,231,210)
 cap=TimedCapture(v,fps=30,video=True)
 expand=cap.run('07-island-expand',seconds=1,actions=click_actions(v,803,16),roi=(628,8,978,190))
 screen(v,'08-expanded-island');v.tap(891,155);time.sleep(.4);screen(v,'09-collapsed-island')
 v.key('spc');time.sleep(1.2);image=screen(v,'10-finished-activity');assert image.getpixel((803,16))!=(158,231,210)
 result['checks'].append('activity dot appears only while needed; click expands from dot; collapse and stop hide activity')
 v.terminal();command(v,'sync');result['disk_before_reboot']=installed_from_disk();assert result['disk_before_reboot']['grants']==6 and result['disk_before_reboot']['version']==[1,1,0]
 assert '[exception]' not in v.log.read_text() and 'User fault pid=1' not in v.log.read_text()
finally:v.close()

v=VM('package-persist-'+firmware,disk,firmware=firmware,external=external,device='virtio-multitouch-pci,virtio-tablet-pci')
try:
 v.wait('[session] login ready');v.type('Refresh-Test42!');v.key('ret');v.wait('[session] desktop unlocked',60);v.terminal()
 command(v,'pkg list','hello 1.1.0  Hello ArkOS');command(v,'pkg run hello');v.wait('name=pkg.hello',60);time.sleep(.6)
 screen(v,'11-persistent-package-app');v.key('ctrl-w');time.sleep(.4);v.terminal();command(v,'pkg remove hello','[package] Removed hello');command(v,'pkg list','No installed packages.')
 result['checks'].append('independent disk decode; reboot preserves package/version/grants; native removal')
 assert '[exception]' not in v.log.read_text() and 'User fault pid=1' not in v.log.read_text()
finally:v.close()
result['result']='PASS';(out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print('PASS ArkPkg lifecycle, private permissions, reboot, menu bar and activity',firmware,flush=True)
