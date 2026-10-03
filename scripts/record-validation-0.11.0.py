#!/usr/bin/env python3
"""Record actual 0.11.0 payloads and selected evidence without retagging candidates."""
import hashlib,json,sys
from datetime import datetime,timezone
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build'
sys.path.insert(0,str(ROOT/'scripts'))
from arkpkg import inspect

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def read(path):return json.loads((BUILD/path).read_text())
def main():
 iso=BUILD/'arkos-0.11.0.iso';kernel=BUILD/'kernel.elf'
 report={'version':'0.11.0','generated_utc':datetime.now(timezone.utc).isoformat(),
  'iso_volume':'ARKOS0110','iso_sha256':sha(iso),'kernel_sha256':sha(kernel),
  'scope_note':'Final desktop, motion and tiny-surface runs use the delivery ISO. Other tests retain their actual candidate or diagnostic payloads; they are not relabelled as final-ISO runs.',
  'runs':[],'evidence':[],'limitations':['Wi-Fi awaits a physical device/driver target; QEMU E1000 is wired.',
   'Stable 60 FPS and actual 120 Hz presentation are not certified.',
   'SDK apps render at 2x on 1440p/4K; desktop chrome remains at 100%.',
   'TLS 1.2 only; fixed public roots, no revocation queries or session persistence.',
   'Counts are bounded by disk/COW space and metadata; runtime process/surface budgets remain.']}
 def evidence(path):
  file=BUILD/path
  row={'path':str(Path('build')/path),'sha256':sha(file),'bytes':file.stat().st_size}
  if row not in report['evidence']:report['evidence'].append(row)
 def run(name,kind,log,**values):
  evidence(log);report['runs'].append({'name':name,'kind':kind,'result':'PASS','log':str(Path('build')/log),**values})
 for fw in ('bios','uefi'):
  path=Path('test-desktop-011-'+fw);result=read(path/'results.json')
  assert len(result['checks'])==3 and result['iso_sha256']==report['iso_sha256'] and result['kernel_sha256']==report['kernel_sha256']
  run('desktop-'+fw,'final-production','0.11.0-desktop-'+fw+'-validation.log',iso_sha256=result['iso_sha256'],kernel_sha256=result['kernel_sha256'],checks=result['checks'])
  for name in ('results.json','serial.log','01-settings-general.png','02-edit-menu-after-wallpaper.png','03-view-menu-hover.png','07-todo-native-font.png','08-todo-text-entry.png'):evidence(path/name)
  path=Path('test-motion-vmware-'+fw);metadata=read(path/'metadata.json');result=read(path/'result.json')
  assert result['passed'] and metadata['iso_sha256']==report['iso_sha256']
  run('motion-'+fw,'final-production','0.11.0-motion-'+fw+'-validation.log',iso_sha256=metadata['iso_sha256'],kernel_sha256=report['kernel_sha256'],scope='Curved minimize/restore, preview, launcher, interior reversal and actual app launch; video sampling is not FPS certification.')
  for name in ('metadata.json','result.json','serial.log','reference-sequence.mp4'):evidence(path/name)
  for scene in ('01-window-minimize','04-window-restore','04b-window-midflight-reverse','05-launcher-open','06-launcher-close','07-launcher-midflight-reverse'):
   for name in ('frames.csv','actions.json','summary.json'):evidence(path/scene/name)
 result=read('test-surface-final/results.json');assert result['result']=='PASS' and result['iso_sha256']==report['iso_sha256']
 run('surface-final','final-production','0.11.0-surface-final-validation.log',**{k:result[k] for k in ('iso_sha256','kernel_sha256','checks')})
 for name in ('results.json','serial.log','tiny-surface.png'):evidence(Path('test-surface-final')/name)
 result=read('test-package-count/results.json');assert result['packages']==48 and result['total_applications']==65 and result['blob_records']==49 and result['index_pages']==2 and all(r['result']=='PASS' for r in result['results'])
 path=Path('test-package-count');assert sha(BUILD/path/'iso/boot/kernel.elf')==result['kernel_sha256']
 run('package-count','native-diagnostic-and-candidate-desktop','0.11.0-package-count-validation.log',diagnostic_kernel_sha256=result['kernel_sha256'],diagnostic_iso_sha256=sha(BUILD/path/'package-count.iso'),scope='48 installs, reboot grants/slots and Ring3 surface; independent 49-object/two-index-page decode. Candidate desktop pagination; final production catalog recovery also covered by surface-final.')
 for name in ('results.json','install.log','reboot.log'):evidence(path/name)
 for name in ('serial.log','launcher-page1.png','launcher-page2.png','launcher-page3.png'):evidence(Path('test-package-count-desktop')/name)
 result=read('test-tls/results.json');path=Path('test-tls');assert result['private_test_ca'] and result['tampered_records']==2 and sha(BUILD/path/'iso/boot/kernel.elf')==result['kernel_sha256']
 for row in result['results']:
  fw=row['firmware'];assert row['result']=='PASS' and row['pcap_sha256']==sha(BUILD/path/(fw+'.pcap'))
  run('tls-'+fw,'native-diagnostic',str(path/(fw+'.log')),kernel_sha256=result['kernel_sha256'],iso_sha256=sha(BUILD/path/'tls-diagnostic.iso'),scope='Test CA, hostname/expiry/issuer rejection, tampered GCM record, exact encrypted response and plain HTTP recovery.')
  evidence(path/(fw+'.pcap'))
 evidence(path/'results.json');evidence('0.11.0-tls-validation.log')
 result=read('test-production-https-public/results.json');assert result['result']=='PASS' and result['production_trust_anchors']
 run('public-https','production-candidate','0.11.0-https-public-validation.log',kernel_sha256=result['kernel_sha256'],scope='Actual Browser, public roots, https://example.com/ status 200 and 577-byte response; candidate ISO was not separately retained.')
 for name in ('results.json','serial.log','https-example.png'):evidence(Path('test-production-https-public')/name)
 result=read('test-highres/results.json');assert all(r['result']=='PASS' for r in result['results'])
 for row in result['results']:
  geometry=row['geometry'];path=Path('test-highres')/geometry
  assert sha(BUILD/path/'iso/boot/kernel.elf')==result['kernel_sha256']
  run('highres-'+geometry,'production-candidate-with-selected-grub-entry','0.11.0-highres-validation.log',kernel_sha256=result['kernel_sha256'],iso_sha256=sha(BUILD/'test-highres'/(geometry+'.iso')),checks=row)
  for name in ('serial.log','01-native-2x-todo.png','02-native-input.png','03-settings.png'):evidence(Path('test-highres-'+geometry)/name)
 evidence('test-highres/results.json')
 for suite,iso_name in (('test-process-smp','protection.iso'),('test-process-api','service-boundary.iso')):
  rows=read(Path(suite)/'results.json');assert {r['firmware'] for r in rows}=={'bios','uefi'} and all(r['result']=='PASS' for r in rows)
  for row in rows:
   fw=row['firmware'];run(suite+'-'+fw,'native-diagnostic',str(Path(suite)/(fw+'.log')),kernel_sha256=sha(BUILD/suite/'iso/boot/kernel.elf'),iso_sha256=sha(BUILD/suite/iso_name))
  evidence(Path(suite)/'results.json')
 for fw in ('bios','uefi'):
  log='0.11.0-installer-'+fw+'-validation.log';assert 'PASS native installer' in (BUILD/log).read_text()
  run('installer-'+fw,'production-candidate-hard-disk',log,kernel_sha256=sha(BUILD/('test-install-'+fw)/'installed-kernel.elf'),scope='Native install, independent GPT CRC, ISO removed, boot from installed disk and persisted account; installed payload extracted read-only for hashing.')
  evidence(Path('test-install-'+fw)/'serial.log');evidence(Path('test-installed-boot-'+fw)/'serial.log');evidence(Path('test-installed-boot-'+fw)/'03-harddisk-boot.png')
 # Preserve original logs, including subsequent failed targets, with explicit scopes.
 for log,marker,scope in (
  ('0.11.0-host-checks.log','Native user apps PASS','make check only; later check-package-host initially failed compilation and was fixed in host-boundaries'),
  ('0.11.0-host-boundaries.log','ALL ACCOUNT TESTS PASSED','Package/blob/Pinyin/account checks; later old service-host model failed and was corrected before services-check'),
  ('0.11.0-services-check.log','518 checks, 0 failures','Corrected service model and production bounds'),
  ('0.11.0-app-network-host.log','12000 malformed wire cases','Calculator/UTF-8 and network parser ASan/UBSan'),
  ('0.11.0-sdk-validation.log','Built native ArkOS ELF','SDK Hello static ELF, W^X and native build')):
  assert marker in (BUILD/log).read_text();run(log.removeprefix('0.11.0-').removesuffix('.log'),'host-checked-scope',log,scope=scope)
 containers=[inspect(p) for p in sorted((BUILD/'packages').glob('ark.*.arkpkg'))]
 assert len(containers)==17 and all(p['system'] and p['version']=='0.11.0' for p in containers)
 (BUILD/'0.11.0-system-packages-inspect.json').write_text(json.dumps(containers,ensure_ascii=False,indent=2)+'\n')
 evidence('0.11.0-system-packages-inspect.json');evidence('0.11.0-package-inspect.json');evidence('0.11.0-final-build.log')
 report['system_packages']=containers
 (BUILD/'0.11.0-final-validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
 print(json.dumps({'iso_sha256':report['iso_sha256'],'kernel_sha256':report['kernel_sha256'],'runs':len(report['runs']),'evidence':len(report['evidence'])},indent=2))
if __name__=='__main__':main()
