"""Real native GUI smoke and persistent settings test via absolute VirtIO mouse."""
import os,sys,time,json,subprocess
from pathlib import Path
from PIL import Image
from vm import VM,ROOT
from fixtures import fresh_compat_disk

def mouse(v,x,y,down=None):
 events=[{'type':'abs','data':{'axis':'x','value':round(x*32767/1279)}},{'type':'abs','data':{'axis':'y','value':round(y*32767/799)}}]
 if down is not None:events.append({'type':'btn','data':{'button':'left','down':down}})
 v.q('input-send-event',{'events':events});time.sleep(.18)
def click(v,x,y):
 mouse(v,x,y,True);mouse(v,x,y,False);time.sleep(.45)
def screen(v,name):
 time.sleep(.5);v.screen(name);Image.open(v.out/(name+'.ppm')).save(v.out/(name+'.png'));print(name,flush=True)

def main():
 work=ROOT/'build/ui-v3';work.mkdir(exist_ok=True)
 disk=work/'data.img'
 if disk.exists():disk.unlink()
 subprocess.run([sys.executable,str(ROOT/'scripts/create-disk.py'),str(disk)],check=True)
 external=work/'compat.img';fresh_compat_disk(external)
 gpu=os.environ.get('ARKOS_TEST_GPU','std')
 v=VM('v3-settings-'+gpu,disk=disk,external=external,gpu=gpu,device='virtio-multitouch-pci,virtio-tablet-pci')
 try:
  time.sleep(2);v.key('f4');screen(v,'appearance')
  # Toggle a non-default theme and choose the second wallpaper.
  click(v,884,266);screen(v,'dark')
  click(v,580,266);click(v,885,544)
  click(v,1020,364)
  for index,name in [(1,'input'),(2,'display'),(3,'storage'),(4,'system')]:
   click(v,310,195+43*index);screen(v,name)
   if index==1:
    click(v,1020,260);click(v,1020,326);screen(v,'input-large-pointer')
   if index==4:click(v,1020,260)
  # Maximize and restore; controls use the new right-aligned title bar.
  click(v,987,94);screen(v,'maximized')
  click(v,1188,78);screen(v,'restored')
  click(v,944,95);screen(v,'minimized')
  v.key('f4');screen(v,'reopened')
  v.terminal();v.command('cat /home/ark/.arkcfg');time.sleep(1);screen(v,'settings-file')
  log=v.log.read_text();assert 'wall=1' in log and 'pointer_scale=2' in log and 'utc_offset=14' in log,log
  v.command('sync');time.sleep(.7);v.command('shutdown');v.p.wait(timeout=15)
 finally:v.close()
 v=VM('v3-settings-reboot-'+gpu,disk=disk,external=external,gpu=gpu,device='virtio-multitouch-pci,virtio-tablet-pci')
 try:
  v.key('f4');screen(v,'appearance-persisted');v.terminal();v.command('cat /home/ark/.arkcfg');time.sleep(1)
  log=v.log.read_text();assert 'wall=1' in log and 'pointer_scale=2' in log and 'utc_offset=14' in log,log
  screen(v,'settings-persisted');(work/'result.json').write_text(json.dumps({'gpu':gpu,'settings_reboot':True,'window_controls':'screenshots'},indent=2));print('PASS persistent native settings',flush=True)
 finally:v.close()
if __name__=='__main__':main()
