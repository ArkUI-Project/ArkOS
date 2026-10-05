#!/usr/bin/env python3
"""Native ring3 tests against production services, accounts, GPU and filesystem.
Builds an isolated ISO; no release binary or user's persistent disk is changed.
"""
from pathlib import Path
import os,subprocess,json,time,re,sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/test-process-api';OUT.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pie','-mno-red-zone','-mgeneral-regs-only','-mcmodel=small','-I'+str(ROOT/'include')]
def run(args,**kw):subprocess.run(args,check=True,**kw)
objects=[]
run(['gcc',*flags,'-c',str(ROOT/'user/start.S'),'-o',str(OUT/'start.o')])
run(['gcc',*flags,'-c',str(ROOT/'kernel/lib.c'),'-o',str(OUT/'user-lib.o')])
for name,mode in [('controller',1),('untrusted',0)]:
 run(['gcc',*flags,f'-DAPI_CONTROLLER={mode}','-c',str(ROOT/'tests/process_api_probe.c'),'-o',str(OUT/(name+'.o'))])
 run(['ld','-nostdlib','-z','max-page-size=0x1000','-T',str(ROOT/'user/user.ld'),str(OUT/'start.o'),str(OUT/'user-lib.o'),str(OUT/(name+'.o')),'-o',str(OUT/(name+'.elf'))])
 run(['ld','-r','-b','binary',name+'.elf','-o',name+'-embed.o'],cwd=OUT);objects.append(str(OUT/(name+'-embed.o')))
objects.append(str(ROOT/'build/programs_embed.o'))

sources=[ROOT/'tests/process_api_fixture.c']+sorted(p for p in (ROOT/'kernel').glob('*.c')if p.name!='main.c')+[ROOT/'boot/entry.S',ROOT/'boot/ap.S',ROOT/'kernel/interrupts.S',ROOT/'kernel/user_entry.S',ROOT/'kernel/module_asm.S']
for i,source in enumerate(sources):
 obj=OUT/f'kernel{i}.o';extra=['-Dprocess_syscall_dispatch=services_real_dispatch'] if source.name=='services.c' else []
 run(['gcc',*flags,*extra,'-c',str(source),'-o',str(obj)]);objects.append(str(obj))
tree=OUT/'iso';(tree/'boot/grub').mkdir(parents=True,exist_ok=True)
run(['ld','-nostdlib','--gc-sections','-z','noexecstack','-z','max-page-size=0x1000','-T',str(ROOT/'boot/linker.ld'),*objects,*map(str,sorted((ROOT/'build/bearssl').rglob('*.o'))),'-o',str(tree/'boot/kernel.elf')])
(tree/'boot/grub/grub.cfg').write_text('set timeout=0\nset default=0\ninsmod all_video\nset gfxmode=1280x800x32\nset gfxpayload=keep\nmenuentry "Native service boundary" {\n multiboot2 /boot/kernel.elf\n boot\n}\n')
iso=OUT/'service-boundary.iso'
with(OUT/'build.log').open('w')as log:run(['python3',str(ROOT/'scripts/mkiso.py'),str(tree),str(iso)],stdout=log,stderr=log)
results=[]
for firmware in ['bios','uefi']:
 serial=OUT/(firmware+'.log');serial.write_text('');err=(OUT/(firmware+'-qemu.log')).open('w')
 args=['qemu-system-x86_64','-machine','pc','-accel','tcg','-m','512M','-cdrom',str(iso),'-boot','d','-vga','vmware','-nic','none','-display','none','-serial','file:'+str(serial),'-qmp','stdio','-no-reboot','-device','virtio-multitouch-pci','-device','virtio-tablet-pci']
 if os.environ.get('QEMU_DATA'):args+=['-L',os.environ['QEMU_DATA']]
 if firmware=='uefi':args+=['-drive','if=pflash,format=raw,readonly=on,file='+os.environ.get('OVMF_CODE','/usr/share/OVMF/OVMF_CODE_4M.fd')]
 p=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,bufsize=0)
 try:
  p.stdout.readline();p.stdin.write(b'{"execute":"qmp_capabilities"}\n');p.stdin.flush();deadline=time.monotonic()+90
  while True:
   text=serial.read_text(errors='replace')
   if ' FAIL'in text or '[exception]'in text or '[process] User fault'in text:raise AssertionError(text)
   if '[api-probe] SYSTEM survivor and native service isolation PASS'in text:break
   if p.poll()is not None or time.monotonic()>deadline:raise RuntimeError(firmware+' timeout/exited\n'+text)
   time.sleep(.1)
  print('PASS '+firmware+'\n'+text,flush=True);results.append({'firmware':firmware,'serial':str(serial),'result':'PASS'})
 finally:p.terminate();p.wait(timeout=5);err.close()
(OUT/'results.json').write_text(json.dumps(results,indent=2)+'\n')
