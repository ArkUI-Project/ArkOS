# Historical 0.7 UI coordinates; current production coverage: v8_desktop_test.py.
from fixtures import fresh_data_disk
from vm import VM,ROOT
import os,time,struct,zlib,json,sys
from PIL import Image
os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4'
fw=sys.argv[1] if len(sys.argv)>1 else 'bios'
out=ROOT/'build'/('test-native-'+fw);out.mkdir(exist_ok=True);disk=out/'data.img';fresh_data_disk(disk)
v=VM('native-'+fw,disk,firmware=fw,device='virtio-multitouch-pci,virtio-tablet-pci,virtio-keyboard-pci')
def screen(name):v.screen(name);Image.open(out/(name+'.ppm')).save(out/(name+'.png'))
def app(name):v.key('f5');time.sleep(.3);v.type(name);v.key('ret');time.sleep(.6);v.wait('[ui] launcher open '+name)
def right(x,y):
 v.q('input-send-event',{'events':[{'type':'abs','data':{'axis':'x','value':round(x*32767/1279)}},{'type':'abs','data':{'axis':'y','value':round(y*32767/799)}},{'type':'btn','data':{'down':True,'button':'right'}}]});time.sleep(.12);v.q('input-send-event',{'events':[{'type':'btn','data':{'down':False,'button':'right'}}]});time.sleep(.3)
try:
 v.wait('[session] setup ready');v.type('Native73!');v.key('tab');v.type('Native73!');v.key('ret');v.wait('[session] desktop unlocked',60)
 assert '; native keyboard' in v.log.read_text()
 v.key('ctrl-d');v.touch(62,90);v.touch(325,596,'update');v.touch(325,596,'end');v.wait('[desktop] Icon layout saved');right(750,450);screen('01-context-menu');v.key('esc');v.key('ctrl-p');v.wait('.bmp',30)
 v.key('ctrl-r');time.sleep(1);v.key('ctrl-d');v.key('f3');time.sleep(.4);v.key('ctrl-i');v.type('zhongguo');v.key('spc');v.wait('[ime] Native Pinyin commit accepted');v.key('ctrl-i');v.key('ctrl-s');v.wait('.gif',30)
 app('Todo');v.type('Verify native hard disk boot');v.key('ret');screen('02-todo')
 app('Timer');v.tap(398,425);v.tap(400,363);time.sleep(1.2);screen('03-timer')
 v.key('f4');time.sleep(.5);screen('04-settings');v.tap(300,418);screen('05-permissions')
 # Clock initially starts, then native permission revocation terminates it.
 app('Clock');v.wait('[app] Clock ring3 ready');v.key('f4');time.sleep(.4);v.tap(300,418);time.sleep(.2);screen('06-permission-layout')
 # Coordinates selected from the fixed 850x570 ArkUI settings layout.
 v.tap(989,276);v.wait('[process] Exit pid=2 status=0xffffffffffffffff');time.sleep(.4);screen('07-permission-revoked')
 app('Capture');screen('08-native-capture-files')
 assert '[exception]' not in v.log.read_text() and 'User fault pid=1' not in v.log.read_text()
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
(out/'results.json').write_text(json.dumps({'result':'PASS','firmware':fw,'native_files':files,'cpus':4,'input':'VirtIO keyboard + multitouch + tablet','host_runtime_services':False},indent=2));print('PASS native keyboard, drag, right click, offline Pinyin, BMP/GIF on disk and independent decoding',fw)
