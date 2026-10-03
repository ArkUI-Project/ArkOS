#!/bin/sh
set -eu
ark_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ark_kernel="$ark_root/arkos-arm64.elf"
if [ ! -f "$ark_kernel" ]; then ark_kernel="$ark_root/build/arm64/arkos-arm64.elf"; fi
exec qemu-system-aarch64 -machine virt,virtualization=off -cpu cortex-a72 -m 512M -smp "${ARKOS_CPUS:-4}" -nographic -kernel "$ark_kernel" "$@"
