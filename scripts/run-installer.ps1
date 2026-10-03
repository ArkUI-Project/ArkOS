param([switch]$BootInstalled)
$ErrorActionPreference = 'Stop'
$ArkRoot = Split-Path -Parent $PSScriptRoot
$ArkIso = Join-Path $ArkRoot 'arkos-0.9.1.iso'
if (-not (Test-Path $ArkIso)) { $ArkIso = Join-Path $ArkRoot 'build/arkos-0.9.1.iso' }
$ArkTarget = Join-Path $ArkRoot 'arkos-installed.img'
if (-not (Test-Path $ArkTarget)) {
 if ($BootInstalled) { throw 'Run without -BootInstalled to install first.' }
 & qemu-img.exe create -f raw $ArkTarget 128M
 if ($LASTEXITCODE -ne 0) { throw 'Unable to create empty virtual disk.' }
}
$ArkArgs = @('-machine','q35','-smp','4','-m','512M','-vga','vmware','-drive',"file=$ArkTarget,format=raw,if=ide,index=0",'-device','virtio-tablet-pci','-device','virtio-keyboard-pci','-serial','stdio','-netdev','user,id=net0','-device','e1000,netdev=net0,romfile=')
if ($BootInstalled) { $ArkArgs += @('-boot','c') } else { $ArkArgs += @('-cdrom',$ArkIso,'-boot','d') }
& qemu-system-x86_64.exe @ArkArgs
