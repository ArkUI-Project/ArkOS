"""Observed QEMU timings; compare unchanged machine/options, sequential runs.
Time includes emulation and the host, and is not a physical-machine promise.
"""
from pathlib import Path
import subprocess, time, selectors, json, statistics, sys
root=Path(__file__).resolve().parents[1];out=root/'build/test-boot-benchmark';out.mkdir(parents=True,exist_ok=True)
old=Path(sys.argv[1]) if len(sys.argv)>1 else root.parent/'arkos-v7/build/arkos-0.7.0.iso'
new=root/'build/arkos-0.9.0.iso'
results=[]
for run in range(3):
 for version,iso in [('0.7',old),('0.8',new)]:
  start=time.monotonic();p=subprocess.Popen(['qemu-system-x86_64','-machine','pc','-accel','tcg','-smp','1','-m','512M','-vga','std','-cdrom',str(iso),'-boot','d','-display','none','-serial','stdio','-monitor','none','-nic','none','-no-reboot'],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
  select=selectors.DefaultSelector();select.register(p.stdout,selectors.EVENT_READ);data='';marks={}
  try:
   while 'setup' not in marks:
    if time.monotonic()-start>40:raise AssertionError(data)
    if not select.select(.01):continue
    block=p.stdout.read(16384)
    if not block:raise AssertionError(p.stderr.read().decode())
    data+=block.decode(errors='replace');now=time.monotonic()-start
    for name,needle in [('entry','[boot] Multiboot2 entry'),('desktop','[arkos] desktop ready'),('setup','[session] setup ready')]:
     if name not in marks and needle in data:marks[name]=now
   marks['native_to_setup']=marks['setup']-marks['entry'];marks['version']=version;marks['run']=run;results.append(marks)
   (out/f'{version}-{run}.log').write_text(data)
  finally:p.terminate();p.wait(timeout=5);select.close()
summary={v:{k:round(statistics.median(x[k] for x in results if x['version']==v),4) for k in ['entry','desktop','setup','native_to_setup']} for v in ['0.7','0.8']}
(out/'results.json').write_text(json.dumps({'environment':'QEMU TCG pc, 1 CPU, 512 MiB, std VGA, no disk/network; 3 alternating runs','samples':results,'median_seconds':summary},indent=2));print(json.dumps(summary,indent=2))
