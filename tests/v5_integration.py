"""Full release guest: real HTTP UI, native SDK save/reboot, FAT/NTFS, motion."""
from fixtures import fresh_data_disk, fresh_compat_disk
from vm import VM,ROOT
from motion_vm_test import mouse,key,TimedCapture,click_actions
from pathlib import Path
import time,threading,http.server,hashlib,json
out=ROOT/'build/test-v5-integration';out.mkdir(parents=True,exist_ok=True)
disk=out/'data.img';fresh_data_disk(disk)
external=out/'external.img';fresh_compat_disk(external)
requests=[]
body='ArkOS native network works.\n中文网络正文已通过原生 TCP/HTTP 到达用户态。\nNo Linux kernel. No Chromium claim.\n'.encode()
class Handler(http.server.BaseHTTPRequestHandler):
 def do_GET(self):
  requests.append(self.path);self.send_response(200);self.send_header('Content-Type','text/plain; charset=utf-8');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
 def log_message(self,*args):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',8080),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
v=VM('v5-integration',disk,device='virtio-tablet-pci,virtio-multitouch-pci',external=external if external.exists() else None)
try:
 v.wait('[session] setup ready');v.type('Native53!');v.key('tab');v.type('Native53!');v.key('ret');v.wait('[session] desktop unlocked',60)
 mouse(v,1150,500);time.sleep(.3);v.screen('01-files')
 # Stable cached window transition + launcher under the actual release kernel.
 capture=TimedCapture(v,30,True)
 demo=capture.run('motion-demo',seconds=8.8,actions=click_actions(v,920,118,.5,'minimize Files')+click_actions(v,372,740,1.5,'restore Files')+[(2.1,'leave Dock',lambda:mouse(v,1170,500)),(2.8,'launcher open',lambda:key(v,'f5')),(3.8,'launcher close',lambda:key(v,'f5'))]+click_actions(v,816,741,4.5,'launch Clock')+[(5.0,'leave Dock',lambda:mouse(v,1150,500)),(6.3,'launcher open',lambda:key(v,'f5')),(7.3,'launcher close',lambda:key(v,'f5'))],roi=(0,0,1280,800))
 # Browser is Dock index6, active URL receives Enter only on explicit action.
 mouse(v,742,740,True);mouse(v,742,740,False);time.sleep(.6);v.key('ret')
 deadline=time.monotonic()+8
 while not requests and time.monotonic()<deadline:time.sleep(.05)
 assert requests==['/'],requests
 time.sleep(.5);mouse(v,1140,490);v.screen('02-http-browser')
 # Paint receives real pointer input through a copied kernel surface/event queue.
 mouse(v,890,740,True);mouse(v,890,740,False);v.wait('[app] Paint ring3 ready');time.sleep(.5)
 mouse(v,340,340,True)
 for x,y in [(390,325),(445,370),(500,315),(560,370),(620,335)]:mouse(v,x,y,True);time.sleep(.05)
 mouse(v,620,335,False);v.key('s');time.sleep(.5);mouse(v,1130,500);v.screen('03-paint-saved')
 v.terminal();v.command('cat Paint.svg');v.wait('<path stroke=',15);v.screen('04-terminal-svg')
 # Read external volumes through authenticated file service; errors may be explicit read-only.
 if external.exists():
  v.command('ls /mnt/fat32');v.command('ls /mnt/ntfs');v.screen('05-external-volumes')
 results={'native_release':True,'iso_sha256':hashlib.sha256((ROOT/'build/arkos-0.5.0.iso').read_bytes()).hexdigest(),'http_requests':requests,'http_body_bytes':len(body),'http_body_sha256':hashlib.sha256(body).hexdigest(),'paint_svg_saved_and_read':True,'motion_summary':demo['summary']}
 (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
 print(json.dumps(results,indent=2))
finally:v.close();server.shutdown();server.server_close()
# Same physical disk, new VM, real mount + password verifier, no auto-login.
v=VM('v5-persistence',disk,device='virtio-tablet-pci')
try:
 v.wait('[session] login ready');v.type('Native53!');v.key('ret');v.wait('[session] desktop unlocked',60);v.terminal();v.command('cat Paint.svg');v.wait('<path stroke=',15);v.screen('persisted-svg');print('PERSISTENCE PASS')
 results['reboot_password_and_paint_persisted']=True;(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
finally:v.close()
