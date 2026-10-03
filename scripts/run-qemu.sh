#!/usr/bin/env sh
set -eu
arkos_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
arkos_iso="$arkos_root/arkos-0.13.0.iso"
arkos_disk="$arkos_root/arkos-data.img"
if [ ! -f "$arkos_iso" ]; then arkos_iso="$arkos_root/build/arkos-0.13.0.iso"; arkos_disk="$arkos_root/build/arkos-data.img"; fi
if [ ! -f "$arkos_iso" ]; then echo 'Build first: make'; exit 1; fi
if [ ! -f "$arkos_disk" ]; then python3 "$arkos_root/scripts/create-disk.py" "$arkos_disk"; fi
arkos_external=${ARKOS_EXTERNAL_DISK:-"$arkos_root/arkos-compat.img"}
if [ -f "$arkos_external" ]; then
 set -- -drive "file=$arkos_external,format=raw,if=ide,index=1" "$@"
fi
if [ -n "${QEMU_DATA:-}" ]; then set -- -L "$QEMU_DATA" "$@"; fi
exec "${QEMU_BIN:-qemu-system-x86_64}" -machine "${ARKOS_MACHINE:-q35}" -cpu "${ARKOS_CPU:-max}" -smp "${ARKOS_CPUS:-4}" -m "${ARKOS_MEMORY:-512M}" -vga "${ARKOS_GPU:-vmware}" -cdrom "$arkos_iso" -boot d \
 -drive "file=$arkos_disk,format=raw,if=ide,index=0" -device virtio-multitouch-pci -device virtio-tablet-pci -device virtio-keyboard-pci \
 -global vmware-svga.vgamem_mb=64 -serial stdio -netdev user,id=net0 -device e1000,netdev=net0,romfile= "$@"
