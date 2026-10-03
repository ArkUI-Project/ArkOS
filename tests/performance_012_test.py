#!/usr/bin/env python3
"""Comparable native QEMU motion timing, one VM at a time and no frame capture
inside timed animations. Serial frame counts measure completed guest presents."""
import hashlib,json,os,re,sys,time
from pathlib import Path
from fixtures import fresh_data_disk
from vm import ROOT,VM
from motion_vm_test import mouse
candidate=len(sys.argv)>1 and sys.argv[1]=='--candidate'
label=sys.argv[2] if len(sys.argv)>2 else 'parallel'
assert re.fullmatch(r'[a-z0-9-]+',label)
OUT=ROOT/('build/test-performance-012-'+label if candidate else 'build/test-performance-012');OUT.mkdir(exist_ok=True)
os.environ.update(ARKOS_MACHINE='q35',ARKOS_SMP='4')
images=[('011',ROOT/'build/release/0.11.0-final/ArkOS-0.11.0-dev/arkos-0.11.0.iso'),('012',ROOT/'build/arkos-0.12.0.iso')]
if candidate:images=images[1:]
results=[]
for version,iso in images:
 disk=OUT/(version+'.img');fresh_data_disk(disk);os.environ['ARKOS_ISO']=str(iso);v=VM('performance-'+version+('-'+label if candidate else ''),disk,device='virtio-multitouch-pci,virtio-tablet-pci,virtio-keyboard-pci')
 try:
  v.enroll_test_user();v.screen('warmup');time.sleep(3)
  for repeat in range(4):
   mouse(v,920,119,True);time.sleep(.08);mouse(v,920,119,False);time.sleep(1)
   v.key('f2');time.sleep(1)
  text=v.log.read_text();records=[]
  for line in text.splitlines():
   if '[motion] end kind=window' not in line:continue
   q={k:int(val) for k,val in re.findall(r'(\w+)=(\d+)',line)};q['fps']=round(q['frames']*1000/q['elapsed_ms'],2);records.append(q)
  assert len(records)==8 and all(r['frames']>1 for r in records)
  results.append({'version':version,'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'kernel_kind':'shipped 0.11 cached backdrop' if version=='011' else '0.12 live backdrop with identical material optimization','records':records,'average_fps':round(sum(q['fps'] for q in records)/len(records),2)})
  print(json.dumps(results[-1]),flush=True)
 finally:v.close()
(OUT/'results.json').write_text(json.dumps({'platform':'QEMU TCG, q35, max, 4 CPUs, 512 MiB, VMware SVGA, 1280x800, one VM at a time','scenario':'Files minimize/restore, 4 cycles; warm cache; no timed screenshot traffic','results':results},indent=2)+'\n')
