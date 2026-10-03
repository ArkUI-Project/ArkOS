#!/usr/bin/env python3
"""Real e1000 DMA + QEMU slirp DHCP/DNS/HTTP, with controlled local fixtures.
Only this diagnostic overrides its resolver to 10.0.2.2 (UDP5353); release uses DHCP.
Does not modify release ISO/disks. Captures actual Ethernet pcap for each boot.
"""
from pathlib import Path
import os,subprocess,time,json,socket,threading,struct,hashlib
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
ROOT=Path(__file__).resolve().parents[1]; OUT=ROOT/'build/test-net';OUT.mkdir(parents=True,exist_ok=True)
TOOL=ROOT.parent/'toolroot/usr';env=os.environ.copy();env['PATH']=str(TOOL/'bin')+':'+env['PATH'];env['LD_LIBRARY_PATH']=str(TOOL/'lib/x86_64-linux-gnu');env['QEMU_MODULE_DIR']=str(TOOL/'lib/x86_64-linux-gnu/qemu')
PLAIN='ArkOS native HTTP: real DMA packets.\nChinese: 你好，网络。\n'.encode()
requests=[];dns_requests=[]
class Fixture(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  requests.append(self.path)
  if self.path=='/oversize':
   self.send_response(200);self.send_header('Content-Length','16385');self.end_headers();return
  if self.path=='/chunked':
   self.send_response(200);self.send_header('Transfer-Encoding','chunked');self.send_header('Content-Type','text/plain; charset=utf-8');self.end_headers()
   for part in [b'Chunked ',b'response: ','你好\n'.encode()]:
    self.wfile.write(('%x;fixture=yes\r\n'%len(part)).encode()+part+b'\r\n');self.wfile.flush();time.sleep(.02)
   self.wfile.write(b'0\r\nX-Fixture: passed\r\n\r\n');return
  body=PLAIN if self.path=='/plain' else b'Controlled missing page\n'
  self.send_response(200 if self.path=='/plain' else 404);self.send_header('Content-Type','text/plain; charset=utf-8');self.send_header('Content-Length',str(len(body)));self.end_headers()
  for at in range(0,len(body),7):self.wfile.write(body[at:at+7]);self.wfile.flush();time.sleep(.006)
http=ThreadingHTTPServer(('127.0.0.1',8080),Fixture);threading.Thread(target=http.serve_forever,daemon=True).start()
dns=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);dns.bind(('127.0.0.1',5353));dns.settimeout(.2);stop=False
def dns_loop():
 while not stop:
  try:data,addr=dns.recvfrom(1024)
  except socket.timeout:continue
  at=12;parts=[]
  while at<len(data) and data[at]:
   size=data[at];at+=1;parts.append(data[at:at+size].decode());at+=size
  at+=5;dns_requests.append('.'.join(parts))
  if dns_requests[-1]=='arkos.test':dns.sendto(data[:2]+struct.pack('!5H',0x8180,1,1,0,0)+data[12:at]+b'\xc0\x0c'+struct.pack('!HHIH',1,1,60,4)+bytes([10,0,2,2]),addr)
threading.Thread(target=dns_loop,daemon=True).start()
flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pie','-mno-red-zone','-mgeneral-regs-only','-mcmodel=small','-I'+str(ROOT/'include')]
objects=[]
for source in ['tests/net_fixture.c','kernel/e1000.c','kernel/mmio.c','kernel/platform.c','kernel/lib.c','boot/entry.S','kernel/interrupts.S']:
 obj=OUT/(Path(source).stem+'.o');subprocess.run(['gcc',*flags,'-c',str(ROOT/source),'-o',str(obj)],check=True,env=env);objects.append(str(obj))
