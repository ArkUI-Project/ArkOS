#!/usr/bin/env python3
"""Production VM/ArkFS/block code under actual x86 CPU faults. Fixture disks only."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,sys,time
from fixtures import fresh_data_disk
from vm import ROOT,VM
OUT=ROOT/'build/test-memory-013';OUT.mkdir(exist_ok=True)
CC=os.environ.get('ARKOS_CC','gcc');LD=os.environ.get('ARKOS_LD','ld')
flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pie','-mno-red-zone','-mgeneral-regs-only','-mcmodel=small','-I'+str(ROOT/'include')]
def run(args,**kw):subprocess.run(args,check=True,**kw)
log=(OUT/'build.log').open('w')
run([CC,*flags,'-c',str(ROOT/'user/start.S'),'-o',str(OUT/'start.o')],stdout=log,stderr=log)
run([CC,*flags,'-c',str(ROOT/'kernel/lib.c'),'-o',str(OUT/'user-lib.o')],stdout=log,stderr=log)
def user(name,readonly=False):
 extra=['-DVM_WRITE_FAILURE'] if readonly else []
 run([CC,*flags,*extra,'-c',str(ROOT/'tests/vm_memory_probe.c'),'-o',str(OUT/(name+'.o'))],stdout=log,stderr=log)
 run([LD,'-nostdlib','-z','max-page-size=0x1000','-T',str(ROOT/'user/user.ld'),str(OUT/'start.o'),str(OUT/'user-lib.o'),str(OUT/(name+'.o')),'-o',str(OUT/(name+'.elf'))],stdout=log,stderr=log)
 run([LD,'-m','elf_x86_64','-r','-b','binary',name+'.elf','-o',name+'-embed.o'],cwd=OUT,stdout=log,stderr=log)
user('untrusted');user('controller')
objects=[]
sources=[ROOT/'tests/vm_memory_fixture.c']+sorted(p for p in (ROOT/'kernel').glob('*.c')if p.name!='main.c')+[ROOT/'boot/entry.S',ROOT/'boot/ap.S',ROOT/'kernel/interrupts.S',ROOT/'kernel/user_entry.S']
for i,s in enumerate(sources):
 obj=OUT/f'kernel{i}.o';extra=['-Dprocess_syscall_dispatch=services_real_dispatch'] if s.name=='services.c' else []
 run([CC,*flags,*extra,'-c',str(s),'-o',str(obj)],stdout=log,stderr=log);objects.append(str(obj))
objects+=[str(OUT/'controller-embed.o'),str(OUT/'untrusted-embed.o'),str(ROOT/'build/programs_embed.o'),*map(str,sorted((ROOT/'build/bearssl').rglob('*.o')))]
def image(name):
 tree=OUT/name;(tree/'boot/grub').mkdir(parents=True,exist_ok=True)
 run([LD,'-nostdlib','--gc-sections','-z','noexecstack','-z','max-page-size=0x1000','-T',str(ROOT/'boot/linker.ld'),*objects,'-o',str(tree/'boot/kernel.elf')],stdout=log,stderr=log)
 (tree/'boot/grub/grub.cfg').write_text('set timeout=0\nset default=0\ninsmod all_video\nset gfxmode=1280x800x32\nset gfxpayload=keep\nterminal_output gfxterm\nmenuentry "ArkOS memory validation" {\n multiboot2 /boot/kernel.elf\n boot\n}\n')
 iso=OUT/(name+'.iso');run(['grub-mkrescue','-o',str(iso),str(tree)],stdout=log,stderr=log);return iso
iso=image('memory');results=[]
os.environ.update(ARKOS_MACHINE='q35',ARKOS_SMP='1')
for fw in ([] if '--failure-only' in sys.argv else ['bios','uefi']):
 disk=OUT/(fw+'.img');fresh_data_disk(disk);os.environ['ARKOS_ISO']=str(iso);v=VM('memory-013-'+fw,disk,firmware=fw,ready='[vm-probe] Scheduler ready')
 try:
  v.wait('[vm-probe] Real physical exhaustion, automatic page-out/page-in and file preservation PASS',timeout=180)
  text=v.log.read_text();assert ' FAIL' not in text and '[exception]' not in text;assert text.count('[process] User fault')==3
  results.append({'firmware':fw,'result':'PASS','kernel_sha256':hashlib.sha256((OUT/'memory/boot/kernel.elf').read_bytes()).hexdigest(),'serial':str(v.log),'disk_sha256':hashlib.sha256(disk.read_bytes()).hexdigest()});print('PASS demand and swap '+fw,flush=True)
 finally:v.close()
# Restart a populated fixture with real backend writes rejected by QEMU blkdebug.
user('controller',True);ro_iso=image('memory-readonly');os.environ['ARKOS_ISO']=str(ro_iso)
# VM helper normally owns a writable test drive; launch directly for this failure path.
serial=OUT/'write-failure.log';serial.write_text('');err=(OUT/'write-failure-qemu.log').open('w')
failed_disk=OUT/'write-failure.img';shutil.copyfile(OUT/'bios.img',failed_disk)
config=OUT/'deny-writes.conf';config.write_text('[inject-error]\nevent = "write_aio"\nerrno = "13"\n')
args=['qemu-system-x86_64','-machine','q35','-cpu','max','-m','512M','-cdrom',str(ro_iso),'-boot','d','-vga','vmware','-display','none','-serial','file:'+str(serial),'-nic','none','-drive','file=blkdebug:'+str(config)+':'+str(failed_disk)+',format=raw,if=ide,index=0,werror=report','-no-reboot']
p=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=err)
try:
 deadline=time.monotonic()+90
 while True:
  text=serial.read_text(errors='replace') if serial.exists() else ''
  if ' FAIL' in text or '[exception]' in text or '[process] User fault' in text:raise AssertionError(text)
  if '[vm-probe] Denied disk writes preserve resident pages and files PASS' in text:break
  if p.poll() is not None or time.monotonic()>deadline:raise RuntimeError(text+'\n'+(OUT/'write-failure-qemu.log').read_text())
  time.sleep(.2)
 results.append({'case':'backend rejects writes with EACCES','result':'PASS','serial':str(serial),'kernel_sha256':hashlib.sha256((OUT/'memory-readonly/boot/kernel.elf').read_bytes()).hexdigest()});print('PASS real block write rejection',flush=True)
finally:p.terminate();p.wait(timeout=5);err.close();log.close()
(OUT/'results.json').write_text(json.dumps(results,indent=2)+'\n')
