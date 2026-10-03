from fixtures import fresh_data_disk
from vm import VM,ROOT
from PIL import Image
import os,time,sys,struct,zlib,json
os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4'
fw=sys.argv[1] if len(sys.argv)>1 else 'bios'
out=ROOT/'build'/('test-v8-capture-'+fw);out.mkdir(exist_ok=True)
disk=out/'data.img';fresh_data_disk(disk)
v=VM('v8-capture-'+fw,disk,firmware=fw,device='virtio-multitouch-pci,virtio-tablet-pci')
try:
 v.wait('[session] setup ready');v.type('Capture83!');v.key('tab');v.type('Capture83!');v.key('ret');v.wait('[session] desktop unlocked',60)
 v.key('ctrl-d');v.touch(62,90);v.touch(325,596,'update');v.touch(325,596,'end');v.wait('[desktop] Icon layout saved')
 v.q('input-send-event',{'events':[{'type':'abs','data':{'axis':'x','value':19210}},{'type':'abs','data':{'axis':'y','value':18455}},{'type':'btn','data':{'down':True,'button':'right'}}]});time.sleep(.1);v.q('input-send-event',{'events':[{'type':'btn','data':{'down':False,'button':'right'}}]});v.screen('01-context-menu');Image.open(out/'01-context-menu.ppm').save(out/'01-context-menu.png');v.key('esc')
 v.key('ctrl-p');v.wait('.bmp');v.key('ctrl-r');v.key('f4');time.sleep(.7);v.tap(300,418);time.sleep(.7);v.key('f1');v.command('uname');v.key('f4');v.tap(300,231);v.wait('.gif',30)
 v.screen('02-native-recording');Image.open(out/'02-native-recording.ppm').save(out/'02-native-recording.png')
 assert '[exception]' not in v.log.read_text()
finally:v.close()
raw=disk.read_bytes();indices=[]
for lba in [8192,8200]:
 index=raw[lba*512:(lba+8)*512]
 if index[:8]==b'ARKBLOB1' and zlib.crc32(index[:4092])==struct.unpack_from('<I',index,4092)[0]:indices.append((struct.unpack_from('<Q',index,8)[0],index))
assert indices
_,index=max(indices);files=[]
for slot in range(16):
 r=index[32+slot*96:32+(slot+1)*96];uid,start,size,crc=struct.unpack_from('<IIII',r)
 if not uid:continue
 assert uid==1000;name=r[24:88].split(b'\0',1)[0].decode();data=raw[start*512:start*512+size];assert zlib.crc32(data)==crc;(out/name).write_bytes(data);files.append(name)
 if name.endswith('.bmp'):im=Image.open(out/name);assert im.size==(1280,800);im.save(out/'native-screenshot.png')
 if name.endswith('.gif'):
  im=Image.open(out/name);assert im.size==(512,320) and im.n_frames>=10
  frames=[]
  for i in range(im.n_frames):im.seek(i);frames.append(im.convert('RGB').tobytes())
  assert len(set(frames))>3
assert any(n.endswith('.bmp') for n in files) and any(n.endswith('.gif') for n in files)
(out/'results.json').write_text(json.dumps({'result':'PASS','firmware':fw,'native_files':files,'runtime_host_bridge':False},indent=2)+'\n');print('PASS native capture, independent BMP/GIF decode, icon drag and context menu',fw)