tree=OUT/'iso';(tree/'boot/grub').mkdir(parents=True,exist_ok=True);elf=tree/'boot/kernel.elf'
subprocess.run(['ld','-nostdlib','-z','max-page-size=0x1000','-T',str(ROOT/'boot/linker.ld'),*objects,'-o',str(elf)],check=True)
(tree/'boot/grub/grub.cfg').write_text('set timeout=0\nset default=0\ninsmod all_video\nset gfxmode=1024x768x32\nset gfxpayload=keep\nmenuentry "Native network diagnostic" {\n multiboot2 /boot/kernel.elf\n boot\n}\n')
iso=OUT/'net-diagnostic.iso'
with (OUT/'build.log').open('w') as log:subprocess.run(['grub-mkrescue','-o',str(iso),str(tree)],stdout=log,stderr=log,check=True,env=env)
def check_capture(path):
 data=path.read_bytes();assert data[:4]==b'\xd4\xc3\xb2\xa1', 'pcap little endian format'
 at=24;counts={'arp':0,'ipv4':0,'udp':0,'tcp':0}
 def checksum(data):
  if len(data)%2:data+=b'\0'
  total=sum(struct.unpack('!%dH'%(len(data)//2),data))
  while total>>16:total=(total&65535)+(total>>16)
  return (~total)&65535
 while at<len(data):
  _,_,length,_=struct.unpack_from('<4I',data,at);at+=16;frame=data[at:at+length];at+=length
  if len(frame)<14:continue
  kind=struct.unpack_from('!H',frame,12)[0]
  if kind==0x806:counts['arp']+=1
  if kind!=0x800:continue
  counts['ipv4']+=1;ip=frame[14:];header=(ip[0]&15)*4;size=struct.unpack_from('!H',ip,2)[0]
  assert checksum(ip[:header])==0,'IPv4 checksum'
  protocol=ip[9];body=ip[header:size]
  if protocol in (6,17):
   counts['tcp' if protocol==6 else 'udp']+=1
   if protocol==17:body=body[:struct.unpack_from('!H',body,4)[0]]
   if protocol==6 or struct.unpack_from('!H',body,6)[0]:assert checksum(ip[12:20]+bytes([0,protocol])+struct.pack('!H',len(body))+body)==0,'transport checksum'
 assert all(counts.values()),counts
 return counts
results=[]
try:
 for firmware in ['bios','uefi']:
  serial=OUT/(firmware+'.log');serial.unlink(missing_ok=True);pcap=OUT/(firmware+'.pcap')
  qemu=os.environ.get('QEMU_BIN',str(TOOL/'bin/qemu-system-x86_64'))
  cmd=[qemu,'-L',str(TOOL/'share/qemu'),'-machine','pc','-accel','tcg','-m','256M','-cdrom',str(iso),'-boot','d','-vga','std','-netdev','user,id=net0','-device','e1000,netdev=net0,romfile=','-object','filter-dump,id=capture,netdev=net0,file='+str(pcap),'-display','none','-serial','file:'+str(serial),'-no-reboot']
  if firmware=='uefi':cmd+=['-drive','if=pflash,format=raw,readonly=on,file='+os.environ.get('OVMF_CODE',str(TOOL/'share/OVMF/OVMF_CODE_4M.fd'))]
  with (OUT/(firmware+'-qemu.log')).open('w') as log:
   p=subprocess.Popen(cmd,stdout=log,stderr=log,env=env)
   try:
    deadline=time.monotonic()+50
    while True:
     text=serial.read_text(errors='replace') if serial.exists() else ''
     if '[net-test] FAIL' in text:raise AssertionError(text)
     if '[net-test] PASS' in text:break
     if p.poll() is not None or time.monotonic()>deadline:raise RuntimeError(firmware+' timeout\n'+text)
     time.sleep(.05)
    result=text.split('[net-test] PASS ')[1].splitlines()[0];print('PASS '+firmware+': '+result,flush=True)
    results.append({'firmware':firmware,'result':result,'pcap_bytes':pcap.stat().st_size})
   finally:p.terminate();p.wait(timeout=5)
  results[-1]['wire_checksums']=check_capture(pcap)
finally:stop=True;http.shutdown();dns.close()
assert dns_requests==['arkos.test']*2,dns_requests
(OUT/'results.json').write_text(json.dumps({'kernel_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'body_sha256':hashlib.sha256(PLAIN).hexdigest(),'http_requests':requests,'dns_requests':dns_requests,'results':results},indent=2)+'\n')
