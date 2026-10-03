#!/usr/bin/env python3
"""Actual FILE gestures and Markdown failure recovery on a disposable disk."""
import hashlib,json,os,sys,time
from PIL import Image
from fixtures import fresh_data_disk
from vm import ROOT,VM
from motion_vm_test import mouse
fw=sys.argv[1] if len(sys.argv)>1 else 'bios'
name='drop-failure-012-'+fw
out=ROOT/'build'/('test-'+name);out.mkdir(exist_ok=True)
disk=out/'data.img';fresh_data_disk(disk)
os.environ.update(ARKOS_MACHINE='q35',ARKOS_SMP='4')
v=VM(name,disk,firmware=fw,device='virtio-multitouch-pci,virtio-tablet-pci,virtio-keyboard-pci')
def command(text,expected=None):
 at=len(v.log.read_text());v.command(text);v.wait(text+'\n',after=at)
 if expected:v.wait(expected,after=at)
 return v.log.read_text()[at:]
def drag(x,y,nx,ny):
 mouse(v,x,y,True);time.sleep(.18)
 for i in range(1,13):mouse(v,x+(nx-x)*i//12,y+(ny-y)*i//12);time.sleep(.08)
 mouse(v,nx,ny,False);time.sleep(.9)
def shot(name):
 v.screen(name);Image.open(out/(name+'.ppm')).save(out/(name+'.png'))
try:
 v.enroll_test_user();v.terminal()
 command('mkdir DropTest');command('mkdir DropTest/Folder')
 command('echo Original loaded document > DropTest/Good.md')
 command('cd DropTest');command('open files');time.sleep(.7)
 v.terminal();command('pkg run ark.markdown');v.wait('[app] Markdown ring3 ready');time.sleep(.6)
 # Expose the Files rows on the left and the recipient on the right.
 drag(600,165,800,84);shot('01-windows')
 v.key('f2');time.sleep(.6);drag(360,237,1180,360);v.wait('[drop] delivered to Markdown')
 v.key('ctrl-s');time.sleep(.7);shot('02-rejected-folder')
 v.terminal();command('cat /home/ark/Welcome.md','# 欢迎')
 # Load a real file, then alter its disk contents while the reader stays open.
 v.key('f2');time.sleep(.6);at=len(v.log.read_text());drag(360,277,1180,360);v.wait('[drop] delivered to Markdown',after=at)
 shot('03-loaded-file');v.terminal();command('echo Replacement on disk > Good.md')
 command('pkg run ark.markdown');time.sleep(.6);v.key('ctrl-s');time.sleep(.7)
 v.terminal();command('cat Good.md','Original loaded document')
 # A second failed directory drop must preserve both the loaded file and path.
 v.key('f2');time.sleep(.6);at=len(v.log.read_text());drag(360,237,1180,360);v.wait('[drop] delivered to Markdown',after=at)
 v.terminal();command('echo Second replacement > Good.md');command('pkg run ark.markdown');time.sleep(.6)
 v.key('ctrl-s');time.sleep(.7);shot('04-preserved-document');v.terminal();command('cat Good.md','Original loaded document')
 assert '[exception]' not in v.log.read_text() and 'User fault' not in v.log.read_text()
 result={'result':'PASS','firmware':fw,'iso_sha256':hashlib.sha256((ROOT/'build/arkos-0.12.0.iso').read_bytes()).hexdigest(),
 'kernel_sha256':hashlib.sha256((ROOT/'build/kernel.elf').read_bytes()).hexdigest(),
 'checks':['actual file gestures delivered to native Markdown','directory read failure preserves initial document/path','accepted file is read into live document and saved back through native VFS','later directory drop preserves the loaded document and its save path']}
 (out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
finally:v.close()
