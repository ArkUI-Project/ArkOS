from vm import VM,ROOT
import os,sys,time,struct,zlib
from PIL import Image
os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4'
fw=sys.argv[1] if len(sys.argv)>1 else 'bios'
out=ROOT/'build'/('test-install-012-'+fw);out.mkdir(exist_ok=True)
disk=out/'target.img'
with disk.open('wb') as f:f.truncate(128*1024*1024)
v=VM('install-012-'+fw,disk,firmware=fw)
try:
 v.wait('[session] setup ready');v.type('Install73!');v.key('tab');v.type('Install73!');v.key('ret');v.wait('[session] desktop unlocked',60)
 v.key('f5');time.sleep(.4);v.type('Installer');v.key('ret');time.sleep(.5);v.screen('01-installer')
 v.type('ERASE');v.key('ret');v.wait('[installer] Confirmed native disk installation started',10);v.wait('[installer] Native installation complete',90);v.screen('02-installed')
finally:v.close()
raw=disk.read_bytes();assert raw[510:512]==b'\x55\xaa';start,size=struct.unpack_from('<II',raw,446+3*16+8);assert raw[start*512:start*512+8]==b'ARKFS1\0\0'
for lba in (1,len(raw)//512-1):
 h=bytearray(raw[lba*512:(lba+1)*512]);assert h[:8]==b'EFI PART';crc=struct.unpack_from('<I',h,16)[0];struct.pack_into('<I',h,16,0);assert zlib.crc32(h[:92])==crc
 table,n,each,crc=struct.unpack_from('<QIII',h,72);assert zlib.crc32(raw[table*512:table*512+n*each])==crc
os.environ['ARKOS_HDD_ONLY']='1'
v=VM('installed-boot-012-'+fw,disk,firmware=fw)
try:
 v.wait('[session] login ready');v.type('Install73!');v.key('ret');v.wait('[session] desktop unlocked',60);v.key('ctrl-t');time.sleep(.5);v.screen('03-harddisk-boot');Image.open(v.out/'03-harddisk-boot.ppm').save(v.out/'03-harddisk-boot.png');assert '[storage] ArkFS mounted' in v.log.read_text();v.terminal();v.command('pkg list');v.wait('ark.reminders 0.12.0');v.command('reg set /user/install/check string durable');v.wait('Configuration saved.');assert '[exception]' not in v.log.read_text()
finally:v.close()
v=VM('installed-reboot-012-'+fw,disk,firmware=fw)
try:
 v.wait('[session] login ready');v.type('Install73!');v.key('ret');v.wait('[session] desktop unlocked',60);v.terminal();v.command('reg get /user/install/check');v.wait('/user/install/check = durable');assert '[exception]' not in v.log.read_text();print('PASS native installer, GPT CRCs, optical drive removed, independent '+fw+' disk boot and persisted account/configuration')
finally:v.close()

import json,hashlib
(out/"results.json").write_text(json.dumps({"result":"PASS","firmware":fw,"iso_sha256":hashlib.sha256((ROOT/"build/arkos-0.12.0.iso").read_bytes()).hexdigest(),"checks":["native installation to disposable blank 128MiB disk","independent GPT CRC decode","HDD boot with ISO removed and account persistence","registry save on installed system and independent second HDD boot"]},indent=2)+"\n")
