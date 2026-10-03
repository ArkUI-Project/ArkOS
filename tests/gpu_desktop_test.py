#!/usr/bin/env python3
"""Compare the actual desktop rendered by SVGA FIFO and software fallback.

The first headless screendump intentionally happens AFTER desktop startup;
this regresses QEMU's deferred display-mode commit, not just a toy framebuffer.
"""
from pathlib import Path
import hashlib,json,os,shutil,tempfile,time
from PIL import Image,ImageChops
import vm as vm_helper
from vm import VM,ROOT
parent=ROOT/'build/test-gpu';parent.mkdir(parents=True,exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='desktop-',dir=parent))
(work/'build').mkdir()
iso=work/'build/arkos-0.4.0.iso';shutil.copy2(Path(os.environ.get('ARKOS_ISO',str(ROOT/'build/arkos-0.4.0.iso'))),iso)
vm_helper.ROOT=work
images={}
for gpu,firmware in [('vmware','bios'),('std','bios'),('vmware','uefi')]:
 label=gpu+'-'+firmware
 vm=VM('desktop-'+label,device=None,gpu=gpu,firmware=firmware)
 try:
  vm.screen('initial')
  if gpu=='vmware':vm.wait('[gpu] SVGA II FIFO fill/copy verified at full-mode bounds')
  time.sleep(4.1) # Let the finite welcome notification disappear in every run.
  vm.screen('desktop')
  ppm=vm.out/'desktop.ppm';im=Image.open(ppm).convert('RGB');im.save(ppm.with_suffix('.png'));images[label]=im
  vm.key('f4');vm.wait('[ui] open Settings');time.sleep(2.2);vm.screen('settings')
  vm.terminal();vm.command('echo gpu-frame-check');vm.wait('\ngpu-frame-check');time.sleep(2.2);vm.screen('terminal')
  extra=vm.out/'qmp-unexpected.log'
  if extra.exists():assert 'Unknown command' not in extra.read_text(),extra.read_text()
  assert '[exception]' not in vm.log.read_text()
 finally:vm.close()
comparisons={}
for label in ['vmware-bios','vmware-uefi']:
 a,b=images[label],images['std-bios'];assert a.size==b.size
 # RTC minute changes between independent boots are not graphics faults.
 a=a.crop((0,38,a.width,a.height));b=b.crop((0,38,b.width,b.height))
 diff=ImageChops.difference(a,b);diff.save(work/(label+'-difference.png'))
 r,g,b=diff.split();mask=ImageChops.lighter(ImageChops.lighter(r,g),b)
 changed=sum(v!=0 for v in mask.tobytes());comparisons[label]=changed
result={'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'changed_pixels_below_menu_bar':comparisons,'evidence':str(work)}
(work/'results.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result),flush=True)
assert not any(comparisons.values()),'SVGA desktop does not match software fallback: '+str(work)
print('PASS: BIOS and UEFI SVGA desktops are pixel-identical to software below the RTC menu bar; settings/terminal frames have no FIFO errors',flush=True)
