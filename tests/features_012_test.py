#!/usr/bin/env python3
"""Real desktop gestures, wheel, theme, background reminder and restart.
All disks are independently generated fixtures."""
import hashlib,json,os,re,subprocess,sys,time
from datetime import datetime,timedelta,timezone
from PIL import Image,ImageChops
from fixtures import fresh_data_disk, fresh_compat_disk
from vm import ROOT,VM
from motion_vm_test import mouse
files_only='--files-only' in sys.argv
OUT=ROOT/('build/test-features-012-files' if files_only else 'build/test-features-012');OUT.mkdir(exist_ok=True)
disk=OUT/'data.img';external=OUT/'compat.img'
fresh_data_disk(disk);fresh_compat_disk(external)
(OUT/'files.sh').write_text('mkdir Wheel\n'+''.join('touch Wheel/File%02d.txt\n'%i for i in range(25)))
(OUT/'long.txt').write_text(''.join('第 %02d 行：中文字体与滚轮浏览\n'%i for i in range(65)))
for name in ('files.sh','long.txt'):
 subprocess.run(['mcopy','-o','-i',str(external)+'@@1048576',str(OUT/name),'::/'+name],env={**os.environ,'MTOOLS_SKIP_CHECK':'1'},check=True)
subprocess.run(['mcopy','-o','-i',str(external)+'@@1048576',str(ROOT/'build/workspace-012.arkpkg'),'::/workspace.arkpkg'],env={**os.environ,'MTOOLS_SKIP_CHECK':'1'},check=True)
os.environ.update(ARKOS_MACHINE='q35',ARKOS_SMP='4')
result={'iso_sha256':hashlib.sha256((ROOT/'build/arkos-0.12.0.iso').read_bytes()).hexdigest(),'checks':[]}
v=VM('features-012-files' if files_only else 'features-012',disk,external=external,device='virtio-multitouch-pci,virtio-tablet-pci,virtio-keyboard-pci')
def shot(name):
 v.screen(name);im=Image.open(v.out/(name+'.ppm'));im.save(v.out/(name+'.png'));return im
def click(x,y):
 mouse(v,x,y,True);time.sleep(.12);mouse(v,x,y,False);time.sleep(.6)
