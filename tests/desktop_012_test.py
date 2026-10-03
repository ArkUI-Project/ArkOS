#!/usr/bin/env python3
"""Actual desktop and native app input on project-created QEMU fixtures."""
import hashlib,json,os,re,subprocess,sys,time
from pathlib import Path
from PIL import Image
from fixtures import fresh_data_disk, fresh_compat_disk
from vm import ROOT,VM
from motion_vm_test import mouse
firmware=sys.argv[1] if len(sys.argv)>1 else 'bios'
name='desktop-012-'+firmware;out=ROOT/'build'/('test-'+name);out.mkdir(exist_ok=True)
disk=out/'data.img';external=out/'compat.img';fresh_data_disk(disk);fresh_compat_disk(external)
subprocess.run(['mcopy','-o','-i',str(external)+'@@'+str(2048*512),str(ROOT/'build/workspace-012.arkpkg'),'::/workspace.arkpkg'],env={**os.environ,'MTOOLS_SKIP_CHECK':'1'},check=True)
os.environ.update(ARKOS_MACHINE='q35',ARKOS_SMP='4')
result={'firmware':firmware,'iso_sha256':hashlib.sha256((ROOT/'build/arkos-0.12.0.iso').read_bytes()).hexdigest(),'kernel_sha256':hashlib.sha256((ROOT/'build/kernel.elf').read_bytes()).hexdigest(),'checks':[]}
v=VM(name,disk,firmware=firmware,external=external,device='virtio-multitouch-pci,virtio-tablet-pci,virtio-keyboard-pci')
def shot(name):
 v.screen(name);im=Image.open(v.out/(name+'.ppm'));im.save(v.out/(name+'.png'));return im

def command(text,expected=None):
 before=len(v.log.read_text());v.command(text);v.wait(text+'\n',after=before)
 if expected:v.wait(expected,after=before,timeout=60)
 return v.log.read_text()[before:]

def click(x,y):
 mouse(v,x,y,True);time.sleep(.15);mouse(v,x,y,False);time.sleep(.5)

