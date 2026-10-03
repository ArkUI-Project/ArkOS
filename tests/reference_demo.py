#!/usr/bin/env python3
"""Record an uninterrupted 9.4s native guest demonstration of the source gestures.

Use after building the ISO, with the same QEMU environment as motion_vm_test.py.
No reference-video pixels are used. Timing CSV and guest serial logs accompany MP4.
"""
import hashlib,json,time
from pathlib import Path
from motion_vm_test import TimedCapture, mouse, key, click_actions, warmup
from fixtures import fresh_data_disk
from vm import VM,ROOT

out=ROOT/'build/test-reference-demo'
out.mkdir(parents=True,exist_ok=True)
disk=out/'demo-data.img';fresh_data_disk(disk)
vm=VM('reference-demo',disk=disk,device='virtio-tablet-pci,virtio-multitouch-pci')
try:
 warmup(vm,'vmware')
 # A full-size Settings sheet over Files mirrors the source's two-layer case.
 key(vm,'f4');time.sleep(1)
 mouse(vm,987,95,True);time.sleep(.08);mouse(vm,987,95,False);time.sleep(.8)
 mouse(vm,1144,78,True);time.sleep(.08);mouse(vm,1144,78,False);time.sleep(.7)
 mouse(vm,1080,600);time.sleep(.3)
 actions=[(2.90,'hover Settings Dock tile',lambda:mouse(vm,668,741))]
 actions+=click_actions(vm,668,741,3.433,'restore curved Settings sheet')
 actions.append((3.76,'move to window control',lambda:mouse(vm,1144,78)))
 actions+=click_actions(vm,1144,78,4.333,'minimize curved Settings sheet')
 actions += [(6.433,'show launcher',lambda:key(vm,'f5')),
             (7.500,'hide launcher',lambda:key(vm,'f5')),
             (8.333,'show launcher again',lambda:key(vm,'f5'))]
 result=TimedCapture(vm,30,True).run('demo',seconds=9.4,actions=actions,roi=(0,0,1280,800))
 metadata={'iso_sha256':hashlib.sha256((ROOT/'build/arkos-0.4.0.iso').read_bytes()).hexdigest(),
           'native_guest':True,'source_video_pixels_used':False,'cpu':'QEMU TCG','display':'VMware SVGA II','summary':result['summary']}
 (out/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
 print(out/'demo/capture.mp4')
finally:vm.close()
