from pathlib import Path
import subprocess, time, selectors, json
root=Path(__file__).resolve().parents[1];out=root/'build/test-arm64';out.mkdir(parents=True,exist_ok=True)
results=[]
for cores in [1,4]:
 p=subprocess.Popen(['qemu-system-aarch64','-machine','virt,virtualization=off','-cpu','cortex-a72','-m','512M','-smp',str(cores),'-nographic','-monitor','none','-kernel',str(root/'build/arm64/arkos-arm64.elf')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,bufsize=0)
 text='';select=selectors.DefaultSelector();select.register(p.stdout,selectors.EVENT_READ)
 def wait(needle,timeout=15):
  global text
  end=time.monotonic()+timeout
  while needle not in text:
   if time.monotonic()>end:raise AssertionError(text)
   if select.select(.1):text+=p.stdout.read(4096).decode(errors='replace')
 try:
  wait('Serial shell ready');assert '[arm64] MMU + EL0 + SVC + W^X protection PASS' in text and 'FAIL' not in text;assert f'PSCI online CPUs={cores}' in text
  p.stdin.write('info\ncpus\necho 你好 ARM64\nbench\nuptime\npoweroff\n'.encode());p.stdin.flush()
  p.wait(timeout=15)
  while select.select(.01):
   data=p.stdout.read(4096)
   if not data:break
   text+=data.decode(errors='replace')
  assert '你好 ARM64' in text and 'Online CPUs: '+str(cores) in text and text.count('Parallel workers PASS')==2,text
  (out/f'{cores}-cores.log').write_text(text);results.append({'cores':cores,'result':'PASS','commands':['info','cpus','echo UTF-8','bench','uptime','poweroff']})
 finally:
  if p.poll() is None:p.terminate();p.wait(timeout=5)
  select.close()
(out/'results.json').write_text(json.dumps(results,indent=2));print('PASS ARM64 EL1 + FDT + PSCI (1 / 4 CPUs) + serial commands')
