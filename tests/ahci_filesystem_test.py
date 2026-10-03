"""Real SATA transport: FAT32 write, NTFS read-only and a separate UEFI reboot."""
from fixtures import fresh_data_disk, fresh_compat_disk
from vm import VM,ROOT
import os,subprocess,hashlib,json,time
os.environ['ARKOS_MACHINE']='q35';os.environ['ARKOS_SMP']='4'
out=ROOT/'build/test-ahci-filesystems';out.mkdir(parents=True,exist_ok=True)
external=out/'compat.img';disk=out/'data.img'
fresh_compat_disk(external);fresh_data_disk(disk)
volume=str(external)+'@@1048576'
def ntfs_hash():
 with external.open('rb') as f:f.seek(133120*512);return hashlib.sha256(f.read(65536*512)).hexdigest()
before=ntfs_hash()
script=out/'sata.sh';script.write_text('echo AHCI_FAT32_WRITE > /mnt/fat32/sata-v6.txt\ncp /mnt/ntfs/README.txt /mnt/fat32/ntfs-copy.txt\nsync\necho AHCI_DONE\n')
subprocess.run(['mcopy','-o','-i',volume,str(script),'::/sata.sh'],check=True)
for firmware in ['bios','uefi']:
 v=VM('ahci-fs-'+firmware,disk,firmware=firmware,gpu='vmware',external=external)
 try:
  v.wait('[session] setup ready' if firmware=='bios' else '[session] login ready')
  assert '[ahci] Native SATA DMA disk' in v.log.read_text()
  v.type('Native63!')
  if firmware=='bios':v.key('tab');v.type('Native63!')
  v.key('ret');v.wait('[session] desktop unlocked',60);v.terminal()
  checkpoint=len(v.log.read_text())
  if firmware=='bios':
   v.command('sh /mnt/fat32/sata.sh');v.wait('AHCI_DONE',after=checkpoint)
   checkpoint=len(v.log.read_text());v.command('echo forbidden > /mnt/ntfs/forbidden.txt');v.wait('read-only',after=checkpoint)
  else:
   v.command('cat /mnt/fat32/sata-v6.txt');v.wait('AHCI_FAT32_WRITE',after=checkpoint)
   v.command('cat /mnt/fat32/ntfs-copy.txt');v.wait('real NTFS volume',after=checkpoint)
  assert '[exception]' not in v.log.read_text() and 'User fault' not in v.log.read_text()
 finally:v.close()
 assert ntfs_hash()==before
actual=subprocess.check_output(['mtype','-i',volume,'::/sata-v6.txt']).decode()
assert actual=='AHCI_FAT32_WRITE\n',repr(actual)
assert 'real NTFS volume' in subprocess.check_output(['mtype','-i',volume,'::/ntfs-copy.txt']).decode()
(out/'result.json').write_text(json.dumps({'result':'PASS','driver':'native AHCI SATA','firmware':['bios','uefi'],'checks':['FAT32 native write + independent host mtype','native NTFS to FAT32 copy','UEFI reboot reads prior FAT32 writes','NTFS write rejected; entire NTFS partition SHA256 unchanged'],'ntfs_sha256':before},indent=2))
print('PASS AHCI: FAT32 write and reboot persistence, NTFS read/copy and rejected writes, host mtype + unchanged NTFS digest')
