#!/usr/bin/env python3
"""Production guest driver + VirtIO-serial DMA; a local peer supplies wire packets.

This complements real UTM/SPICE window tests. It does not certify the SPICE
client UI: the peer is an explicit test fixture, not an OS runtime service.
"""
import hashlib,json,os,shutil,socket,struct,sys,tempfile,time
from pathlib import Path
from PIL import Image,ImageChops
from fixtures import fresh_data_disk
from vm import VM,ROOT

firmware=sys.argv[1] if len(sys.argv)>1 else 'bios'
name='spice-mouse-'+firmware
out=ROOT/'build'/('test-'+name);out.mkdir(parents=True,exist_ok=True)
disk=out/'data.img';fresh_data_disk(disk)
iso=out/'tested.iso';shutil.copyfile(ROOT/'build/arkos-0.13.0.iso',iso)
os.environ.update(ARKOS_ISO=str(iso),ARKOS_MACHINE='q35',ARKOS_SMP='4',ARKOS_MEMORY='1024M')
sock_path=Path(tempfile.gettempdir())/f'arkos-mouse-{os.getpid()}-{firmware}.sock'
extra=['-device','virtio-serial-pci,id=arkpointerbus,max_ports=2,disable-legacy=on',
       '-chardev',f'socket,id=arkpointer,path={sock_path},server=on,wait=off',
       '-device','virtserialport,nr=1,bus=arkpointerbus.0,chardev=arkpointer,name=com.redhat.spice.0']
v=VM(name,disk,firmware,device='virtio-tablet-pci,virtio-keyboard-pci',extra_args=extra)
peer=None;checks=[]
def connect():
 global peer
 peer=socket.socket(socket.AF_UNIX);peer.settimeout(10);peer.connect(str(sock_path))
 v.wait('[spice-input] Native absolute mouse channel connected.')
 raw=b''
 while len(raw)<36:raw+=peer.recv(36-len(raw))
 assert struct.unpack_from('<II',raw)==(1,28)
 assert struct.unpack_from('<II',raw,8)==(1,6)
 assert struct.unpack_from('<I',raw,32)[0]==1|(1<<13)
def wire(kind,body=b'',port=2):
 payload=struct.pack('<IIQI',1,kind,0,len(body))+body
 peer.sendall(struct.pack('<II',port,len(payload))+payload)
def pointer(x,y,mask=0):
 wire(1,struct.pack('<IIIB',x,y,mask,0));time.sleep(.12)
def click(x,y):pointer(x,y,2);pointer(x,y,0);time.sleep(.3)
def shot(label):
 v.screen(label);im=Image.open(v.out/(label+'.ppm')).convert('RGB');im.save(v.out/(label+'.png'));return im
def changed(a,b,box):
 return sum(pixel!=(0,0,0) for pixel in ImageChops.difference(a.crop(box),b.crop(box)).getdata())
try:
 v.wait('[spice-input] Native VirtIO-serial mouse transport ready.')
 connect();v.enroll_test_user();v.key('f4');time.sleep(.8)
 before=shot('01-settings');checkpoint=len(v.log.read_text())
 pointer(480,282,2);time.sleep(.25)
 assert '[ui] Settings page' not in v.log.read_text()[checkpoint:]
 pointer(420,240,0);assert '[ui] Settings page' not in v.log.read_text()[checkpoint:]
 click(480,282);v.wait('[ui] Settings page 关于本机',after=checkpoint)
 v.wait('[page] end frames=',after=checkpoint);shot('02-about-on-release')
 click(440,137);time.sleep(.4)
 checks.append('native SPICE press/leave cancels; release opens the correct child page')
 before=shot('03-before-drag');pointer(590,73,2);pointer(650,113,2);pointer(650,113,0)
 after=shot('04-after-drag');assert changed(before,after,(500,45,800,86))>5000
 checks.append('SPICE held motion moves the Settings window through the production desktop')
 # Permission list has more rows than fit; wheel scrolls actual clipped rows.
 click(300,374);pointer(800,440,0);before=shot('05-before-wheel')
 for _ in range(3):pointer(800,440,32);pointer(800,440,0)
 after=shot('06-after-wheel');assert changed(before,after,(460,265,1090,610))>3000
 checks.append('wire wheel scrolls a real Settings application list')
 # Disconnect a held drag, reconnect, and move without a button. The window
 # must stay in place; this tests release generation through SYS_EVENT.
 pointer(650,113,2);before=shot('07-held-before-disconnect')
 checkpoint=len(v.log.read_text());peer.close();peer=None
 v.wait('[spice-input] Mouse channel disconnected.',after=checkpoint)
 connect();pointer(700,155,0);after=shot('08-reconnected-release')
 assert changed(before,after,(205,135,450,630))<2500
 checks.append('port disconnect releases a held drag; reconnect resumes absolute input')
 log=v.log.read_text();assert '[exception]' not in log and '[process] User fault' not in log
 result={'result':'PASS','firmware':firmware,'transport':'actual modern VirtIO-serial PCI + test socket peer',
         'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),
         'kernel_sha256':hashlib.sha256((ROOT/'build/kernel.elf').read_bytes()).hexdigest(),'checks':checks}
 (out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
finally:
 if peer:peer.close()
 v.close();sock_path.unlink(missing_ok=True)
