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
    -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -DBR_AES_X86NI=0 -DBR_SSE2=0 \
    kernel/storage.c kernel/arkfs2.c kernel/arkfs2_lz.c kernel/arkfs2_seal.c kernel/sha256.c \
    kernel/vfs.c kernel/lib.c tests/storage_test.c \
    third_party/bearssl/src/aead/gcm.c \
    third_party/bearssl/src/hash/ghash_ctmul64.c \
    third_party/bearssl/src/symcipher/aes_ct.c \
    third_party/bearssl/src/symcipher/aes_ct_ctr.c \
    third_party/bearssl/src/symcipher/aes_ct_enc.c \
    -o "$test_dir/storage-test"
ASAN_OPTIONS=detect_leaks=0 "$test_dir/storage-test" "$test_dir/data.img"