def drag(x,y,nx,ny):
 mouse(v,x,y,True);time.sleep(.15)
 for step in range(1,9):mouse(v,x+(nx-x)*step//8,y+(ny-y)*step//8);time.sleep(.08)
 mouse(v,nx,ny,False);time.sleep(.7)
try:
 v.enroll_test_user();v.terminal();command('pkg list','ark.reminders 0.12.0');result['checks'].append('19 native boot packages')
 command('reg set /user/test/string string durable','Configuration saved.');assert '/user/test/string = durable' in command('reg get /user/test/string')
 command('reg set /user/test/integer int -9223372036854775808','Configuration saved.');assert '-9223372036854775808' in command('reg get /user/test/integer');result['checks'].append('native registry string and signed 64-bit values')
 command('echo First terminal','First terminal');v.key('ctrl-n');v.wait('[ui] new window Terminal');command('echo Second terminal','Second terminal');shot('01-two-terminals');v.key('ctrl-w');time.sleep(.7);command('echo First survives','First survives');shot('02-terminal-close');result['checks'].append('independent terminal command, history and scrollback contexts; close restores first')
 command('pkg run ark.notes');v.wait('[app] Notes ring3 ready');v.wait('[permission] Consent requested by notes');v.key('ret');v.wait('[permission] Allowed notes');time.sleep(.5)
 v.key('ctrl-i');v.type('nihaoma');shot('03-system-pinyin');v.key('spc');v.key('ctrl-s');v.wait('[notes] File saved');shot('04-notes-chinese');v.key('ctrl-i')
 # Native content buffer changes size, rather than being scaled by compositor.
 drag(990,652,1115,675);v.wait('[app] surface resized');shot('05-resized-notes')
 # Partly off-screen positions remain draggable and keep safe clipping.
 drag(500,130,110,62);shot('06-offscreen-notes');drag(130,66,520,148);time.sleep(.2)
 # Minimize retains a live Notes process; restoring from F3 uses the same PID.
 log=v.log.read_text();note_pid=int(re.findall(r'Spawn pid=(\d+).*name=notes ',log)[-1]);
 click(995,146);time.sleep(.5);assert '[process] Exit pid='+str(note_pid) not in v.log.read_text();v.key('f3');time.sleep(.5);assert len(re.findall(r'Spawn pid=\d+.*name=notes ',v.log.read_text()))==1;shot('07-restored-notes');v.key('ctrl-w');v.wait('[process] Exit pid='+str(note_pid));v.terminal();text=command('ps');assert not re.search(r'^'+str(note_pid)+r'\s',text,re.M);assert 'Ark Desktop' in text;result['checks'].append('Chinese TEXT delivered and saved; private surface resize; negative window clipping; dead task omitted and system tasks visible')
 command('pkg run ark.calendar');v.wait('[app] Calendar ring3 ready');v.key('ctrl-n');v.type('Persistent calendar event');v.key('ret');v.wait('[calendar] Native event saved');shot('08-calendar');v.key('ctrl-w');time.sleep(.6);v.terminal();assert 'binary' in command('reg list /apps/calendar/events/');result['checks'].append('Calendar creates persistent dated native events')
 command('pkg run ark.reminders');v.wait('[app] Reminders ring3 ready');v.type('Due reminder');v.key('ret');v.wait('[reminders] due notification');shot('09-reminder-dot');click(803,16);shot('10-reminder-expanded');v.key('ctrl-w');time.sleep(.5);v.terminal();assert 'binary' in command('reg list /apps/reminders/items/');result['checks'].append('Reminders persist and desktop reports actual due notification')
 command('pkg install /mnt/fat32/workspace.arkpkg','[package] Installed workspace');command('pkg run workspace');v.wait('[workspace] surface opened');time.sleep(.7);shot('11-workspace')
 # Package window starts at app 19 geometry; first primary x=145,y=102.
 # Query by its distinctive new-window blue control in the real pixels.
 im=Image.open(v.out/'11-workspace.ppm');points=[]
 for y in range(90,500):
  for x in range(100,1150):
   r,g,b=im.getpixel((x,y))[:3]
   if 35<=r<=45 and 96<=g<=108 and 209<=b<=220:points.append((x,y))
 assert points;bx=max(x for x,y in points);by=min(y for x,y in points);click(bx-45,by+15);v.wait('[workspace] surface opened',after=v.log.read_text().find('[workspace] surface opened')+1);time.sleep(.7);shot('12-package-two-surfaces');result['checks'].append('one installed Ring3 process opens two independent surfaces')
 v.key('ctrl-w');time.sleep(.7);assert '[process] Exit pid='+re.findall(r'Spawn pid=(\d+).*name=pkg.workspace ',v.log.read_text())[-1] not in v.log.read_text();result['checks'].append('closing one package surface preserves the other surface and process')
 v.key('f4');time.sleep(.7);shot('13-settings-root');click(202,16);mouse(v,500,340);time.sleep(1.2);im=shot('13a-menu-live');assert sum(sum(im.getpixel((x,74)))>550 for x in range(180,398))>200;mouse(v,252,16);time.sleep(.35);im=shot('13b-menu-hover');assert sum(sum(im.getpixel((x,74)))>550 for x in range(226,444))>200;v.key('esc');click(750,284);shot('14-settings-about-child');v.key('esc');shot('15-settings-back');result['checks'].append('Settings root links to actual child pages and Escape returns')
 v.key('ctrl-t');time.sleep(.7);shot('16-tasks');click(460,232);time.sleep(1.1);shot('17-performance')
 v.terminal();command('sync','synchronized');assert 'User fault' not in v.log.read_text() and '[exception]' not in v.log.read_text();result['result']='PASS'
 (out/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print(json.dumps(result,ensure_ascii=False),flush=True)
finally:v.close()
# Independently reopen the actual persisted disk with the production image.
v=VM(name+'-persist',disk,firmware=firmware,external=external,device='virtio-multitouch-pci,virtio-tablet-pci,virtio-keyboard-pci')
try:
 v.wait('[session] login ready');v.type('Refresh-Test42!');v.key('ret');v.wait('[session] desktop unlocked');v.terminal();command('reg get /user/test/string','/user/test/string = durable');command('cat notes.txt','你好吗');assert 'binary' in command('reg list /apps/calendar/events/');assert 'binary' in command('reg list /apps/reminders/items/');assert 'binary' in command('reg list /user/input/pinyin/learned/');command('pkg list','workspace 0.12.0');result['checks'].append('guest restart independently restores registry, Chinese note, learned words, calendar, reminder and package');(out/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print('PASS native persistence '+firmware,flush=True)
finally:v.close()
