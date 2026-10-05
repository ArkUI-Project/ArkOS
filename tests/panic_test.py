from vm import VM,ROOT
from pathlib import Path
import subprocess,os,shutil
from PIL import Image
out=ROOT/'build/test-panic';out.mkdir(exist_ok=True)
flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pie','-mno-red-zone','-mgeneral-regs-only','-mcmodel=small','-Iinclude']
subprocess.run(['gcc',*flags,'-c','tests/panic_fixture.c','-o',str(out/'fixture.o')],check=True,cwd=ROOT)
objects=sorted(str(p) for p in (ROOT/'build').glob('*.o') if p.name!='main.o')
tree=out/'iso';(tree/'boot/grub').mkdir(parents=True,exist_ok=True)
subprocess.run(['ld','-nostdlib','-z','noexecstack','-z','max-page-size=0x1000','-T','boot/linker.ld',str(out/'fixture.o'),*objects,'-o',str(tree/'boot/kernel.elf')],check=True,cwd=ROOT)
shutil.copyfile(ROOT/'boot/grub.cfg',tree/'boot/grub/grub.cfg')
with (out/'build.log').open('w') as f:subprocess.run(['python3',str(ROOT/'scripts/mkiso.py'),str(tree),str(out/'panic.iso')],check=True,stdout=f,stderr=f)
os.environ['ARKOS_ISO']=str(out/'panic.iso');os.environ['ARKOS_SMP']='4';os.environ['ARKOS_MACHINE']='q35'
class PanicVM(VM):
 def wait(self,s,timeout=20,after=0):return super().wait('[panic] invalid opcode',timeout,after)
for fw in ['bios','uefi']:
 v=PanicVM('panic-'+fw,firmware=fw)
 try:
  v.screen('panic');im=Image.open(v.out/'panic.ppm');im.save(v.out/'panic.png');pixels=list(im.getdata());assert sum(c==(0x16,0x3d,0x86) for c in pixels)>len(pixels)*.8;assert sum(c==(255,255,255) for c in pixels)>1000
  print('PASS native kernel invalid-opcode fatal screen',fw)
 finally:v.close()
