#!/usr/bin/env sh
# Build one original ArkOS native ELF using this source checkout's SDK/runtime.
set -eu
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 source.c output.elf" >&2
    exit 2
fi
case "$1" in /*) app_source=$1 ;; *) app_source=$PWD/$1 ;; esac
case "$2" in /*) app_output=$2 ;; *) app_output=$PWD/$2 ;; esac
sdk_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
app_objects=$(mktemp -d)
trap 'rm -rf "$app_objects"' EXIT HUP INT TERM
cd "$sdk_root"
index=0
for source in "$app_source" sdk/app.c kernel/lib.c user/start.S user/unicode.c user/raster.c user/arkui.c user/arkui_icons.c user/arkui_animation.c user/motion.c user/ribbon.c; do
    "${CC:-gcc}" -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding \
      -fno-builtin -fno-math-errno -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables \
      -m64 -mno-red-zone -msse2 -mfpmath=sse -mcmodel=small \
      -Iinclude -Isdk -c "$source" -o "$app_objects/$index.o"
    index=$((index+1))
done
mkdir -p "$(dirname "$app_output")"
"${LD:-ld}" -nostdlib -z max-page-size=0x1000 -T user/user.ld "$app_objects"/*.o -o "$app_output"
python3 scripts/check-native-elf.py "$app_output"
echo "Built native ArkOS ELF: $app_output"
