#!/usr/bin/env python3
"""Create a self-contained GPU QEMU development VM for installed UTM 4.7.5.

Configuration keys follow UTM's v4.7.5 Codable format. Guest execution is the
normal ArkOS ISO, VirtIO/VirGL PCI GPU, VMware display and native guest drivers.
Existing bundle settings and disk contents are preserved on subsequent runs.
"""
from pathlib import Path
import argparse,plistlib,shutil,subprocess,uuid

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,default=ROOT/'build/ArkOS-0.13.0-GPU.utm')
parser.add_argument('--name',default='ArkOS 0.13 GPU')
default_iso=ROOT/'arkos-0.13.0.iso'
if not default_iso.is_file():default_iso=ROOT/'build/arkos-0.13.0.iso'
parser.add_argument('--iso',type=Path,default=default_iso)
args=parser.parse_args()
if not args.iso.is_file():parser.error('Build the ArkOS ISO first: make')
bundle=args.output.resolve();data=bundle/'Data'
# QEMU keeps the ISO and configuration open. Updating a running bundle would
# leave UTM's cached configuration and its live process on different devices.
running=subprocess.run(['ps','-axo','command'],text=True,capture_output=True,check=True).stdout
if str(data/'arkos-data.img') in running:
    parser.error('Shut down this ArkOS virtual machine before updating its boot media and mouse channel.')
data.mkdir(parents=True,exist_ok=True)
shutil.copyfile(args.iso,data/'arkos-0.13.0.iso')
disk=data/'arkos-data.img'
if not disk.exists():subprocess.run(['python3',str(ROOT/'scripts/create-disk.py'),str(disk)],check=True)
config=bundle/'config.plist'
if not config.exists():
    display={'DynamicResolution':False,'UpscalingFilter':'Nearest','DownscalingFilter':'Linear','NativeResolution':True}
    values={
        'Backend':'QEMU','ConfigurationVersion':4,
        'Information':{'Name':args.name,'UUID':str(uuid.uuid4()).upper(),'IconCustom':False,
                       'Notes':'ArkOS GPU 开发环境。首次启动创建本地账户，数据保存在此虚拟机内。'},
        'System':{'Architecture':'x86_64','Target':'q35','CPU':'max','CPUFlagsAdd':[],
                  'CPUFlagsRemove':[],'CPUCount':4,'ForceMulticore':True,'MemorySize':1024,'JITCacheSize':256},
        'QEMU':{'DebugLog':True,'UEFIBoot':False,'RNGDevice':False,'BalloonDevice':False,
                'TPMDevice':False,'Hypervisor':False,'TSO':False,'RTCLocalTime':False,'PS2Controller':True,
                'AdditionalArguments':['-boot d','-device virtio-tablet-pci','-device virtio-keyboard-pci',
                                       '-netdev user,id=arknet','-device e1000,netdev=arknet,romfile=',
                                       '-global vmware-svga.vgamem_mb=64',
                                       '-serial "file:'+str(data/'serial.log')+'"']},
        'Input':{'UsbBusSupport':'Disabled','UsbSharing':False,'MaximumUsbShare':0},
        'Sharing':{'DirectoryShareMode':'None','DirectoryShareReadOnly':True,'ClipboardSharing':False},
        'Display':[{**display,'Hardware':'vmware-svga'},
                   {**display,'Hardware':'virtio-gpu-gl-pci'}],
        'Drive':[{'ImageName':'arkos-data.img','ImageType':'Disk','Interface':'IDE','InterfaceVersion':1,
                  'Identifier':str(uuid.uuid4()).upper(),'ReadOnly':False},
                 {'ImageName':'arkos-0.13.0.iso','ImageType':'CD','Interface':'IDE','InterfaceVersion':1,
                  'Identifier':str(uuid.uuid4()).upper(),'ReadOnly':True}],
        'Network':[],'Serial':[],'Sound':[]}
    config.write_bytes(plistlib.dumps(values))
values=plistlib.loads(config.read_bytes())
arguments=values['QEMU']['AdditionalArguments']
pointer_arguments=['-device virtio-serial-pci,id=arkpointerbus,max_ports=2,disable-legacy=on',
                   '-chardev spicevmc,id=arkpointer,name=vdagent',
                   '-device virtserialport,nr=1,bus=arkpointerbus.0,chardev=arkpointer,name=com.redhat.spice.0']
for argument in pointer_arguments:
    if argument not in arguments:arguments.append(argument)
config.write_bytes(plistlib.dumps(values))
print(bundle)
