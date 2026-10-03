#!/usr/bin/env sh
set -eu
arkos_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
arkos_qemu="$arkos_root/build/tools/qemu-system-x86_64-high-refresh"
if [ ! -x "$arkos_qemu" ]; then
 python3 "$arkos_root/scripts/prepare-qemu-high-refresh.py" --output "$arkos_qemu"
fi
export QEMU_BIN="$arkos_qemu"
export QEMU_DATA="${QEMU_DATA:-/opt/homebrew/share/qemu}"
exec sh "$arkos_root/scripts/run-qemu.sh" "$@"
