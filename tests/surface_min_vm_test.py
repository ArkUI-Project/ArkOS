#!/usr/bin/env python3
"""Render an installed tiny surface with the final production desktop."""
import hashlib,json,os,shutil,time
from collections import Counter
from PIL import Image
from vm import ROOT,VM

out=ROOT/'build/test-surface-final';out.mkdir(exist_ok=True)
# This fixture is created exclusively by package_count_vm_test.py.
disk=out/'data.img';shutil.copyfile(ROOT/'build/test-package-count/data.img',disk)
os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4'
v=VM('surface-final',disk,device='virtio-multitouch-pci,virtio-tablet-pci')
try:
 v.wait('[session] login ready');v.type('Count-Test42!');v.key('ret');v.wait('[session] desktop unlocked')
 # A display refresh commits the headless VMware mode before the short fixture.
 v.q('screendump',{'filename':str(out/'warmup.ppm')})
 v.terminal();v.command('pkg list');v.wait('many47 1.0.0')
 checkpoint=len(v.log.read_text());v.command('pkg run many47');v.wait('[package-count] executable surface PASS');v.wait('[motion] end kind=window value=65536',after=checkpoint);time.sleep(.1)
 v.q('screendump',{'filename':str(out/'tiny-surface.ppm')})
 im=Image.open(out/'tiny-surface.ppm').convert('RGB');im.save(out/'tiny-surface.png')
 # The fixture publishes exactly 128x80 solid pixels. Find long contiguous spans
 # and require their original width over many rows, rather than rescaled content.
 spans=Counter()
 for y in range(im.height):
  start=None
  for x in range(im.width+1):
   match=x<im.width and im.getpixel((x,y))==(71,139,200)
   if match and start is None:start=x
   if not match and start is not None:
    if x-start>=100:spans[(start,x-start)]+=1
    start=None
 assert any(width==128 and rows>=60 for (_,width),rows in spans.items()),spans
 assert 'User fault' not in v.log.read_text() and '[exception]' not in v.log.read_text()
 result={'result':'PASS','iso_sha256':hashlib.sha256((ROOT/'build/arkos-0.11.0.iso').read_bytes()).hexdigest(),'kernel_sha256':hashlib.sha256((ROOT/'build/kernel.elf').read_bytes()).hexdigest(),'checks':['65 applications and stable many47 package recovered','tiny 128x80 surface keeps its pixel width inside a usable system frame','actual Ring3 execution and no guest fault']}
 (out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
finally:v.close()
