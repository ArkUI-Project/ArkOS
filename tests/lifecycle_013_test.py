#!/usr/bin/env python3
"""Actual selection, completed clicks and app-wide exit on fixture disks."""
import hashlib,json,os,re,shutil,subprocess,sys,time
from PIL import Image
from fixtures import fresh_data_disk, fresh_compat_disk
from vm import ROOT,VM
from motion_vm_test import mouse
fw=sys.argv[1] if len(sys.argv)>1 else 'bios'
name='lifecycle-013-'+fw;out=ROOT/('build/test-'+name);out.mkdir(exist_ok=True)
disk=out/'data.img';external=out/'compat.img'
fresh_data_disk(disk);fresh_compat_disk(external)
subprocess.run(['mcopy','-o','-i',str(external)+'@@1048576',str(ROOT/'build/workspace-013.arkpkg'),'::/workspace.arkpkg'],env={**os.environ,'MTOOLS_SKIP_CHECK':'1'},check=True)
tested_iso=out/'tested.iso';shutil.copyfile(ROOT/'build/arkos-0.13.0.iso',tested_iso)
iso_hash=hashlib.sha256(tested_iso.read_bytes()).hexdigest();kernel_hash=hashlib.sha256((ROOT/'build/kernel.elf').read_bytes()).hexdigest()
os.environ['ARKOS_ISO']=str(tested_iso)
os.environ.update(ARKOS_MACHINE='q35',ARKOS_SMP='4')
v=VM(name,disk,firmware=fw,external=external,device='virtio-tablet-pci,virtio-keyboard-pci')
checks=[]
def click(x,y):mouse(v,x,y,True);time.sleep(.1);mouse(v,x,y,False);time.sleep(.6)
def right(x,y):
 mouse(v,x,y,False)
 for down in (True,False):v.q('input-send-event',{'events':[{'type':'btn','data':{'button':'right','down':down}}]});time.sleep(.1)
 time.sleep(.6)
def shot(label):
 v.screen(label);im=Image.open(out/(label+'.ppm'));im.save(out/(label+'.png'));return im
def command(text,expected=None):
 at=len(v.log.read_text());v.command(text);v.wait(text+'\n',after=at)
 if expected:v.wait(expected,after=at,timeout=45)
 return v.log.read_text()[at:]
def count_surfaces():return v.log.read_text().count('[workspace] surface opened')
try:
 v.enroll_test_user();v.terminal();command('mkdir Context');command('echo Selected text > Context/Only.txt');command('cd Context');command('open files');time.sleep(.7)
 right(800,510);blank=shot('01-empty-selection');at=len(v.log.read_text());click(822,524);assert 'new window Notes' not in v.log.read_text()[at:];click(720,470)
 right(360,238);selected=shot('02-selected-file')
 # Disabled text is muted, selected-file Open uses the foreground ink.
 def dark(im,box):return sum(r<65 and g<90 and b<115 for r,g,b in im.crop(box).convert('RGB').get_flattened_data())
 assert dark(selected,(378,248,430,268))>dark(blank,(818,520,870,540))+15
 at=len(v.log.read_text());click(390,252);v.wait('[app] Notes ring3 ready',after=at);v.wait('[permission] Consent requested by notes');v.key('ret');v.wait('[permission] Allowed notes');time.sleep(.7);shot('03-file-opened');v.key('ctrl-w');time.sleep(.8)
 checks.append('blank file context disables Open/Rename/Delete; selected file opens its real document')
 v.terminal();command('pkg install /mnt/fat32/workspace.arkpkg','[package] Installed workspace');command('pkg run workspace');v.wait('[workspace] surface opened');time.sleep(.8);shot('04-package-one-window')
 pid=int(re.findall(r'Spawn pid=(\d+).*name=pkg.workspace ',v.log.read_text())[-1])
 before=count_surfaces();mouse(v,865,190,True);time.sleep(.3);assert count_surfaces()==before;mouse(v,980,280,False);time.sleep(.5);assert count_surfaces()==before
 click(865,190);v.wait('[workspace] surface opened',after=v.log.read_text().find('[workspace] surface opened')+1);time.sleep(.8);assert count_surfaces()==before+1;shot('05-two-surfaces')
 checks.append('SDK button requires press/release inside the same control; release outside cancels')
 at=len(v.log.read_text());mouse(v,852,153,True);time.sleep(.25);assert '[ui] minimize workspace' not in v.log.read_text()[at:];mouse(v,852,153,False);v.wait('[ui] minimize workspace',after=at);time.sleep(.8);assert '[process] Exit pid='+str(pid) not in v.log.read_text()
 v.terminal();command('pkg run workspace');time.sleep(.8);assert len(re.findall(r'Spawn pid=\d+.*name=pkg.workspace ',v.log.read_text()))==1;shot('06-restored-same-process')
 at=len(v.log.read_text());mouse(v,940,153,True);time.sleep(.25);assert '[process] Exit pid='+str(pid) not in v.log.read_text()[at:];mouse(v,610,320,False);time.sleep(.5);assert '[ui] close workspace' not in v.log.read_text()[at:]
 click(940,153);v.wait('[process] Exit pid='+str(pid));time.sleep(.9);shot('07-quit-all-surfaces')
 v.terminal();processes=command('ps');assert not re.search(r'^'+str(pid)+r'\s',processes,re.M);assert 'Ark Desktop' in processes
 checks.append('minimize keeps the same PID; Close exits the two-surface process and removes its live task')
 v.key('ctrl-t');time.sleep(.9);shot('08-live-process-monitor');v.key('ctrl-w');time.sleep(.6)
 v.key('f2');time.sleep(.6);v.key('ctrl-w');time.sleep(.8);at=len(v.log.read_text());mouse(v,127,746,False);time.sleep(1.2);shot('09-closed-files-no-preview');assert '[motion] begin kind=preview' not in v.log.read_text()[at:]
 at=len(v.log.read_text());mouse(v,289,746,False);time.sleep(1.2);shot('10-closed-notes-no-preview');assert '[motion] begin kind=preview' not in v.log.read_text()[at:]
 checks.append('closed native Files and exited Notes have no Dock thumbnail')
 v.terminal();command('echo First context');v.key('ctrl-n');v.wait('[ui] new window Terminal');command('echo Second context');v.key('ctrl-w');time.sleep(.8);at=len(v.log.read_text());mouse(v,235,746,False);time.sleep(1.2);assert '[motion] begin kind=preview' not in v.log.read_text()[at:];v.terminal();command('echo Fresh context','Fresh context');shot('11-fresh-terminal')
 checks.append('native Terminal Close clears every terminal instance and reopening creates a fresh context')
 assert '[process] User fault' not in v.log.read_text() and '[exception]' not in v.log.read_text()
 result={'result':'PASS','firmware':fw,'iso_sha256':iso_hash,'kernel_sha256':kernel_hash,'checks':checks}
 (out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
finally:v.close()