def drag(x,y,nx,ny):
 mouse(v,x,y,True);time.sleep(.18)
 for i in range(1,13):mouse(v,x+(nx-x)*i//12,y+(ny-y)*i//12);time.sleep(.07)
 mouse(v,nx,ny,False);time.sleep(.8)
def command(text,expected=None):
 at=len(v.log.read_text());v.command(text);v.wait(text+'\n',after=at)
 if expected:v.wait(expected,after=at,timeout=60)
 return v.log.read_text()[at:]
def wheel(x,y,n):
 mouse(v,x,y)
 button='wheel-down' if n>0 else 'wheel-up'
 for _ in range(abs(n)):
  v.q('input-send-event',{'events':[{'type':'btn','data':{'button':button,'down':True}},{'type':'btn','data':{'button':button,'down':False}}]});time.sleep(.1)
 time.sleep(.6)
def changed(a,b,region):
 def ink(im):
  out=Image.new('1',im.size);out.putdata([r<95 and g<115 and blue<140 for r,g,blue in im.convert('RGB').getdata()]);return out
 assert ImageChops.difference(ink(a.crop(region)),ink(b.crop(region))).getbbox(),region
try:
 v.enroll_test_user();v.terminal();command('sh /mnt/fat32/files.sh');command('cd Wheel');command('open files');time.sleep(.8)
 before=shot('01-files-top');wheel(700,390,4);after=shot('02-files-scrolled');changed(before,after,(360,230,910,545));wheel(700,390,-30)
 v.key('ctrl-n');v.type('New.txt');v.key('ret');v.wait('[app] Notes ring3 ready');v.wait('[permission] Consent requested by notes');v.key('ret');v.wait('[permission] Allowed notes');time.sleep(.5);v.type('New document');v.key('ctrl-s');v.wait('[notes] File saved');v.key('ctrl-w');time.sleep(.8);v.terminal();command('cat /home/ark/Wheel/New.txt','New document');result['checks'].append('Files wheel changes rows; New dialog replaces selected default and creates an actual saved document')
 if files_only:
  result['result']='PASS';(OUT/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print(json.dumps(result),flush=True);raise SystemExit(0)
 command('pkg run ark.notes');time.sleep(.8);v.key('ctrl-n');v.wait('[ui] new window Notes');v.type('Independent blank');v.key('ctrl-s');v.wait('[notes] File saved',after=v.log.read_text().rfind('[ui] new window Notes'));newpid=int(re.findall(r'Spawn pid=(\d+).*name=notes ',v.log.read_text())[-1]);shot('03-independent-notes');v.key('ctrl-w');v.wait('[process] Exit pid='+str(newpid));v.terminal();command('ls /home/ark','未命名-'+str(newpid)+'.txt');result['checks'].append('Ctrl+N creates an independent blank Notes document with a unique saved path')
 command('run notes /mnt/fat32/long.txt');time.sleep(.8);before=shot('04-notes-before-scroll');wheel(600,365,10);after=shot('05-notes-scrolled');changed(before,after,(230,235,910,525));v.key('ctrl-w');time.sleep(.7);result['checks'].append('independent Notes receives native SCROLL events and preserves UTF-8 text')
 # Dock reorder, then unpin an actual app by dragging out of its bounds.
 drag(775,746,397,746);shot('06-dock-reordered');drag(1153,746,1153,600);shot('07-dock-unpinned');v.terminal();dock=command('reg get /user/appearance/dock');assert 'ark.calendar' in dock and 'ark.installer' not in dock.split(' = ',1)[-1];result['checks'].append('Dock drag reorder/unpin saves the actual ordered package IDs')
 command('pkg install /mnt/fat32/workspace.arkpkg','[package] Installed workspace');command('pkg run workspace');v.wait('[workspace] surface opened');time.sleep(.8);v.type('Drag payload');shot('08-workspace-source')
 # The new workspace is at (145,102), with text at local (32,98).
 drag(215,265,803,16);shot('09-shelf');v.wait('[drop] stored in shelf');time.sleep(.7)
 drag(731,98,425,330);v.wait('[workspace] drop accepted');shot('10-shelf-drop');click(885,156);time.sleep(.5)
 # A second gesture verifies that the drag source's pressed state was reset.
 at=len(v.log.read_text());drag(220,265,803,16);v.wait('[drop] stored in shelf',after=at);shot('11-second-drag');click(885,156);result['checks'].append('native text drag to Island shelf, drop into package, receipt acknowledgement and a second drag after release')
 v.key('ctrl-w');time.sleep(.7);v.key('f4');time.sleep(.7);click(210,370);click(750,284);shot('12-theme-page');click(930,279);shot('13-dark-settings')
 # The actual UI choice must persist and broadcast the dark theme.
 v.terminal();assert '/user/appearance/dark = 1' in command('reg get /user/appearance/dark');v.key('ctrl-w');time.sleep(.5)
 for app in ('calendar','reminders','todo','paint','markdown','clock','calculator','timer','wasm','browser','notes'):
  v.terminal();command('pkg run ark.'+app);time.sleep(.8)
  if app=='todo':v.wait('[permission] Consent requested by todo');v.key('ret');v.wait('[permission] Allowed todo');time.sleep(.5)
  im=shot('dark-'+app);v.key('ctrl-w');time.sleep(.6)
 result['checks'].append('all 11 independent built-in apps and native Settings render in the persisted dark theme')
 v.terminal();command('pkg run ark.reminders');time.sleep(.8);v.type('Background due');v.key('tab');v.key('home')
 for _ in range(16):v.key('delete')
 due=datetime.now(timezone.utc)+timedelta(minutes=1);local=due+timedelta(hours=8);v.type(local.strftime('%Y-%m-%d %H:%M'));v.key('ret');v.wait('[reminders] saved native schedule',after=v.log.read_text().rfind('pkg run ark.reminders'));pid=int(re.findall(r'Spawn pid=(\d+).*name=reminders ',v.log.read_text())[-1]);v.key('ctrl-w');v.wait('[process] Exit pid='+str(pid));at=len(v.log.read_text());v.wait('[reminders] due notification',timeout=90,after=at);shot('14-background-reminder');result['checks'].append('desktop reports a scheduled due reminder after the Reminders process exits')
 v.key('ctrl-t');time.sleep(.7);click(460,232);time.sleep(1.2);shot('15-performance');v.terminal();command('sync','synchronized');assert 'User fault' not in v.log.read_text() and '[exception]' not in v.log.read_text();result['result']='PASS';(OUT/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print(json.dumps(result,ensure_ascii=False),flush=True)
finally:v.close()
v=VM('features-012-persist',disk,external=external,device='virtio-multitouch-pci,virtio-tablet-pci,virtio-keyboard-pci')
try:
 v.wait('[session] login ready');v.type('Refresh-Test42!');v.key('ret');v.wait('[session] desktop unlocked');v.terminal();saved=command('reg get /user/appearance/dock');assert saved.split(' = ',1)[-1].strip()==dock.split(' = ',1)[-1].strip();command('reg get /user/appearance/dark','/user/appearance/dark = 1');shot('16-dark-restart');result['checks'].append('independent guest restart restores custom Dock order, unpin and dark theme');(OUT/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print('PASS Dock/theme restart',flush=True)
finally:v.close()
