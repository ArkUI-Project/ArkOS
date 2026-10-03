"""Production ISO: native WASM, real disk reads, keyboard punctuation and traps."""
from fixtures import fresh_data_disk, fresh_compat_disk
from vm import VM,ROOT
from PIL import Image
import subprocess,os,sys,time,json
os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4'
fw=sys.argv[1] if len(sys.argv)>1 else 'bios'
out=ROOT/'build'/('test-v9-wasm-'+fw);out.mkdir(parents=True,exist_ok=True)
disk=out/'data.img';external=out/'compat.img';fresh_data_disk(disk);fresh_compat_disk(external)
# Ordinary files placed on FAT32 before power-on; ArkOS loads every byte itself.
for src in (ROOT/'examples/wasm').glob('*.wasm'):
 subprocess.run(['mcopy','-o','-i',str(external)+'@@1048576',str(src),'::/'+src.name],check=True)
# Read several syscall chunks and skip a legal custom section (> old text limit).
large=(ROOT/'examples/wasm/hello.wasm').read_bytes();payload=b'\0'+bytes(40000);size=len(payload);enc=[]
while True:
 b=size&127;size>>=7;enc.append(b|(128 if size else 0))
 if not size:break
(out/'large.wasm').write_bytes(large+b'\0'+bytes(enc)+payload)
subprocess.run(['mcopy','-o','-i',str(external)+'@@1048576',str(out/'large.wasm'),'::/large.wasm'],check=True)
devices='virtio-multitouch-pci,virtio-tablet-pci'+(',virtio-keyboard-pci' if fw=='bios' else '')
v=VM('v9-wasm-'+fw,disk,external=external,firmware=fw,device=devices)
def screen(name):v.screen(name);Image.open(out/(name+'.ppm')).save(out/(name+'.png'))
def run(target,expected):
 v.terminal();start=len(v.log.read_text());v.command('wasm '+target);v.wait(expected,after=start,timeout=40)
try:
 v.wait('[session] setup ready');v.type('Wasm929!');v.key('tab');v.type('Wasm929!');v.key('ret');v.wait('[session] desktop unlocked',60)
 v.terminal();v.command('echo "http://ark:8080;a:b;c" > punctuation.txt');v.command('cat punctuation.txt');v.wait('http://ark:8080;a:b;c')
 # Right Shift and simultaneous left+right Shift produce a colon too.
 v.type('echo "');v.key('shift_r-semicolon');v.key('shift-shift_r-semicolon');v.type('"');v.key('ret');v.wait('::\n')
 run('builtin:hello','[wasm] completed builtin:hello');v.wait('result=42');screen('01-native-wasm')
 for name,res in [('fibonacci',55),('memory',125),('float',4),('wasi',len('Hello from native ArkOS WASM! 中国 : ;\n'.encode()))]:
  run('builtin:'+name,'result='+str(res))
 run('builtin:out_of_bounds','[wasm] trap: [trap] out of bounds memory access')
 run('builtin:loop','[wasm] trap: [trap] out of gas');screen('02-native-trap')
 run('/mnt/fat32/hello.wasm','[permission] Consent requested by wasm');v.key('esc');v.wait('[permission] Denied wasm');time.sleep(.5);assert '[wasm] completed /mnt/fat32/hello.wasm' not in v.log.read_text()
 # Fresh process forgets denial; persistent grants remain explicitly controlled.
 v.key('ctrl-w');time.sleep(1.2)
 run('/mnt/fat32/hello.wasm','[permission] Consent requested by wasm');v.key('ret');v.wait('[permission] Allowed wasm');v.wait('[wasm] completed /mnt/fat32/hello.wasm')
 run('/mnt/fat32/large.wasm','[wasm] completed /mnt/fat32/large.wasm')
 for name,res in [('fibonacci',55),('global',6),('table',9),('sqrt',9),('round',2),('grow_limit',-1)]:run('/mnt/fat32/'+name+'.wasm','result='+str(res))
 for name,error in [('invalid_type','incorrect type'),('truncated','malformed'),('bad_import','unsupported import'),('bad_pointer','out of bounds'),('recursion','stack overflow'),('divide_zero','divide by zero')]:run('/mnt/fat32/'+name+'.wasm',error)
 run('/mnt/fat32/wasi_exit.wasm','exit=7')
 run('builtin:hello','[wasm] completed builtin:hello');v.key('ctrl-s');time.sleep(.4);run('blob:hello.wasm','[wasm] completed blob:hello.wasm');screen('03-disk-wasm')
 v.terminal();v.command('blobs');v.wait('hello.wasm');v.command('ps');v.wait('wasm');screen('04-native-processes')
 # Touch-only symbol entry, including the previously unavailable ':' and ';'.
 v.type('echo "');v.tap(1020,18);time.sleep(.4);v.tap(910,553);time.sleep(.2)
 v.tap(350,674);v.tap(270,674);v.tap(995,553);v.type('"');v.key('ret');v.wait(':;\n');screen('05-touch-punctuation')
 log=v.log.read_text();assert '[exception]' not in log and 'User fault' not in log,log[-3000:]
finally:v.close()
v=VM('v9-persist-'+fw,disk,external=external,firmware=fw,device=devices)
try:
 v.wait('[session] login ready');v.type('Wasm929!');v.key('ret');v.wait('[session] desktop unlocked',60)
 v.terminal();v.command('wasm blob:hello.wasm');v.wait('[wasm] completed blob:hello.wasm');assert '[permission] Consent requested by wasm' not in v.log.read_text()
finally:v.close()
(out/'results.json').write_text(json.dumps({'result':'PASS','firmware':fw,'cpus':4,'keyboard':'VirtIO' if fw=='bios' else 'PS/2','native_runtime':True,'host_bridge':False,'verified':['standard WASM modules','recursion, globals, tables, floats, memory.grow','WASI stdout/exit','native traps and fuel','invalid unused function rejected','unknown imports rejected','FAT32 binary reads over 16KiB','trusted permission denial/grant','ArkFS binary persistence','hardware and touch colon/semicolon']},indent=2)+'\n')
print('PASS native WASM, input, permission, disk and reboot',fw)
