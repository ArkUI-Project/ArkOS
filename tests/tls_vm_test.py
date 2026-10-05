#!/usr/bin/env python3
"""Native TLS over real guest E1000. Private CA/resolver belong only to this diagnostic."""
from pathlib import Path
import hashlib,json,os,socket,ssl,struct,subprocess,threading,time
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/test-tls';OUT.mkdir(parents=True,exist_ok=True)
CC=os.environ.get('ARK_CC','gcc');LD=os.environ.get('ARK_LD','ld');OPENSSL=os.environ.get('OPENSSL','openssl')
def run(args,**kw):return subprocess.run(list(map(str,args)),check=True,**kw)
log=(OUT/'build.log').open('w')
def cert(args):run([OPENSSL,*args],stdout=log,stderr=log)
cert(['req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:P-256','-nodes','-keyout',OUT/'ca.key','-out',OUT/'ca.pem','-days','30','-subj','/CN=ArkOS diagnostic CA','-addext','basicConstraints=critical,CA:TRUE','-addext','keyUsage=critical,keyCertSign,cRLSign'])
for name,host in [('good','arkos.test'),('wrong','wrong.test'),('expired','arkos.test'),('untrusted','arkos.test')]:
 cert(['req','-newkey','ec','-pkeyopt','ec_paramgen_curve:P-256','-nodes','-keyout',OUT/(name+'.key'),'-out',OUT/(name+'.csr'),'-subj','/CN='+host])
 ext=OUT/(name+'.ext');ext.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:'+host+'\n')
 if name=='untrusted':cert(['x509','-req','-in',OUT/(name+'.csr'),'-signkey',OUT/(name+'.key'),'-out',OUT/(name+'.pem'),'-days','7','-extfile',ext])
 elif name=='expired':
  (OUT/'index.txt').write_text('');(OUT/'serial').write_text('01\n');(OUT/'ca.cnf').write_text('[ca]\ndefault_ca=test\n[test]\ndatabase='+str(OUT/'index.txt')+'\nserial='+str(OUT/'serial')+'\nnew_certs_dir='+str(OUT)+'\ncertificate='+str(OUT/'ca.pem')+'\nprivate_key='+str(OUT/'ca.key')+'\ndefault_md=sha256\npolicy=policy\n[policy]\ncommonName=supplied\n')
  cert(['ca','-batch','-config',OUT/'ca.cnf','-in',OUT/(name+'.csr'),'-out',OUT/(name+'.pem'),'-startdate','20200101000000Z','-enddate','20200102000000Z','-extfile',ext])
 else:cert(['x509','-req','-in',OUT/(name+'.csr'),'-CA',OUT/'ca.pem','-CAkey',OUT/'ca.key','-CAcreateserial','-out',OUT/(name+'.pem'),'-days','7','-extfile',ext])
brssl=OUT/'bear-host/brssl';run(['make','-C',ROOT/'third_party/bearssl','-j4','CC='+os.environ.get('HOST_CC','cc'),'LD='+os.environ.get('HOST_CC','cc'),'AR=ar','BUILD='+str(brssl.parent),'tools'],stdout=log,stderr=log)
with (OUT/'anchors.h').open('w') as f:run([brssl,'ta',OUT/'ca.pem'],stdout=f,stderr=log)
flags=['-std=c11','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pie','-mno-red-zone','-mgeneral-regs-only','-mcmodel=small','-I'+str(ROOT/'include')]
objects=[]
for source in ['tests/tls_fixture.c','kernel/tls.c']:
 obj=OUT/(Path(source).stem+'.o');extra=['-DARK_TLS_TEST_ANCHORS="'+str(OUT/'anchors.h')+'"'] if source.endswith('/tls.c') else []
 run([CC,*flags,*extra,'-c',ROOT/source,'-o',obj],stdout=log,stderr=log);objects.append(obj)
objects += [ROOT/'build'/(s+'.o') for s in ['e1000','mmio','platform','lib','random','entry','interrupts']]
objects += sorted((ROOT/'build/bearssl').rglob('*.o'))
tree=OUT/'iso';(tree/'boot/grub').mkdir(parents=True,exist_ok=True);elf=tree/'boot/kernel.elf'
run([LD,'-nostdlib','--gc-sections','-z','noexecstack','-z','max-page-size=0x1000','-T',ROOT/'boot/linker.ld',*objects,'-o',elf],stdout=log,stderr=log)
(tree/'boot/grub/grub.cfg').write_text('set timeout=0\ninsmod all_video\nset gfxmode=1024x768x32\nset gfxpayload=keep\nmenuentry "TLS diagnostic" {\n multiboot2 /boot/kernel.elf\n boot\n}\n')
iso=OUT/'tls-diagnostic.iso';run(['python3',str(ROOT/'scripts/mkiso.py'),tree,iso],stdout=log,stderr=log);log.close()
requests=[];servers=[]
class Fixture(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  requests.append([self.server.server_port,self.path]);body=b'Native TLS authenticated body\n';self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
def server(port,name=None):
 s=ThreadingHTTPServer(('127.0.0.1',port),Fixture)
 if name:
  c=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);c.minimum_version=c.maximum_version=ssl.TLSVersion.TLSv1_2;c.set_ciphers('ECDHE-ECDSA-AES128-GCM-SHA256');c.load_cert_chain(OUT/(name+'.pem'),OUT/(name+'.key'));s.socket=c.wrap_socket(s.socket,server_side=True)
 servers.append(s);threading.Thread(target=s.serve_forever,daemon=True).start()
