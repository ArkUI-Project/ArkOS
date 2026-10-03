#!/usr/bin/env sh
# Creates an EMPTY emulated disk only. ArkOS itself performs installation.
set -eu
ark_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ark_iso="$ark_root/arkos-0.9.1.iso"
[ -f "$ark_iso" ] || ark_iso="$ark_root/build/arkos-0.9.1.iso"
ark_target=${ARKOS_INSTALL_DISK:-"$ark_root/arkos-installed.img"}
[ -f "$ark_target" ] || qemu-img create -f raw "$ark_target" 128M
exec qemu-system-x86_64 -machine q35 -smp 4 -m 512M -vga vmware -cdrom "$ark_iso" -boot d -drive "file=$ark_target,format=raw,if=ide,index=0" -device virtio-tablet-pci -device virtio-keyboard-pci -serial stdio -netdev user,id=net0 -device e1000,netdev=net0,romfile= "$@"
