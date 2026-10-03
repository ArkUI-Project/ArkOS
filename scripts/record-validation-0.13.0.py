#!/usr/bin/env python3
"""Index actual 0.13 production, candidate, diagnostic and host evidence."""
import hashlib,json,struct,sys
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

def installed_kernel(disk):
    """Read only: walk the installed ISO9660 directories, not the host ISO."""
    with disk.open('rb') as f:
        def extent(record):
            lba,size=struct.unpack_from('<I',record,2)[0],struct.unpack_from('<I',record,10)[0]
            f.seek(lba*2048);return f.read(size)
        def entry(directory,name):
            pos=0
            while pos<len(directory):
                length=directory[pos]
                if not length:pos=(pos//2048+1)*2048;continue
                record=directory[pos:pos+length];pos+=length
                filename=record[33:33+record[32]].decode('ascii').split(';')[0].lower()
                if filename==name:return record
            raise ValueError('Missing installed boot entry: '+name)
        f.seek(16*2048);pvd=f.read(2048);assert pvd[1:6]==b'CD001'
        directory=extent(pvd[156:156+pvd[156]])
        boot=extent(entry(directory,'boot'))
        return extent(entry(boot,'kernel.elf'))

def main():
    iso=BUILD/'arkos-0.13.0.iso';kernel=BUILD/'kernel.elf'
    with iso.open('rb') as f:f.seek(16*2048+40);volume=f.read(32).decode('ascii').strip()
    assert volume=='ARKOS0130'
    report={
        'version':'0.13.0','generated_utc':datetime.now(timezone.utc).isoformat(),
        'iso_volume':volume,'iso_sha256':sha(iso),'kernel_sha256':sha(kernel),
        'scope_note':'Final GPU performance, visible UTM and native install use the delivery ISO. High-resolution runs select GRUB entries with the final kernel. Earlier UI and protection/VM/GPU diagnostics keep their own payload hashes. Historical network and ARM64 suites were not rerun.',
        'runs':[],'evidence':[],'measurements':[],
        'limitations':[
            'Final Metal GPU glass averages 24.66fps; stable 60fps and actual 120Hz remain unmet.',
            'GPU material samples every rendered animation frame; CPU fallback resamples at 50ms during curved motion.',
            'Material shading is GPU; desktop drawing, curved composition, readback and final scanout retain existing paths.',
            'No Android Skia device pixel comparison; independent native oracle maximum GPU channel difference 2/255.',
            'GPU fixed pipeline, one in-flight command, 64 queue entries/64KiB command buffer, maximum canvas 3840x2160 and two DMA buffers each at most 3968x2304x4 bytes.',
            '32 tasks/threads, 12 surfaces, ordinary 64MiB/SYSTEM 512MiB mapped budget, 480MiB anonymous arena/128 regions, physical pool at most 896MiB and swap at most 64MiB.',
            'No swap encryption or remote TLB shootdown; destructive mappings reject owners still running on another CPU.',
            'Monitor power, ports, compressed memory and per-PID network measurements remain unavailable.',
            'Desktop controls stay at 100%; SDK text/layout uses 2x density on 1440p/4K.',
            'Calendar uses 09:00 new events and loads 256; Reminders loads 128, no recurrence/external sync.',
            'Pinyin input 48 bytes, candidate 63 bytes, learning 256 entries; drag text 511/path 127 bytes and one in-flight transfer.',
            'No physical Wi-Fi target/driver; native terminal has no POSIX PTY/full ANSI applications.',
            'TLS/network/FAT32/NTFS and ARM64 retain historical validation; this increment did not rerun their full suites.'
        ]}
    seen=set()
    def evidence(path):
        path=Path(path);file=BUILD/path
        assert file.is_file() and file.suffix in ('.log','.json','.png','.csv','.plist','.conf'),str(path)
        if path.as_posix() in seen:return
        seen.add(path.as_posix())
        report['evidence'].append({'path':str(Path('build')/path),'sha256':sha(file),'bytes':file.stat().st_size})
    def run(name,kind,log,**values):
        evidence(log)
        report['runs'].append({'name':name,'kind':kind,'result':'PASS','log':str(Path('build')/log),**values})
    def images(folder):
        for p in sorted((BUILD/folder).glob('*.png')):evidence(p.relative_to(BUILD))

    r=read('test-gpu-glass/results.json')
    assert all(row['result']=='PASS' for row in r['results'])
    assert sha(BUILD/'test-gpu-glass/gpu-glass.iso')==r['iso_sha256']
    assert sha(BUILD/'test-gpu-glass/iso/boot/kernel.elf')==r['kernel_sha256']
    evidence('test-gpu-glass/results.json');evidence('guest-013-gpu-pixels.log')
    for row in r['results']:
        fw=row['firmware'];path=Path('test-gpu-glass-'+fw)
        assert 'GPU blur/lens draw, pixels and fence/readback verified' in (BUILD/path/'serial.log').read_text()
        run('gpu-pixels-'+fw,'native-diagnostic',path/'serial.log',
            iso_sha256=r['iso_sha256'],kernel_sha256=r['kernel_sha256'],pixel_reports=row['pixel_reports'],
            checks=['live source changes real GPU pixels','7 independent oracle scenes; maximum channel difference 2',
                    'offscreen and clipped pixels preserved','XRGB high byte zero','bad pointer/geometry/flags rejected',
                    'ordinary UI identity denied','14 GPU jobs / 56 shader passes and real fences/readback'])
        evidence(path/'qemu.log')

    r=read('test-highres-013/results.json');assert r['kernel_sha256']==report['kernel_sha256']
    evidence('test-highres-013/results.json')
    for row in r['results']:
        assert row['result']=='PASS';geometry=row['geometry']
        source=Path('test-highres-013')/geometry/'iso/boot/kernel.elf'
        assert sha(BUILD/source)==report['kernel_sha256']
        run('highres-'+geometry,'final-kernel-selected-grub-entry','guest-013-gpu-highres.log',
            iso_sha256=sha(BUILD/'test-highres-013'/(geometry+'.iso')),kernel_sha256=r['kernel_sha256'],checks=row)
        path=Path('test-highres-013-'+geometry)
        evidence(path/'serial.log');evidence(path/'qemu.log');images(path)

    for fw in ('bios','uefi'):
        path=Path('test-install-013-'+fw);r=read(path/'results.json')
        assert r['result']=='PASS' and r['iso_sha256']==report['iso_sha256']
        payload=installed_kernel(BUILD/path/'target.img')
        kernel_hash=hashlib.sha256(payload).hexdigest();assert kernel_hash==report['kernel_sha256']
        (BUILD/path/'installed-kernel.elf').write_bytes(payload)
        run('installer-'+fw,'final-production-hard-disk','guest-013-final-install-'+fw+'.log',
            iso_sha256=r['iso_sha256'],kernel_sha256=kernel_hash,checks=r['checks'],
            kernel_hash_source='Read-only ISO9660 extraction of actual installed disk boot/kernel.elf.')
        evidence(path/'results.json');evidence(path/'serial.log')
        for stem in ('test-installed-boot-013-','test-installed-reboot-013-'):
            evidence(Path(stem+fw)/'serial.log');images(Path(stem+fw))

    path=Path('test-utm-gpu/ArkOS-GPU.utm')
    assert sha(BUILD/path/'Data/arkos-0.13.0.iso')==report['iso_sha256']
    assert 'GPU blur/lens draw, pixels and fence/readback verified' in (BUILD/path/'Data/serial.log').read_text()
    run('visible-utm-boot','final-production',path/'Data/serial.log',
        iso_sha256=report['iso_sha256'],kernel_sha256=report['kernel_sha256'],
        checks=['native UTM visible display reaches guest account setup','native GPU clear/draw/readback self-test passed'])
    evidence(path/'config.plist');evidence(path/'Data/debug.log');evidence(path/'screenshot.png')

    for suite,logstem in (('interaction','guest-013-delivery-interaction-'),('lifecycle','guest-013-final-lifecycle-')):
        for fw in ('bios','uefi'):
            path=Path('test-'+suite+'-013-'+fw);r=read(path/'results.json');assert r['result']=='PASS'
            log=logstem+fw+'.log'
            if suite=='lifecycle' and fw=='uefi':log='guest-013-delivery-lifecycle-uefi.log'
            run(suite+'-'+fw,'production-candidate',log,iso_sha256=r['iso_sha256'],
                kernel_sha256=r['kernel_sha256'],checks=r['checks'],
                page_animation_frames=r.get('page_animation_frames'))
            evidence(path/'results.json');evidence(path/'serial.log');images(path)
            if suite=='interaction':evidence(Path(str(path)+'-persist')/'serial.log')
    cursor=read('test-cursor-013/results.json');assert cursor['result']=='PASS'
    run('cursor-software','production-candidate','guest-013-final-cursor.log',
        iso_sha256=cursor['iso_sha256'],kernel_sha256=cursor['kernel_sha256'],checks=cursor['checks'],
        scope='Eight actual software cursor shapes and stationary repaint; hardware cursor is separate from QMP screendump.')
    evidence('test-cursor-013/results.json');evidence('test-cursor-013/serial.log')
    for p in sorted((BUILD/'test-cursor-013').glob('*.png')):evidence(p.relative_to(BUILD))

    r=read('test-memory-013/results.json');evidence('test-memory-013/results.json');evidence('guest-013-final-memory.log')
    for row in r:
        assert row['result']=='PASS'
        if 'firmware' in row:
            fw=row['firmware'];log=Path('test-memory-013-'+fw)/'serial.log'
            kernel_path=BUILD/'test-memory-013/memory/boot/kernel.elf';name='vm-demand-swap-'+fw
            iso_path=BUILD/'test-memory-013/memory.iso'
        else:
            log=Path('test-memory-013/write-failure.log');kernel_path=BUILD/'test-memory-013/memory-readonly/boot/kernel.elf'
            name='vm-write-eacces';iso_path=BUILD/'test-memory-013/memory-readonly.iso'
            evidence('test-memory-013/deny-writes.conf')
        assert sha(kernel_path)==row['kernel_sha256']
        run(name,'native-diagnostic',log,kernel_sha256=row['kernel_sha256'],iso_sha256=sha(iso_path),checks=row)
    for suite,image in (('test-process-smp','protection.iso'),('test-process-api','service-boundary.iso')):
        r=read(Path(suite)/'results.json');evidence(Path(suite)/'results.json')
        for row in r:
            assert row['result']=='PASS';fw=row['firmware']
            run(suite+'-'+fw,'native-diagnostic',Path(suite)/(fw+'.log'),
                kernel_sha256=sha(BUILD/suite/'iso/boot/kernel.elf'),iso_sha256=sha(BUILD/suite/image))

    for log,marker,scope in (
        ('host-013-final.log','PASS registry namespace','Storage/input/Shell/ArkUI/fonts, SDK apps, account, earlier 599 service checks, Pinyin, binary store, WASM, motion, package and registry checks.'),
        ('host-013-final-liquid.log','PASS 61461','Independent upstream lens, dispersion, highlight, Gaussian, clipping and spring reference.'),
        ('host-013-final-gpu-boundary.log','603 checks, 0 failures','Final service/identity/pointer/geometry boundaries and GPU SYSTEM gate; no mocked GPU success claim.'),
        ('host-013-final-gpu-fallback.log','PASS production live glass','Production CPU fallback pixel equality with 1/4 workers, opaque elision, clipping, tiny geometry and themes.'),
        ('workspace-013-build.log','Built native ArkOS ELF','SDK multi-window/drag ELF and W^X package build.')):
        assert marker in (BUILD/log).read_text();run(log.removesuffix('.log'),'host-checked-scope',log,scope=scope)

    for name in ('baseline','v1','profile','notify','preparation'):
        path=Path('test-performance-013-gpu-'+name);r=read(path/'results.json');evidence(path/'results.json')
        for row in r['results']:
            assert len(row['records'])==8 and sha(BUILD/path/'013-tested.iso')==row['iso_sha256']
            if name=='preparation':assert row['iso_sha256']==report['iso_sha256'] and row['gpu_glass']
            report['measurements'].append({'name':name,'evidence':str(Path('build')/path/'results.json'),
                'platform':r['platform'],'scenario':r['scenario'],'qemu':r['qemu'],**row})
        evidence(path/'serial.log');evidence(path/'qemu.log')
    evidence('test-performance-013-gpu-preparation/warmup.png')
    for log in ('guest-013-gpu-baseline-performance.log','guest-013-gpu-v1-performance.log',
                'guest-013-gpu-profile-performance.log','guest-013-gpu-notify-performance.log',
                'guest-013-gpu-preparation-performance.log'):evidence(log)

    failure_logs=['guest-013-gpu-pixels-initial.log','guest-013-final-install-bios-before-package-version.log',
                  'guest-013-final-install-uefi-before-package-version.log']
    report['retained_failures']=[]
    for log in failure_logs:
        evidence(log);report['retained_failures'].append({'result':'FAIL before fix','log':'build/'+log})
    report['measurement_note']='Same UTM/M4/QEMU configuration, one VM per run; CPU 50ms resampling versus GPU per rendered frame. Final 24.66fps is not 60/120Hz certification; 24.64 to 24.66 is within run noise.'
    packages=[inspect(p) for p in sorted((BUILD/'packages').glob('ark.*.arkpkg'))]
    assert len(packages)==19 and all(p['system'] and p['version']=='0.13.0' for p in packages)
    report['system_packages']=packages
    (BUILD/'0.13.0-system-packages-inspect.json').write_text(json.dumps(packages,ensure_ascii=False,indent=2)+'\n')
    evidence('0.13.0-system-packages-inspect.json')
    workspace=inspect(BUILD/'workspace-013.arkpkg');assert workspace['id']=='workspace' and workspace['version']=='0.13.0'
    report['sdk_example']={**workspace,'elf_sha256':sha(BUILD/'workspace-013.elf')}
    evidence('workspace-013-inspect.json')
    destination=BUILD/'0.13.0-final-validation.json'
    destination.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({'report':str(destination),'runs':len(report['runs']),'evidence_files':len(report['evidence']),
        'iso_sha256':report['iso_sha256'],'kernel_sha256':report['kernel_sha256']},indent=2))

if __name__=='__main__':main()
