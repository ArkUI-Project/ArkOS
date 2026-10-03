from fixtures import fresh_data_disk
from vm import VM, ROOT
from pathlib import Path
import os,time,re,json,statistics
out=ROOT/'build/test-performance';out.mkdir(parents=True,exist_ok=True)
os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4'
versions=[('0.7',ROOT.parent/'arkos-v7/build/arkos-0.7.0.iso',11),('0.8',ROOT/'build/arkos-0.9.0.iso',11)]
records=[]
def cpu(pid):
 assert Path(f'/proc/{pid}/comm').read_text().startswith('qemu-system-'), 'CPU accounting requires the QEMU PID namespace'
 fields=Path(f'/proc/{pid}/stat').read_text().split();return (int(fields[13])+int(fields[14]))/os.sysconf('SC_CLK_TCK')
for version,iso,apps in versions*3:
 os.environ['ARKOS_ISO']=str(iso);disk=out/(version+'.img');fresh_data_disk(disk)
 v=VM('performance-'+version,disk,device='virtio-multitouch-pci,virtio-tablet-pci',gpu='vmware')
 try:
  v.wait('[session] setup ready');v.type('Native63!');v.key('tab');v.type('Native63!');v.key('ret');v.wait('[session] desktop unlocked',60);time.sleep(.3)
  start=len(v.log.read_text())
  for _ in range(3):v.key('f4');time.sleep(.65);v.key('esc');time.sleep(.65)
  segment=v.log.read_text()[start:];motion=[{'frames':int(a),'elapsed_ms':int(b)} for a,b in re.findall(r'end kind=window.*?frames=(\d+) elapsed_ms=(\d+)',segment)]
  assert len(motion)==6,segment
  for a,name in [(7,'Clock'),(8,'Paint'),(9,'Markdown')]:
   v.tap((1280-(apps*74+28))//2+42+a*74,740);v.wait('[app] '+name+' ring3 ready',20);time.sleep(.5)
  v.key('ctrl-d');v.tap(1160,380);time.sleep(1.5)
  t=time.monotonic();c=cpu(v.p.pid);time.sleep(4);elapsed=time.monotonic()-t;usage=100*(cpu(v.p.pid)-c)/elapsed
  record={'sample':len(records)//2,'version':version,'motion':motion,'median_motion_fps':statistics.median(x['frames']*1000/x['elapsed_ms'] for x in motion),'idle_three_apps_host_cpu_percent_one_core':usage,'idle_measure_seconds':elapsed};records.append(record)
  (out/(version+'-'+str(record['sample'])+'.log')).write_text(v.log.read_text());print(record,flush=True)
 finally:v.close()
(out/'results.json').write_text(json.dumps({'environment':'same QEMU TCG q35, 4 CPUs, 512 MiB, VMware SVGA II; three Settings open/close cycles, then hidden Clock/Paint/Markdown idle 4s with live wallpaper','runs':records},indent=2))
