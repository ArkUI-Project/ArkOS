#!/usr/bin/env sh
set -eu
ark_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ark_target=${ARKOS_INSTALL_DISK:-"$ark_root/arkos-installed.img"}
if [ ! -f "$ark_target" ]; then echo 'Install first with scripts/run-installer.sh'; exit 1; fi
exec qemu-system-x86_64 -machine q35 -smp 4 -m 512M -vga vmware -boot c -drive "file=$ark_target,format=raw,if=ide,index=0" -device virtio-tablet-pci -device virtio-keyboard-pci -serial stdio -netdev user,id=net0 -device e1000,netdev=net0,romfile= "$@"
