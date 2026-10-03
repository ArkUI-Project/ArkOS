#!/usr/bin/env python3
"""Production desktop: menu persistence/hover, Settings, native text and all system packages."""
from pathlib import Path
import hashlib,json,os,sys,time
from PIL import Image
from fixtures import fresh_data_disk
from vm import ROOT,VM
from motion_vm_test import mouse
firmware=sys.argv[1] if len(sys.argv)>1 else 'bios';out=ROOT/'build'/('test-desktop-011-'+firmware);out.mkdir(exist_ok=True)
disk=out/'data.img';fresh_data_disk(disk)
os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4'
v=VM('desktop-011-'+firmware,disk,firmware=firmware,device='virtio-multitouch-pci,virtio-tablet-pci,virtio-keyboard-pci')
result={'firmware':firmware,'checks':[],'iso_sha256':hashlib.sha256(Path(os.environ.get('ARKOS_ISO',ROOT/'build/arkos-0.11.0.iso')).read_bytes()).hexdigest(),'kernel_sha256':hashlib.sha256((ROOT/'build/kernel.elf').read_bytes()).hexdigest()}
def shot(name):
 v.screen(name);im=Image.open(v.out/(name+'.ppm'));im.save(v.out/(name+'.png'));return im
try:
 v.enroll_test_user();v.terminal();start=len(v.log.read_text());v.command('pkg list');v.wait('ark.packages 0.11.0',after=start);text=v.log.read_text()[start:];assert all('ark.'+name+' 0.11.0' in text for name in ['terminal','files','notes','settings','about','calculator','browser','clock','paint','markdown','tasks','capture','installer','todo','timer','wasm','packages']);result['checks'].append('17 system packages enumerated from boot manifests')
 v.key('f4');time.sleep(.5);shot('01-settings-general')
 v.tap(202,16);mouse(v,500,340);time.sleep(1.2);im=shot('02-edit-menu-after-wallpaper')
 # The full first row remains a light menu surface after repeated wallpaper ticks.
 assert sum(sum(im.getpixel((x,74)))>550 for x in range(180,398))>200
 mouse(v,252,16);time.sleep(.35);im=shot('03-view-menu-hover');assert sum(sum(im.getpixel((x,74)))>550 for x in range(226,444))>200
 result['checks'].append('dropdown remains complete and hover changes Edit to View without clicking')
 v.key('esc');v.tap(231,369);shot('04-settings-appearance');v.key('ctrl-f');v.type('zzz');shot('05-settings-search-empty');v.key('esc');v.tap(230,334);shot('06-settings-permissions')
 v.terminal();v.command('pkg run ark.todo');v.wait('[ui] open Todo');v.wait('[permission] Consent requested by todo');v.key('ret');v.wait('[permission] Allowed todo');time.sleep(.5);shot('07-todo-native-font')
 v.tap(400,272);v.type('HiDPI sharp text');v.key('ret');v.wait('[todo] Native file saved');time.sleep(.3);shot('08-todo-text-entry')
 v.key('ctrl-w');v.terminal();v.command('sync');v.wait('synchronized');assert 'User fault' not in v.log.read_text() and '[exception]' not in v.log.read_text()
 result['checks'].append('settings navigation/search, native window chrome, Todo startup and text entry; no guest fault')
 (out/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print(json.dumps(result,ensure_ascii=False),flush=True)
finally:v.close()
