#!/usr/bin/env python3
"""Real ATA slave FAT32/NTFS test: independent processes and host cross-checks."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import vm as helper
from vm import VM, ROOT
from fixtures import fresh_compat_disk

FAT_START = 2048
FAT_COUNT = 131072
NTFS_START = 133120
NTFS_COUNT = 65536

def digest_partition(path,start,count):
    h=hashlib.sha256()
    with path.open('rb') as f:
        f.seek(start*512)
        remaining=count*512
        while remaining:
            data=f.read(min(1024*1024,remaining));assert data
            h.update(data);remaining-=len(data)
    return h.hexdigest()

def command(vm,text,expected=None):
    before=len(vm.log.read_text())
    vm.command(text)
    vm.wait(text+'\n',after=before)
    if expected is not None:vm.wait(expected,after=before)
    return vm.log.read_text()[before:]

def shutdown(vm):
    command(vm,'shutdown')
    vm.p.wait(timeout=15)
    assert vm.p.returncode==0
    assert 'Powering off...' in vm.log.read_text()
    vm.close()

def main():
    parent=ROOT/'build/test-external';parent.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix='run-',dir=parent))
    external=work/'compat-data.img';fresh_compat_disk(external)
    ark=work/'arkos-data.img'
    subprocess.run(['python3',str(ROOT/'scripts/create-disk.py'),str(ark)],check=True)
    (work/'build').mkdir()
    iso=work/'build/arkos-0.4.0.iso';shutil.copy2(Path(os.environ.get('ARKOS_ISO',str(ROOT/'build/arkos-0.4.0.iso'))),iso)
    os.environ['ARKOS_ISO']=str(iso)
    iso_sha=hashlib.sha256(iso.read_bytes()).hexdigest()
    helper.ROOT=work
    env=os.environ.copy();tools=ROOT.parent/'toolroot/usr'
    env['PATH']=str(tools/'sbin')+':'+str(tools/'bin')+':'+env['PATH']
    script=work/'guest-script.sh'
    script.write_text('''mkdir /mnt/fat32/原生目录
cp /mnt/ntfs/中文说明.txt /mnt/fat32/原生目录/复制自NTFS.txt
echo "FAT32 kernel write" > /mnt/fat32/guest.txt
echo "Persistent second line" >> /mnt/fat32/guest.txt
write /mnt/fat32/README.TXT Updated by native ArkOS ATA
cp /mnt/fat32/guest.txt /home/ark/from-fat32.txt
cp /mnt/ntfs/README.txt /home/ark/from-ntfs.txt
cat /mnt/ntfs/README.txt | grep ArkOS | wc -l > /mnt/fat32/pipe-count.txt
sync
echo SCRIPT_DONE
''')
    vol=str(external)+'@@'+str(FAT_START*512)
    subprocess.run(['mcopy','-i',vol,str(script),'::/test-script.sh'],env=env,check=True)
    ntfs_before=digest_partition(external,NTFS_START,NTFS_COUNT)
    sessions=[]
    def launch(label,firmware):
        print('Starting '+label+' '+firmware,flush=True)
        vm=VM(label,disk=ark,external=external,firmware=firmware,gpu='std',device='virtio-multitouch-pci,virtio-tablet-pci')
        assert '[storage] ArkFS mounted' in vm.log.read_text()
        assert '[extfs] FAT32 mounted; NTFS read-only' in vm.log.read_text()
        vm.terminal();return vm
    vm=launch('01-bios-write','bios')
    try:
        command(vm,'mounts','FAT32')
        command(vm,'ls /mnt/fat32','中文示例.txt')
        command(vm,'cat /mnt/fat32/README.TXT','created independently')
        command(vm,'cat /mnt/ntfs/README.txt','real NTFS volume')
        command(vm,'sh /mnt/fat32/test-script.sh','SCRIPT_DONE')
        command(vm,'cat /mnt/fat32/guest.txt','FAT32 kernel write\nPersistent second line\n')
        command(vm,'echo forbidden > /mnt/ntfs/should-not-exist.txt','read-only')
        command(vm,'cat /mnt/ntfs/large.txt','16383')
        command(vm,'sync','synchronized')
        vm.screen('native-external-volumes')
        shutdown(vm)
    finally:vm.close()
    assert digest_partition(external,NTFS_START,NTFS_COUNT)==ntfs_before
    print('PASS BIOS: genuine ATA reads/writes, UTF-8 cross-volume copy, NTFS read-only',flush=True)
    sessions.append('BIOS write + native ArkFS copy + NTFS read-only digest')
    vm=launch('02-uefi-read','uefi')
    try:
        command(vm,'cat /mnt/fat32/guest.txt','FAT32 kernel write\nPersistent second line\n')
        command(vm,'cat /mnt/fat32/README.TXT','Updated by native ArkOS ATA')
        command(vm,'cat /home/ark/from-fat32.txt','Persistent second line')
        command(vm,'cat /home/ark/from-ntfs.txt','real NTFS volume')
        command(vm,'cat /mnt/fat32/pipe-count.txt','\n1\n')
        command(vm,'cat /mnt/ntfs/README.txt','real NTFS volume')
        command(vm,'rm /mnt/fat32/pipe-count.txt')
        command(vm,'sync','synchronized')
        vm.screen('uefi-persistent-fat32')
        shutdown(vm)
    finally:vm.close()
    assert digest_partition(external,NTFS_START,NTFS_COUNT)==ntfs_before
    sessions.append('Independent UEFI process read persistent FAT32 and ArkFS copies; deletion flushed')
    extracted=work/'fat32-after.img'
    with external.open('rb') as src,extracted.open('xb') as out:
        src.seek(FAT_START*512)
        remaining=FAT_COUNT*512
        while remaining:
            chunk=src.read(min(1024*1024,remaining));assert chunk
            out.write(chunk);remaining-=len(chunk)
    checked=subprocess.run(['fsck.fat','-n',str(extracted)],env=env,text=True,capture_output=True)
    (work/'fsck-fat32.txt').write_text(checked.stdout+checked.stderr)
    assert checked.returncode==0,checked.stdout+checked.stderr
    def host_text(path):
        return subprocess.check_output(['mtype','-i',str(extracted),'::/'+path],env=env).decode('utf-8')
    assert host_text('guest.txt')=='FAT32 kernel write\nPersistent second line\n'
    assert host_text('README.TXT')=='Updated by native ArkOS ATA'
    chinese=host_text('原生目录/复制自NTFS.txt')
    assert '中文' in chinese
    absent=subprocess.run(['mtype','-i',str(extracted),'::/pipe-count.txt'],env=env,capture_output=True)
    assert absent.returncode!=0
    ntfs_after=digest_partition(external,NTFS_START,NTFS_COUNT)
    result={'result':'PASS','when_utc':datetime.now(timezone.utc).isoformat(),'iso_sha256':iso_sha,'frozen_iso':str(iso),'external_image':str(external),'sessions':sessions,'host_fsck_exit':checked.returncode,'host_chinese_copy':chinese,'ntfs_partition_sha256_before':ntfs_before,'ntfs_partition_sha256_after':ntfs_after,'ntfs_unchanged':ntfs_before==ntfs_after}
    (work/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print('PASS: UEFI persistence + host fsck/mtype + NTFS partition unchanged',flush=True)
    print('Evidence: '+str(work/'result.json'),flush=True)
if __name__=='__main__':main()
