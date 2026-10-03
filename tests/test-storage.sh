#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
python3 scripts/create-disk.py "$test_dir/data.img"
if python3 scripts/create-disk.py "$test_dir/data.img" 2>/dev/null; then
    echo 'FAIL: image creator overwrote an existing file' >&2
    exit 1
fi
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fno-builtin \
    -fsanitize=address,undefined -DARK_STORAGE_HOST_TEST -Iinclude \
    kernel/storage.c kernel/vfs.c kernel/lib.c tests/storage_test.c \
    -o "$test_dir/storage-test"
ASAN_OPTIONS=detect_leaks=0 "$test_dir/storage-test" "$test_dir/data.img"
