#!/usr/bin/env python3
"""Inspect actual software cursor pixels from the production guest framebuffer."""
import hashlib,json,os,shutil,time
from PIL import Image
from fixtures import fresh_data_disk
from vm import ROOT,VM
from motion_vm_test import mouse
name='cursor-013';out=ROOT/('build/test-'+name);out.mkdir(exist_ok=True)
disk=out/'data.img';fresh_data_disk(disk)
iso=out/'tested.iso';shutil.copyfile(ROOT/'build/arkos-0.13.0.iso',iso)
os.environ.update(ARKOS_MACHINE='q35',ARKOS_SMP='4',ARKOS_ISO=str(iso))
kernel_hash=hashlib.sha256((ROOT/'build/kernel.elf').read_bytes()).hexdigest()
v=VM(name,disk,gpu='std',device='virtio-tablet-pci,virtio-keyboard-pci');checks=[]
def inspect(label,x,y,size,down=False):
 mouse(v,x,y,down);time.sleep(.4);v.screen(label);im=Image.open(out/(label+'.ppm')).convert('RGB');im.save(out/(label+'.png'))
 region=im.crop((x-17,y-17,x+34,y+34));points=[(i%51,i//51) for i,c in enumerate(region.get_flattened_data())if max(c)<70]
 assert points,label+' cursor disappeared'
 width=max(p[0] for p in points)-min(p[0] for p in points)+1;height=max(p[1] for p in points)-min(p[1] for p in points)+1
 assert abs(width-size[0])<=4 and abs(height-size[1])<=4,(label,width,height)
 checks.append({'shape':label,'framebuffer_outline':[width,height],'expected_extent':size})
try:
 v.enroll_test_user();inspect('01-arrow',1210,455,(17,27))
 inspect('02-title-grab',610,119,(21,26));inspect('03-title-grabbing',610,119,(21,19),True);mouse(v,610,119,False);time.sleep(.4)
 inspect('04-resize-ew',1041,390,(28,16));inspect('05-resize-diagonal',1041,639,(22,22))
 v.terminal();v.command('echo Cursor redraw');v.wait('Cursor redraw\n');inspect('06-text',905,475,(12,22))
 # Force another ordinary full presentation while the pointer remains still.
 v.command('echo Still visible');v.wait('Still visible\n');inspect('07-stationary-text',905,475,(12,22))
 v.key('f4');time.sleep(.7);inspect('08-button-pointer',202,300,(20,26))
 assert '[exception]' not in v.log.read_text() and 'User fault' not in v.log.read_text()
 result={'result':'PASS','iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'kernel_sha256':kernel_hash,'backend':'production BIOS boot framebuffer (-vga std), actual software cursor pixels','checks':checks};(out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
finally:v.close()
