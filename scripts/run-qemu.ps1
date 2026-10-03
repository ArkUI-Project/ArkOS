param([string]$Gpu = 'vmware', [string]$ExternalDisk = '', [string]$Machine = 'q35', [int]$Cpus = 4)
$ErrorActionPreference = 'Stop'
$ArkRoot = Split-Path -Parent $PSScriptRoot
$ArkIso = Join-Path $ArkRoot 'arkos-0.9.1.iso'
$ArkDisk = Join-Path $ArkRoot 'arkos-data.img'
if (-not (Test-Path $ArkIso)) {
 $ArkIso = Join-Path $ArkRoot 'build/arkos-0.9.1.iso'
 $ArkDisk = Join-Path $ArkRoot 'build/arkos-data.img'
}
if (-not (Test-Path $ArkIso)) { throw 'Build the ISO first with make in Linux or WSL.' }
if (-not (Test-Path $ArkDisk)) { throw 'Missing arkos-data.img. Extract the complete release package or create a new disk with scripts/create-disk.py.' }
if (-not $ExternalDisk) { $ExternalDisk = Join-Path $ArkRoot 'arkos-compat.img' }
$ArkArgs = @('-machine',$Machine,'-smp',"$Cpus",'-m','512M','-vga',$Gpu,'-cdrom',$ArkIso,'-boot','d',
 '-drive',"file=$ArkDisk,format=raw,if=ide,index=0",'-device','virtio-multitouch-pci',
 '-device','virtio-tablet-pci','-device','virtio-keyboard-pci','-serial','stdio','-netdev','user,id=net0','-device','e1000,netdev=net0,romfile=')
if (Test-Path $ExternalDisk) { $ArkArgs += @('-drive',"file=$ExternalDisk,format=raw,if=ide,index=1") }
& qemu-system-x86_64.exe @ArkArgs
