from fixtures import fresh_data_disk
from vm import VM,ROOT
import time,json,sys
firmware=sys.argv[1] if len(sys.argv)>1 else 'bios'
gpu=sys.argv[2] if len(sys.argv)>2 else 'vmware'
disk=ROOT/'build'/('v5-smoke-'+firmware+'-'+gpu+'.img')
fresh_data_disk(disk)
v=VM('v5-smoke-'+firmware+'-'+gpu,disk,firmware=firmware,gpu=gpu,device='virtio-multitouch-pci,virtio-tablet-pci')
try:
 time.sleep(1.2);v.screen('01-setup')
 v.type('Native53!');v.key('tab');v.type('Native53!');v.key('ret')
 v.wait('[session] desktop unlocked',60);time.sleep(.4);v.screen('02-desktop')
 v.key('f5');time.sleep(.5);v.screen('03-launcher');v.key('esc');time.sleep(.4)
 # Dock centered at x256, firsticon x273. 10 apps spaced74.
 for a,name in [(7,'Clock'),(8,'Paint'),(9,'Markdown')]:
  x=256+42+a*74;v.tap(x,740);v.wait('[app] '+name+' ring3 ready',20);time.sleep(.5);v.screen('04-'+name.lower())
 v.key('ctrl-k');time.sleep(.4);v.screen('05-lock')
 v.type('Wrong53!');v.key('ret');time.sleep(.8);v.screen('06-wrong-password')
 v.type('Native53!');v.key('ret');v.wait('[session] desktop unlocked',60,after=v.log.read_text().find('[session] desktop unlocked')+1);time.sleep(.3);v.screen('07-unlocked')
 print(v.log.read_text());print('SMOKE PASS',firmware)
finally:v.close()
