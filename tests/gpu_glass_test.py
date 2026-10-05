#!/usr/bin/env python3
"""Isolated native Ring3 GPU fixture; actual VirGL pixels, not host substitutes.
Honors QEMU_BIN/QEMU_DATA/OVMF_CODE/ARKOS_GPU_DEVICE/ARKOS_SPICE.
"""
from pathlib import Path
import hashlib,json,os,subprocess
from vm import ROOT,VM
OUT=ROOT/'build/test-gpu-glass';OUT.mkdir(exist_ok=True)
CC=os.environ.get('ARKOS_CC','gcc')
LD=os.environ.get('ARKOS_LD','ld')
flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pie','-mno-red-zone','-mcmodel=small','-I'+str(ROOT/'include')]
log=(OUT/'build.log').open('w')
def run(args,**kw):subprocess.run(args,check=True,stdout=log,stderr=log,**kw)
uf=flags+['-msse2','-mfpmath=sse','-fno-math-errno']
objects=[];user_objects=[]
for source in ['user/start.S','kernel/lib.c','user/raster.c','user/liquid_glass.c']:
 obj=OUT/(Path(source).stem+'-user.o');run([CC,*uf,'-c',str(ROOT/source),'-o',str(obj)]);user_objects.append(str(obj))
for name,role in [('controller',1),('untrusted',0)]:
 obj=OUT/(name+'.o');run([CC,*uf,f'-DGLASS_CONTROLLER={role}','-c',str(ROOT/'tests/gpu_glass_probe.c'),'-o',str(obj)])
 run([LD,'-nostdlib','-z','noexecstack','-z','max-page-size=0x1000','-T',str(ROOT/'user/user.ld'),*user_objects,str(obj),'-o',str(OUT/(name+'.elf'))])
 run([LD,'-m','elf_x86_64','-r','-b','binary',name+'.elf','-o',name+'-embed.o'],cwd=OUT);objects.append(str(OUT/(name+'-embed.o')))
sources=[ROOT/'tests/gpu_glass_fixture.c']+sorted(p for p in (ROOT/'kernel').glob('*.c') if p.name!='main.c')+[ROOT/'boot/entry.S',ROOT/'boot/ap.S',ROOT/'kernel/interrupts.S',ROOT/'kernel/user_entry.S']
for i,source in enumerate(sources):
 obj=OUT/f'kernel{i}.o';extra=['-Dprocess_syscall_dispatch=services_real_dispatch'] if source.name=='services.c' else []
 run([CC,*flags,'-mgeneral-regs-only',*extra,'-c',str(source),'-o',str(obj)]);objects.append(str(obj))
objects+=[str(ROOT/'build/programs_embed.o'),*map(str,sorted((ROOT/'build/bearssl').rglob('*.o')))]
tree=OUT/'iso';(tree/'boot/grub').mkdir(parents=True,exist_ok=True);elf=tree/'boot/kernel.elf'
run([LD,'-nostdlib','--gc-sections','-z','noexecstack','-z','max-page-size=0x1000','-T',str(ROOT/'boot/linker.ld'),*objects,'-o',str(elf)])
(tree/'boot/grub/grub.cfg').write_text('set timeout=0\nset default=0\ninsmod all_video\nset gfxmode=1280x800x32\nset gfxpayload=keep\nmenuentry "ArkOS GPU pixels" {\n multiboot2 /boot/kernel.elf\n boot\n}\n')
iso=OUT/'gpu-glass.iso';run(['python3',str(ROOT/'scripts/mkiso.py'),str(tree),str(iso)]);log.close()
os.environ.update(ARKOS_MACHINE='q35',ARKOS_SMP='4',ARKOS_ISO=str(iso));results=[]
for firmware in ['bios','uefi']:
 v=VM('gpu-glass-'+firmware,firmware=firmware,device='virtio-tablet-pci',ready='[glass-probe] Scheduler ready')
 try:
  v.wait('[glass-probe] GPU pixels, live sampling, clipping, XRGB, pointer/geometry/capability isolation PASS',90)
  serial=v.log.read_text();host=(v.out/'qemu.log').read_text(errors='replace')
  assert ' FAIL' not in serial and 'User fault' not in serial and '[exception]' not in serial
  assert 'GPU blur/lens draw, pixels and fence/readback verified' in serial and 'untrusted GPU access rejected' in serial
  assert 'failed to dispatch' not in host and 'shader failed' not in host
  results.append({'firmware':firmware,'result':'PASS','serial':str(v.log),'renderer_log':str(v.out/'qemu.log'),'pixel_reports':[s for s in serial.splitlines() if '[glass-probe] scene=' in s]})
  print('PASS '+firmware+'\n'+'\n'.join(results[-1]['pixel_reports']),flush=True)
 finally:v.close()
(OUT/'results.json').write_text(json.dumps({'kernel_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'scope':'diagnostic kernel with production GPU, memory, services and isolated Ring3 probe','results':results},indent=2)+'\n')