for port,name in [(8080,None),(8443,'good'),(8444,'wrong'),(8445,'expired'),(8446,'untrusted')]:server(port,name)
stop=False;mutations=[];proxy=socket.socket();proxy.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);proxy.bind(('127.0.0.1',8447));proxy.listen();proxy.settimeout(.2)
def relay(a,b):
 try:
  while data:=a.recv(65536):b.sendall(data)
 except OSError:pass
 finally:
  try:b.shutdown(socket.SHUT_WR)
  except OSError:pass
def exact(s,n):
 b=b''
 while len(b)<n:
  part=s.recv(n-len(b))
  if not part:raise EOFError()
  b+=part
 return b
def proxy_loop():
 while not stop:
  try:client,_=proxy.accept()
  except socket.timeout:continue
  except OSError:break
  backend=socket.create_connection(('127.0.0.1',8443));threading.Thread(target=relay,args=(client,backend),daemon=True).start()
  try:
   changed=False
   while True:
    head=exact(backend,5);record=bytearray(exact(backend,int.from_bytes(head[3:5],'big')))
    if head[0]==23 and not changed:record[-1]^=1;changed=True;mutations.append(True)
    client.sendall(head+record)
  except (OSError,EOFError):pass
  finally:client.close();backend.close()
threading.Thread(target=proxy_loop,daemon=True).start()
dns=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);dns.bind(('127.0.0.1',55353));dns.settimeout(.2)
def dns_loop():
 while not stop:
  try:data,addr=dns.recvfrom(1024)
  except socket.timeout:continue
  except OSError:break
  at=12
  while at<len(data) and data[at]:at+=data[at]+1
  at+=5;dns.sendto(data[:2]+struct.pack('!5H',0x8180,1,1,0,0)+data[12:at]+b'\xc0\x0c'+struct.pack('!HHIH',1,1,60,4)+bytes([10,0,2,2]),addr)
threading.Thread(target=dns_loop,daemon=True).start();results=[]
try:
 for firmware in ['bios','uefi']:
  serial=OUT/(firmware+'.log');serial.write_text('');pcap=OUT/(firmware+'.pcap');err=(OUT/(firmware+'-qemu.log')).open('w')
  args=[os.environ.get('QEMU_BIN','qemu-system-x86_64'),'-machine','pc','-cpu','max','-accel','tcg','-m','256M','-vga','std','-cdrom',iso,'-boot','d','-netdev','user,id=net0','-device','e1000,netdev=net0,romfile=','-object','filter-dump,id=capture,netdev=net0,file='+str(pcap),'-display','none','-serial','file:'+str(serial),'-no-reboot']
  if firmware=='uefi':args+=['-drive','if=pflash,format=raw,readonly=on,file='+os.environ.get('OVMF_CODE','/usr/share/OVMF/OVMF_CODE_4M.fd')]
  p=subprocess.Popen(list(map(str,args)),stdout=err,stderr=err)
  try:
   until=time.monotonic()+90
   while True:
    text=serial.read_text()
    if '[tls-test] FAIL' in text:raise AssertionError(text)
    if '[tls-test] PASS' in text:break
    if p.poll() is not None or time.monotonic()>until:raise TimeoutError(text)
    time.sleep(.1)
   results.append({'firmware':firmware,'result':'PASS','pcap_sha256':hashlib.sha256(pcap.read_bytes()).hexdigest()});print('PASS '+firmware+': native TLS encrypted body and four rejection paths',flush=True)
  finally:p.terminate();p.wait(timeout=5);err.close()
  data=pcap.read_bytes();at=24;encrypted_bytes=0
  while at+16<=len(data):
   length=struct.unpack_from('<4I',data,at)[2];at+=16;frame=data[at:at+length];at+=length
   if len(frame)<54 or frame[12:14]!=b'\x08\x00':continue
   ip=frame[14:];h=(ip[0]&15)*4
   if ip[9]!=6:continue
   tcp=ip[h:];ports=struct.unpack_from('!HH',tcp)
   if not any(8443<=x<=8447 for x in ports):continue
   payload=tcp[(tcp[12]>>4)*4:];assert b'Native TLS authenticated body' not in payload;encrypted_bytes+=len(payload)
  assert encrypted_bytes>2000;results[-1]['encrypted_tcp_bytes']=encrypted_bytes
 assert len(mutations)==2 and not any(port in (8444,8445,8446) for port,_ in requests),requests
 (OUT/'results.json').write_text(json.dumps({'kernel_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'private_test_ca':True,'requests':requests,'tampered_records':len(mutations),'results':results},indent=2)+'\n')
finally:
 stop=True;dns.close();proxy.close()
 for s in servers:s.shutdown();s.server_close()
