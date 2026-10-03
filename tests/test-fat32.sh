#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
FAT_TOOLS=${FAT_TOOLS:-../toolroot/usr}
export PATH="$FAT_TOOLS/bin:$FAT_TOOLS/sbin:$PATH"
work=$(mktemp -d "${TMPDIR:-/tmp}/arkos-fat-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
truncate -s 64M "$work/fat32.img"
mkfs.fat -F 32 -n ARKSHARE "$work/fat32.img"
printf 'Host-created FAT32 file\n' > "$work/readme.txt"
printf '中文内容，由宿主 mtools 创建。\n' > "$work/chinese.txt"
mmd -i "$work/fat32.img" ::/Documents
mcopy -i "$work/fat32.img" "$work/readme.txt" ::/README.TXT
mcopy -i "$work/fat32.img" "$work/chinese.txt" ::/Documents/中文文件.txt
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -fno-builtin -fsanitize=address,undefined -Iinclude kernel/fat32.c kernel/lib.c tests/fat32_host_test.c -o "$work/test"
ASAN_OPTIONS=detect_leaks=0 "$work/test" "$work/fat32.img"
fsck.fat -n "$work/fat32.img"
mtype -i "$work/fat32.img" ::/README.TXT
mtype -i "$work/fat32.img" ::/新目录/重命名.txt
if test -n "${FAT_OUTPUT:-}"; then
 test ! -e "$FAT_OUTPUT"
 cp "$work/fat32.img" "$FAT_OUTPUT"
fi
