#!/usr/bin/env python3
"""Record actual payloads, checked scopes and measurements for the 0.12 increment."""
import hashlib,json,sys
from datetime import datetime,timezone
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build'
sys.path.insert(0,str(ROOT/'scripts'))
from arkpkg import inspect
def sha(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
 return h.hexdigest()
def read(path):return json.loads((BUILD/path).read_text())
def main():
 iso=BUILD/'arkos-0.12.0.iso';kernel=BUILD/'kernel.elf'
 with iso.open('rb') as f:f.seek(16*2048+40);volume=f.read(32).decode('ascii').strip()
 assert volume=='ARKOS0120'
 report={'version':'0.12.0','generated_utc':datetime.now(timezone.utc).isoformat(),
 'iso_volume':volume,'iso_sha256':sha(iso),'kernel_sha256':sha(kernel),
 'scope_note':'Only final-production runs use the delivery ISO. Earlier desktop, feature, high-resolution, installer, motion and timing runs keep their actual candidate hashes; protection/API probes use independent diagnostic kernels.',
 'runs':[],'evidence':[],'measurements':[],
 'limitations':['Stable 60 FPS and actual 120 Hz display presentation remain unmet; measured live material is slower than 0.11 cached material.',
 'Live material is resampled every 50ms during curved motion; this is not 120Hz material refresh.',
 'QEMU E1000 is wired; no physical Wi-Fi target or wireless driver.',
 'Desktop chrome stays at 100%; SDK text/layout uses 2x density on 1440p/4K.',
 '32 task/thread slots, 12 surfaces, 64MiB ordinary process mapping; package count instead grows with storage and index memory.',
 'Calendar new events use fixed 09:00 and load at most 256 records; Reminders loads 128 with no recurrence/sync.',
 'Pinyin has 48 input bytes, 63-byte candidates and 256 learned entries; native original decoder with licensed Rime/OpenCC/Unihan data.',
 'Terminal executes native ArkOS commands; POSIX PTY and full-screen ANSI programs are not implemented.',
 'TLS 1.2/public-root checks and ARM64 results retain their historical scope; no new TLS/ARM64 validation in this increment.']}
 seen=set()
 def evidence(path):
  path=Path(path);file=BUILD/path
  assert file.is_file() and file.suffix in ('.log','.json','.png','.csv'),str(path)
  if str(path) in seen:return
  seen.add(str(path));report['evidence'].append({'path':str(Path('build')/path),'sha256':sha(file),'bytes':file.stat().st_size})
 def run(name,kind,log,**values):
  evidence(log);report['runs'].append({'name':name,'kind':kind,'result':'PASS','log':str(Path('build')/log),**values})
 def images(path):
  for p in sorted((BUILD/path).glob('*.png')):evidence(p.relative_to(BUILD))
 for fw in ('bios','uefi'):
  path=Path('test-drop-failure-012-'+fw);r=read(path/'results.json')
  assert r['result']=='PASS' and r['iso_sha256']==report['iso_sha256'] and r['kernel_sha256']==report['kernel_sha256']
  run('drop-failure-'+fw,'final-production','guest-drop-failure-012-'+fw+'.log',iso_sha256=r['iso_sha256'],kernel_sha256=r['kernel_sha256'],checks=r['checks'])
  evidence(path/'results.json');evidence(path/'serial.log');images(path)
  path=Path('test-desktop-012-'+fw);r=read(path/'results.json')
  assert r['result']=='PASS' and len(r['checks'])==10
  run('desktop-'+fw,'production-candidate','guest-final-012-desktop-'+fw+'.log',iso_sha256=r['iso_sha256'],kernel_sha256=r['kernel_sha256'],checks=r['checks'],additional_executed_assertions=['Same PID after minimize/restore, native menu hover/live wallpaper, actual Tasks performance view.'])
  evidence(path/'results.json');evidence(path/'serial.log');evidence(Path(str(path)+'-persist')/'serial.log');images(path)
 r=read('test-features-012-files/results.json');assert r['result']=='PASS' and len(r['checks'])==1
 run('files-wheel-new','production-candidate','guest-final-012-files.log',iso_sha256=r['iso_sha256'],checks=r['checks'])
 evidence('test-features-012-files/results.json');evidence('test-features-012-files/serial.log');images(Path('test-features-012-files'))
 r=read('test-features-012/results.json');assert r['result']=='PASS' and len(r['checks'])==8
 run('feature-gestures-themes-reminder','production-candidate','guest-features-012.log',iso_sha256=r['iso_sha256'],checks=r['checks'][1:],excluded_claim='Original first Files check was weak and had an incorrect directory setup; replaced by files-wheel-new on its actual candidate.')
 evidence('test-features-012/results.json');evidence('test-features-012/serial.log');evidence('test-features-012-persist/serial.log')
 for p in sorted((BUILD/'test-features-012').glob('*.png')):
  if p.name not in ('01-files-top.png','02-files-scrolled.png'):evidence(p.relative_to(BUILD))
 r=read('test-highres-012/results.json');assert all(row['result']=='PASS' for row in r['results'])
 for row in r['results']:
  geometry=row['geometry'];path=Path('test-highres-012')/geometry
  assert sha(BUILD/path/'iso/boot/kernel.elf')==r['kernel_sha256']
  kind='production-kernel-selected-grub-entry' if r['kernel_sha256']==report['kernel_sha256'] else 'production-candidate-selected-grub-entry'
  run('highres-'+geometry,kind,'guest-final-012-highres.log',kernel_sha256=r['kernel_sha256'],iso_sha256=sha(BUILD/'test-highres-012'/(geometry+'.iso')),checks=row)
  evidence(Path('test-highres-012-'+geometry)/'serial.log');images(Path('test-highres-012-'+geometry))
 evidence('test-highres-012/results.json')
 installed=[]
 for fw in ('bios','uefi'):
  path=Path('test-install-012-'+fw);r=read(path/'results.json');assert r['result']=='PASS'
  kernel_hash=sha(BUILD/path/'installed-kernel.elf');installed.append(kernel_hash)
  log='guest-final-012-install-'+fw+'.log';assert 'PASS native installer' in (BUILD/log).read_text()
  run('installer-'+fw,'production-candidate-hard-disk',log,iso_sha256=r['iso_sha256'],kernel_sha256=kernel_hash,checks=r['checks'],kernel_hash_source='Read-only extraction of the installed boot payload.')
  evidence(path/'results.json');evidence(path/'serial.log')
  for name in ('test-installed-boot-012-','test-installed-reboot-012-'):
   evidence(Path(name+fw)/'serial.log');images(Path(name+fw))
  path=Path('test-motion-012-vmware-'+fw);m=read(path/'metadata.json');r=read(path/'result.json');assert r['passed'] and len(r['scenarios'])==10
  run('motion-'+fw,'production-candidate','guest-final-012-motion-'+fw+'.log',iso_sha256=m['iso_sha256'],kernel_sha256=kernel_hash,scope='Actual advanced curved motion, preview, launcher, interior reversal and real app launch; QMP 30fps samples do not certify guest FPS.',machine='QEMU TCG default pc, one CPU, 512MiB')
  for name in ('metadata.json','result.json','serial.log'):evidence(path/name)
  for scene in r['scenarios']:
   folder=path/scene['name']
   for name in ('frames.csv','actions.json','summary.json','serial.log'):evidence(folder/name)
   # Retain representative keyframes alongside complete timings and hashes.
   frames=sorted((BUILD/folder).glob('frame-*.png'));assert len(frames)==scene['sample_count']
   for index in sorted({0,len(frames)//8,len(frames)//4,len(frames)//2,len(frames)-1}):evidence(frames[index].relative_to(BUILD))
 assert len(set(installed))==1
 for suite,image in (('test-process-smp','protection.iso'),('test-process-api','service-boundary.iso')):
  r=read(Path(suite)/'results.json');assert {row['firmware'] for row in r}=={'bios','uefi'} and all(row['result']=='PASS' for row in r)
  for row in r:
   fw=row['firmware'];run(suite+'-'+fw,'native-diagnostic',str(Path(suite)/(fw+'.log')),kernel_sha256=sha(BUILD/suite/'iso/boot/kernel.elf'),iso_sha256=sha(BUILD/suite/image))
  evidence(Path(suite)/'results.json')
 for log,marker,scope in (
 ('host-012.log','ArkUI v5 PASS','Storage/input/Shell/ArkUI/raster/fonts only; subsequent old application model failed before correction.'),
 ('host-apps-drop-012.log','Native user apps PASS','Actual Clock/Paint/Markdown loops, file saves and drop failure/success preserving content.'),
 ('host-extra-012.log','70 objects','Registry/blob checks only; subsequent missing Pinyin link symbol failed before correction.'),
 ('host-pinyin-012.log','PASS native Pinyin','Native full-Pinyin segmentation, sentence ranking, candidates, learning and bounds.'),
 ('host-service-012.log','586 checks, 0 failures','Current native service model, capabilities/UID/pointers, surfaces/events, release and drag ownership.'),
 ('host-package-012.log','24 installed packages','Package validation, grants, COW recovery and multiple live instance lifecycle.'),
 ('host-rest-012.log','vector crossfade equals','Calculator/UTF-8 and original motion reference only; later WASM warning failed before correction.'),
 ('host-raster-012.log','every alpha','SSE2 blend exactness, span guards and rounded AA.'),
 ('host-compositor-012.log','equals prior material pixels','Actual optimized glass/shadow pixels against previous reference.'),
 ('host-wallpaper-bounds-012.log','UBSan no signed overflow','Actual production wallpaper at supported resolutions through 4K; light/dark, corner values and guards.'),
 ('host-wasm-012.log','zero live heap bytes','23 fixtures and independent engine; 600 native lifetime/trap cycles.'),
 ('workspace-012-build.log','Built native ArkOS ELF','SDK Workspace static ELF, W^X and native build.')):
  assert marker in (BUILD/log).read_text();run(log.removesuffix('.log'),'host-checked-scope',log,scope=scope)
 for path in ('test-performance-012/results.json','test-performance-012-parallel/results.json','test-performance-012-files-cache/results.json','test-performance-012-final/results.json'):
  r=read(path);evidence(path)
  for row in r['results']:
   assert len(row['records'])==8
   report['measurements'].append({'evidence':str(Path('build')/path),'platform':r['platform'],'scenario':r['scenario'],**row})
 evidence('guest-final-012-performance.log');evidence('test-performance-012-final/serial.log')
 report['measurement_note']='Initial raw records are retained in JSON; some early VM serial directories were reused. The latest measurement is on its recorded candidate, before the final Markdown drop/text-only fix; it is not retagged final.'
 containers=[inspect(p) for p in sorted((BUILD/'packages').glob('ark.*.arkpkg'))]
 assert len(containers)==19 and all(p['system'] and p['version']=='0.12.0' for p in containers)
 report['system_packages']=containers
 (BUILD/'0.12.0-system-packages-inspect.json').write_text(json.dumps(containers,ensure_ascii=False,indent=2)+'\n')
 evidence('0.12.0-system-packages-inspect.json')
 workspace=inspect(BUILD/'workspace-012.arkpkg');assert workspace['id']=='workspace' and workspace['version']=='0.12.0' and workspace['permissions']==['ui']
 report['sdk_example']={**workspace,'elf_sha256':sha(BUILD/'workspace-012.elf')};evidence('workspace-012-inspect.json')
 for log in ('build-012-highres-final.log','guest-before-media-012-install-bios.log','guest-files-012.log'):evidence(log)
 report['historical_failed_logs_note']='Media version gate and Files target-directory failures are preserved for diagnosis; they are not PASS runs.'
 (BUILD/'0.12.0-final-validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
 print(json.dumps({'iso_sha256':report['iso_sha256'],'kernel_sha256':report['kernel_sha256'],'runs':len(report['runs']),'evidence':len(report['evidence'])},indent=2))
if __name__=='__main__':main()
