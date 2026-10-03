#!/usr/bin/env sh
set -eu
arkos_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$(uname -s)" = Darwin ] && [ -z "${QEMU_BIN:-}" ]; then
 if [ ! -d /Applications/UTM.app ]; then
  echo 'GPU launch on macOS requires a QEMU build with VirGL. Install UTM or set QEMU_BIN to a GL-enabled build.' >&2
  exit 1
 fi
 python3 "$arkos_root/scripts/create-utm-gpu.py" >/dev/null
 exec open -a /Applications/UTM.app "$arkos_root/build/ArkOS-0.13.0-GPU.utm"
fi
# Standalone Linux/QEMU GL builds normally provide a GTK GL display.
# A caller can override the display with explicit trailing QEMU arguments.
exec sh "$arkos_root/scripts/run-qemu.sh" -device virtio-gpu-gl-pci -display gtk,gl=on "$@"
