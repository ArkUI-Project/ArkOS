#!/usr/bin/env python3
"""Build an isolated diagnostic kernel and verify actual GPU VRAM readback.

Never modifies the release kernel, ISO, or disks. Requires GCC, ld, GRUB,
xorriso, and QEMU. Honors QEMU_BIN, QEMU_DATA, OVMF_CODE and local tool env.
"""
from pathlib import Path
import json,os,subprocess,time,hashlib
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/test-gpu';OUT.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pie','-mno-red-zone','-mgeneral-regs-only','-mcmodel=small','-I'+str(ROOT/'include')]
sources=['tests/gpu_fixture.c','kernel/gpu.c','kernel/platform.c','kernel/lib.c','boot/entry.S','kernel/interrupts.S']
objects=[]
for source in sources:
 obj=OUT/(Path(source).stem+'.o');subprocess.run(['gcc',*flags,'-c',str(ROOT/source),'-o',str(obj)],check=True);objects.append(str(obj))
iso_tree=OUT/'iso';(iso_tree/'boot/grub').mkdir(parents=True,exist_ok=True)
elf=iso_tree/'boot/kernel.elf'
subprocess.run(['ld','-nostdlib','-z','max-page-size=0x1000','-T',str(ROOT/'boot/linker.ld'),*objects,'-o',str(elf)],check=True)
(iso_tree/'boot/grub/grub.cfg').write_text('set timeout=0\nset default=0\ninsmod all_video\nset gfxmode=1024x768x32\nset gfxpayload=keep\nmenuentry "GPU diagnostic" {\n multiboot2 /boot/kernel.elf\n boot\n}\n')
iso=OUT/'gpu-diagnostic.iso'
with (OUT/'build.log').open('w') as log:subprocess.run(['python3',str(ROOT/'scripts/mkiso.py'),str(iso_tree),str(iso)],stdout=log,stderr=log,check=True)
results=[]
for vga,firmware in [('vmware','bios'),('std','bios'),('vmware','uefi')]:
 label=vga+'-'+firmware;serial=OUT/(label+'.log');qlog=(OUT/(label+'-qemu.log')).open('w')
 cmd=[os.environ.get('QEMU_BIN','qemu-system-x86_64'),'-machine','pc','-accel','tcg','-m','256M','-cdrom',str(iso),'-boot','d','-vga',vga,'-nic','none','-display','none','-serial','file:'+str(serial),'-qmp','stdio']
 if os.environ.get('QEMU_DATA'):cmd+=['-L',os.environ['QEMU_DATA']]
 if firmware=='uefi':
  code=Path(os.environ.get('OVMF_CODE','/usr/share/OVMF/OVMF_CODE_4M.fd'))
  if not code.exists():print('SKIP UEFI: OVMF_CODE unavailable');qlog.close();continue
  cmd+=['-drive','if=pflash,format=raw,readonly=on,file='+str(code)]
 p=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=qlog)
 def q(name,args=None):
  p.stdin.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode());p.stdin.flush()
  while True:
   line=p.stdout.readline()
   if not line:raise RuntimeError('QEMU closed QMP')
   response=json.loads(line)
   if 'error' in response:raise RuntimeError(response)
   if 'return' in response:return response['return']
 try:
  p.stdout.readline();q('qmp_capabilities');deadline=time.monotonic()+35
  while True:
   text=serial.read_text(errors='replace') if serial.exists() else ''
   if '[gpu-test] FAIL' in text:raise AssertionError(text)
   if '[gpu-test] PASS' in text:break
   if p.poll() is not None or time.monotonic()>deadline:raise RuntimeError(label+' timed out\n'+text)
   # Force a real display refresh as well as capturing the diagnostic state.
   q('screendump',{'filename':str(OUT/(label+'.ppm'))});time.sleep(.1)
  expected='VMware SVGA II / FIFO 2D' if vga=='vmware' else 'Software framebuffer'
  assert 'PASS backend='+expected in text,text
  result=text.split('[gpu-test] PASS ')[1].splitlines()[0]
  print('PASS '+label+': '+result,flush=True);results.append({'configuration':label,'result':result})
 finally:
  if p.poll() is None:q('quit');p.wait(timeout=5)
  qlog.close()
(OUT/'results.json').write_text(json.dumps({'kernel_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'checks':'VRAM readback; device fill/copy; retained-tile copy; forced partial update; streamed/clipped damage and shadow invalidation; FIFO wrap; software fallback','results':results},indent=2)+'\n')
