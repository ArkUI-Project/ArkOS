#!/usr/bin/env python3
"""48 installed packages in real storage, reboot recovery and production Launcher pagination."""
from pathlib import Path
import hashlib,json,os,struct,subprocess,sys,time,zlib
from PIL import Image
from fixtures import fresh_data_disk
from vm import ROOT,VM
sys.path.insert(0,str(ROOT/'scripts'));from arkpkg import compact_elf,text
OUT=ROOT/'build/test-package-count';OUT.mkdir(parents=True,exist_ok=True);CC=os.environ.get('ARK_CC','gcc');LD=os.environ.get('ARK_LD','ld');log=(OUT/'build.log').open('w')
def run(args,**kw):subprocess.run(list(map(str,args)),check=True,stdout=log,stderr=log,**kw)
flags=['-std=c11','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pie','-mno-red-zone','-mgeneral-regs-only','-mcmodel=small','-I'+str(ROOT/'include')]
for name in ['app','controller']:
 source=ROOT/'tests'/('package_count_'+name+'.c');obj=OUT/(name+'.o');run([CC,*flags,'-c',source,'-o',obj])
 if name=='app':
  run([LD,'-nostdlib','-z','max-page-size=0x1000','-T',ROOT/'user/user.ld',ROOT/'build/user/start.o',ROOT/'build/user/lib.o',obj,'-o',OUT/'app.elf'])
  payload=compact_elf((OUT/'app.elf').read_bytes());h=bytearray(256);h[:8]=b'ARKPKG1\0';struct.pack_into('<HH9I',h,8,1,62,256,256+len(payload),1,6,1,0,0,len(payload),0);h[48:80]=text('countfixture',32);h[80:144]=text('Count fixture',64);h[144:208]=text('Native package count test',64);h[208:240]=hashlib.sha256(payload).digest();struct.pack_into('<I',h,252,zlib.crc32(h[:252]));(OUT/'fixture.arkpkg').write_bytes(h+payload)
  (OUT/'payload.S').write_text('.section .rodata\n.global fixture_package,fixture_package_end\nfixture_package:\n.incbin "'+str(OUT/'fixture.arkpkg')+'"\nfixture_package_end:\n')
  run([CC,'-m64','-c',OUT/'payload.S','-o',OUT/'payload.o'])
run([LD,'-nostdlib','-z','max-page-size=0x1000','-T',ROOT/'user/user.ld',ROOT/'build/user/start.o',ROOT/'build/user/lib.o',OUT/'controller.o',OUT/'payload.o','-o',OUT/'controller.elf'])
(OUT/'controller.S').write_text('.section .rodata\n.global controller_start,controller_end\ncontroller_start:\n.incbin "'+str(OUT/'controller.elf')+'"\ncontroller_end:\n');run([CC,'-m64','-c',OUT/'controller.S','-o',OUT/'controller-embed.o'])
run([CC,*flags,'-c',ROOT/'tests/package_count_fixture.c','-o',OUT/'fixture.o'])
# Reuse the actual current guest kernel modules, including immutable system packages.
objects=sorted(p for p in (ROOT/'build').glob('*.o') if p.stem in [s.stem for s in (ROOT/'kernel').glob('*.c')] and p.stem!='main')
objects += [ROOT/'build'/(s+'.o') for s in ['entry','ap','interrupts','user_entry','programs_embed']]+sorted((ROOT/'build/bearssl').rglob('*.o'))
tree=OUT/'iso';(tree/'boot/grub').mkdir(parents=True,exist_ok=True);elf=tree/'boot/kernel.elf';run([LD,'-nostdlib','--gc-sections','-z','noexecstack','-z','max-page-size=0x1000','-T',ROOT/'boot/linker.ld',OUT/'fixture.o',OUT/'controller-embed.o',*objects,'-o',elf])
(tree/'boot/grub/grub.cfg').write_text('set timeout=0\ninsmod all_video\nset gfxmode=1280x800x32\nset gfxpayload=keep\nmenuentry "Package count test" {\n multiboot2 /boot/kernel.elf\n boot\n}\n');iso=OUT/'package-count.iso';run(['python3',str(ROOT/'scripts/mkiso.py'),tree,iso]);log.close();disk=OUT/'data.img';fresh_data_disk(disk)
results=[]
for phase in ['install','reboot']:
 serial=OUT/(phase+'.log');serial.write_text('');err=(OUT/(phase+'-qemu.log')).open('w');p=subprocess.Popen(['qemu-system-x86_64','-cpu','max','-m','512M','-vga','vmware','-cdrom',str(iso),'-boot','d','-drive','file='+str(disk)+',format=raw,if=ide,index=0','-display','none','-serial','file:'+str(serial),'-nic','none'],stdout=err,stderr=err)
 try:
  end=time.monotonic()+90
  while True:
   s=serial.read_text()
   if '[package-count] FAIL' in s or 'User fault' in s:raise AssertionError(s)
   if '[package-count] PASS' in s and '[package-count] executable surface PASS' in s:break
   if p.poll() is not None or time.monotonic()>end:raise TimeoutError(s)
   time.sleep(.1)
  results.append({'phase':phase,'result':'PASS'});print('PASS '+phase+': 48 packages, 65 total, verified execution',flush=True)
 finally:p.terminate();p.wait(timeout=5);err.close()
# Independent committed-root/index decoding, without ArkOS blob/package code.
data=disk.read_bytes();banks=[]
for lba in (8192,8200):
 root=data[lba*512:(lba+8)*512]
 if root[:8]==b'ARKBLOB2' and zlib.crc32(root[:4092])==struct.unpack_from('<I',root,4092)[0]:banks.append(root)
root=max(banks,key=lambda b:struct.unpack_from('<Q',b,8)[0]);next,count=struct.unpack_from('<II',root,16);names=[];pages=0;seen=set()
while next:
 assert next not in seen;seen.add(next);page=data[next*512:(next+8)*512];assert page[:7]==b'ARKIDX2' and zlib.crc32(page[:4092])==struct.unpack_from('<I',page,4092)[0];next,n=struct.unpack_from('<II',page,8);pages+=1
 for i in range(n):
  r=page[32+i*96:128+i*96];uid,start,bytes,crc=struct.unpack_from('<4I',r);body=data[start*512:start*512+bytes];assert zlib.crc32(body)==crc;names.append(r[24:88].split(b'\0')[0].decode())
assert len(names)==count==49 and pages==2 and all('@pkg.many'+str(i) in names for i in range(48))
os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4';v=VM('package-count-desktop',disk,device='virtio-multitouch-pci,virtio-tablet-pci')
try:
 v.wait('[session] login ready');v.type('Count-Test42!');v.key('ret');v.wait('[session] desktop unlocked');v.terminal();v.command('pkg list');v.wait('many47 1.0.0');v.key('ctrl-spc');time.sleep(.5);v.screen('launcher-page1');v.key('pgdn');time.sleep(.5);v.screen('launcher-page2');v.key('pgdn');time.sleep(.5);v.screen('launcher-page3');v.key('esc');v.terminal();v.command('pkg run many47');v.wait('[package-count] executable surface PASS');assert 'User fault' not in v.log.read_text()
 for p in v.out.glob('*.ppm'):Image.open(p).save(p.with_suffix('.png'))
 results.append({'phase':'production desktop and launcher pagination','result':'PASS'})
finally:v.close()
(OUT/'results.json').write_text(json.dumps({'kernel_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'packages':48,'total_applications':65,'blob_records':count,'index_pages':pages,'results':results},indent=2)+'\n')
