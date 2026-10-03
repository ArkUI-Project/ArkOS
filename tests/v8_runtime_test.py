"""Production ISO: native HTTP via real E1000 and native shell commands.
The local server supplies a normal HTTP test page only; no runtime bridge.
"""
from fixtures import fresh_data_disk, fresh_compat_disk
from vm import VM, ROOT
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from PIL import Image
import os, time, threading, json, hashlib, subprocess

os.environ['ARKOS_MACHINE'] = 'q35'
os.environ['ARKOS_SMP'] = '4'
body = '<h1>ArkOS native HTTP</h1><p>真实 E1000 网络请求与原生中文排版。</p>'.encode()
requests = []
class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        requests.append(self.path)
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

server = ThreadingHTTPServer(('127.0.0.1', 8080), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
out = ROOT/'build/test-native-runtime'
out.mkdir(exist_ok=True)
disk = out/'data.img'
fresh_data_disk(disk)
external = out/'compat.img'
fresh_compat_disk(external)
def ntfs_hash():
    with external.open('rb') as f:
        f.seek(133120*512)
        return hashlib.sha256(f.read(65536*512)).hexdigest()
before_ntfs = ntfs_hash()
v = VM('native-runtime', disk, external=external, device='virtio-multitouch-pci,virtio-tablet-pci,virtio-keyboard-pci')
def command(line, expected=None):
    start = len(v.log.read_text())
    v.command(line)
    if expected: v.wait(expected, after=start)
    time.sleep(.12)
try:
    v.wait('[session] setup ready')
    v.type('Runtime73!'); v.key('tab'); v.type('Runtime73!'); v.key('ret')
    v.wait('[session] desktop unlocked', 60)
    v.terminal()
    command('lscpu', 'Online CPUs: 4')
    command('id', 'uid=1000')
    command('net', 'IPv4 10.0.2.15')
    command('cat /mnt/fat32/README.TXT', 'created independently')
    command('cat /mnt/ntfs/README.txt', 'real NTFS volume')
    command('echo AHCI-native-write > /mnt/fat32/NATIVE.TXT')
    command('cat /mnt/fat32/NATIVE.TXT', 'AHCI-native-write')
    command('sync', 'synchronized')
    command('echo zeta > words.txt')
    command('echo alpha >> words.txt')
    command('echo alpha >> words.txt')
    command('sort words.txt | uniq > sorted.txt')
    command('cat sorted.txt', 'alpha\nzeta')
    command('stat sorted.txt', 'bytes: 11')
    command('find', 'sorted.txt')
    command('run clock', '[app] Clock ring3 ready')
    v.terminal()
    command('ps', 'TID  PID  UID  STATE')
    command('open browser')
    time.sleep(.6)
    v.key('ctrl-l'); v.type('http://10.0.2.2:8080/native'); v.key('ret')
    v.wait('[permission] Consent requested by browser');v.key('ret');v.wait('[permission] Allowed browser')
    v.wait('[net] HTTP done status=200 bytes='+str(len(body)))
    v.wait('[browser] Native page received in isolated process')
    time.sleep(.5)
    v.screen('native-http')
    Image.open(out/'native-http.ppm').save(out/'native-http.png')
    assert requests == ['/native'], requests
    v.key('f4');time.sleep(.8)
    start = len(v.log.read_text())
    v.tap(270,332)  # Application permissions in the current Settings sidebar.
    v.wait('[ui] Settings page 应用权限', after=start)
    v.wait('[page] end frames=', after=start)
    start = len(v.log.read_text())
    v.tap(700,478)  # Browser is the fifth catalog entry.
    v.wait('[ui] Settings page 应用访问权限', after=start)
    v.wait('[page] end frames=', after=start)
    v.screen('browser-permissions')
    start = len(v.log.read_text())
    v.tap(1070,402)  # Revoke NETWORK while retaining UI.
    v.wait('[permission] Settings browser grants=4', after=start)
    v.terminal();command('open browser', '[ui] open Browser');time.sleep(.4)
    start = len(v.log.read_text())
    v.key('ctrl-l');v.type('http://10.0.2.2:8080/must-not-leak');v.key('ret')
    v.wait('[permission] Consent requested by browser', after=start)
    v.key('esc');v.wait('[permission] Denied browser', after=start);time.sleep(.5)
    assert requests == ['/native'], requests
    assert '[exception]' not in v.log.read_text()
    print('PASS production ISO: native shell, SMP snapshot, Ring3 app, E1000 HTTP and UTF-8 HTML')
finally:
    v.close()
    server.shutdown()
    server.server_close()
assert ntfs_hash() == before_ntfs, 'read-only NTFS must remain byte-identical'
fat = subprocess.check_output(['mtype','-i',str(external)+'@@1048576','::/NATIVE.TXT'])
assert fat == b'AHCI-native-write\n', fat
(out/'results.json').write_text(json.dumps({'result':'PASS','http_requests':requests,'body_bytes':len(body),'fat32_native_write':'independently read with mtype','ntfs_unchanged_sha256':before_ntfs,'runtime_host_bridge':False},indent=2)+'\n')
print('PASS AHCI data disk + FAT32/NTFS disk + ATAPI optical drive coexistence, independent FAT32 readback, NTFS unchanged')
