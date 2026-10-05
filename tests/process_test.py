#!/usr/bin/env python3
"""Real ring3 protection and scheduling tests in an isolated native test ISO."""
from pathlib import Path
import os,subprocess,json,time,selectors
ROOT=Path(__file__).resolve().parents[1];SMP=os.environ.get('ARK_SMP_TEST')=='1';OUT=ROOT/('build/test-process-smp' if SMP else 'build/test-process');OUT.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pie','-mno-red-zone','-mgeneral-regs-only','-mcmodel=small','-I'+str(ROOT/'include')]
if SMP:flags.append('-DARK_SMP_TEST')
def run(args,**kw):subprocess.run(args,check=True,**kw)
run(['gcc',*flags,'-c',str(ROOT/'user/start.S'),'-o',str(OUT/'start.o')])
objects=[]
for i in range(8):
 run(['gcc',*flags,f'-DPROBE_MODE={i}','-c',str(ROOT/'tests/process_probe.c'),'-o',str(OUT/f'probe{i}.o')])
 run(['ld','-nostdlib','-z','max-page-size=0x1000','-T',str(ROOT/'user/user.ld'),str(OUT/'start.o'),str(OUT/f'probe{i}.o'),'-o',str(OUT/f'probe{i}.elf')])
 run(['ld','-r','-b','binary',f'probe{i}.elf','-o',f'embed{i}.o'],cwd=OUT);objects.append(str(OUT/f'embed{i}.o'))
for i,s in enumerate(['tests/process_fixture.c','kernel/process.c','kernel/elf.c','kernel/platform.c','kernel/device.c','kernel/pci.c','kernel/block.c','kernel/ahci.c','kernel/mmio.c','kernel/lib.c','boot/entry.S','kernel/interrupts.S','kernel/user_entry.S']+(['kernel/smp.c','kernel/microcode.c','boot/ap.S'] if SMP else [])):
 o=OUT/f'kernel{i}.o';run(['gcc',*flags,'-c',str(ROOT/s),'-o',str(o)]);objects.append(str(o))
tree=OUT/'iso';(tree/'boot/grub').mkdir(parents=True,exist_ok=True)
run(['ld','-nostdlib','-z','noexecstack','-z','max-page-size=0x1000','-T',str(ROOT/'boot/linker.ld'),*objects,'-o',str(tree/'boot/kernel.elf')])
(tree/'boot/grub/grub.cfg').write_text('set timeout=0\nset default=0\ninsmod all_video\nset gfxmode=1024x768x32\nset gfxpayload=keep\nmenuentry "Protection test" {\n multiboot2 /boot/kernel.elf\n boot\n}\n')
iso=OUT/'protection.iso'
with (OUT/'build.log').open('w')as log:run(['python3',str(ROOT/'scripts/mkiso.py'),str(tree),str(iso)],stdout=log,stderr=log)
results=[]
for firmware in ['bios','uefi']:
 serial=OUT/(firmware+'.log');serial.write_text('');err=(OUT/(firmware+'-qemu.log')).open('w')
 args=['qemu-system-x86_64','-machine','pc','-accel','tcg','-smp','4' if SMP else '1','-m','512M','-cdrom',str(iso),'-boot','d','-vga','std','-nic','none','-display','none','-serial','file:'+str(serial),'-qmp','stdio','-no-reboot']
 if os.environ.get('QEMU_DATA'):args+=['-L',os.environ['QEMU_DATA']]
 if firmware=='uefi':args+=['-drive','if=pflash,format=raw,readonly=on,file='+os.environ.get('OVMF_CODE','/usr/share/OVMF/OVMF_CODE_4M.fd')]
 p=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,bufsize=0)
 try:
  p.stdout.readline();p.stdin.write(b'{"execute":"qmp_capabilities"}\n');p.stdin.flush();deadline=time.monotonic()+60
  while True:
   text=serial.read_text(errors='replace')
   if '[process-test] FAIL'in text or '[exception]'in text or '[probe]'in text and ' FAIL'in text:raise AssertionError(text)
   if '[probe] Ring3 survivor running after faults and preemption PASS'in text:break
   if p.poll()is not None or time.monotonic()>deadline:raise RuntimeError('timeout/exited '+firmware+'\n'+text)
   time.sleep(.1)
  print('PASS '+firmware+'\n'+text,flush=True);results.append({'firmware':firmware,'serial':str(serial),'result':'PASS'})
 finally:
  p.terminate();p.wait(timeout=5);err.close()
(OUT/'results.json').write_text(json.dumps(results,indent=2)+'\n')
