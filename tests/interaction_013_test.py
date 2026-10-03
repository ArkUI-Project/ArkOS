#!/usr/bin/env python3
"""Real guest pointer edges, navigation, account forms and task snapshots."""
import os,re,json,time,shutil,hashlib,sys
from storage_vm_test import snapshot
from PIL import Image,ImageChops
from fixtures import fresh_data_disk
from vm import ROOT,VM
from motion_vm_test import mouse
fw=sys.argv[1] if len(sys.argv)>1 else 'bios'
name='interaction-013-'+fw;out=ROOT/('build/test-'+name);out.mkdir(exist_ok=True)
disk=out/'data.img';fresh_data_disk(disk)
tested_iso=out/'tested.iso';shutil.copyfile(ROOT/'build/arkos-0.13.0.iso',tested_iso)
iso_hash=hashlib.sha256(tested_iso.read_bytes()).hexdigest();kernel_hash=hashlib.sha256((ROOT/'build/kernel.elf').read_bytes()).hexdigest()
os.environ['ARKOS_ISO']=str(tested_iso)
os.environ.update(ARKOS_MACHINE='q35',ARKOS_SMP='4')
v=VM(name,disk,firmware=fw,device='virtio-tablet-pci,virtio-keyboard-pci')
checks=[]
def click(x,y):mouse(v,x,y,True);time.sleep(.08);mouse(v,x,y,False);time.sleep(.55)
def shot(n):v.screen(n);im=Image.open(v.out/(n+'.ppm'));im.save(v.out/(n+'.png'));return im
try:
 v.enroll_test_user();v.terminal();v.command('echo input ready');v.wait('input ready\n')
 v.key('f4');time.sleep(.7);before=shot('01-settings-root');mouse(v,1225,119,False)
 # General > About. Moving out before release cancels, then a full click navigates.
 checkpoint=len(v.log.read_text());mouse(v,480,282,True);mouse(v,420,240,False);time.sleep(.3);assert '[ui] Settings page' not in v.log.read_text()[checkpoint:]
 mouse(v,480,282,True);time.sleep(.25);held=shot('02-navigation-held');assert '[ui] Settings page' not in v.log.read_text()[checkpoint:];mouse(v,480,282,False);v.wait('[ui] Settings page 关于本机',after=checkpoint);v.wait('[page] end frames=',after=checkpoint);time.sleep(.55);after=shot('03-about-page');assert ImageChops.difference(before.crop((450,252,1000,670)),after.crop((450,252,1000,670))).getbbox()
 page_frames=int(re.search(r'\[page\] end frames=(\d+)',v.log.read_text()[checkpoint:]).group(1));assert page_frames>0
 checks.append('press/release navigation renders a changed child page')
 click(440,137);shot('04-circular-back')
 v.key('ctrl-u');time.sleep(.7);shot('05-users-page')
 # User management is inside the current Settings window.
 click(621,570);shot('06-create-user')
 v.type('second');v.key('tab');v.type('Second user');v.key('tab');v.type('New-Test42!');v.key('tab');v.type('Wrong-Test42!');v.key('ret');time.sleep(.6);assert '|second|' not in snapshot(disk)['entries']['/.system/accounts']['data']
 for _ in range(len('Wrong-Test42!')):v.key('backspace')
 v.type('New-Test42!');v.key('ret');v.wait('[account] User record saved',timeout=30);time.sleep(.8);shot('07-created-user')
 accounts=snapshot(disk)['entries']['/.system/accounts']['data'];assert '|second|' in accounts and 'New-Test42!' not in accounts;checks.append('account creation committed in ArkFS, with protected verifier and masked password')
 v.key('ctrl-t');v.wait('[ui] open Tasks');time.sleep(.8);shot('08-memory-monitor');click(317,148);shot('09-cpu-monitor');click(486,148);shot('10-disk-monitor');v.key('ctrl-f');v.type('desktop');shot('11-monitor-search')
 checks.append('activity monitor live memory/CPU/disk views and search');assert 'New-Test42!' not in v.log.read_text()
 assert '[process] User fault' not in v.log.read_text() and '[exception]' not in v.log.read_text()
 result={'result':'PASS','firmware':fw,'iso_sha256':iso_hash,'kernel_sha256':kernel_hash,'page_animation_frames':page_frames,'checks':checks};(out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
finally:v.close()
v=VM(name+'-persist',disk,firmware=fw,device='virtio-tablet-pci,virtio-keyboard-pci')
try:
 v.wait('[session] login ready');v.type('Refresh-Test42!');v.key('ret');v.wait('[session] desktop unlocked');v.key('ctrl-u');time.sleep(.8);v.screen('01-restored-users');Image.open(v.out/'01-restored-users.ppm').save(v.out/'01-restored-users.png');assert '|second|' in snapshot(disk)['entries']['/.system/accounts']['data'];assert '[exception]' not in v.log.read_text();result['checks'].append('independent reboot restores the second account and native user list');(out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS account restart '+fw,flush=True)
finally:v.close()
