#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/wasm-host
python3 scripts/make-wasm-examples.py
# -Wno-maybe-musttail-local-addr: same GCC diagnostic wasm.mk already silences
# for the vendored interpreter translation units; musttail never lets an
# automatic really escape.
FLAGS="-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-maybe-musttail-local-addr -ffunction-sections -fdata-sections -include runtime/wasm/config.h -Iinclude -Iruntime/wasm -Ithird_party/wasm3/source -fsanitize=address,undefined"
# Test the freestanding allocator under a separate symbol, never interpose on
# the sanitizer or the host's own allocation machinery.
ALLOC="-Dmalloc=ark_wasm_malloc -Dcalloc=ark_wasm_calloc -Drealloc=ark_wasm_realloc -Dfree=ark_wasm_free"
for n in m3_bind m3_code m3_compile m3_core m3_env m3_exec m3_function m3_info m3_module m3_parse m3_validate m3_deterministic; do
 cc $FLAGS -Wno-unused-function $ALLOC -c third_party/wasm3/source/$n.c -o build/wasm-host/$n.o
done
cc $FLAGS $ALLOC -fno-builtin -DARK_API_HOST_TEST -c runtime/wasm/port.c -o build/wasm-host/port.o
cc $FLAGS -c runtime/wasm/runtime.c -o build/wasm-host/runtime.o
cc $FLAGS -Wl,--gc-sections tests/wasm_host_test.c build/wasm-host/*.o -lm -o build/wasm-host-test
ASAN_OPTIONS=detect_leaks=0 python3 tests/wasm_test.py
node tests/wasm_oracle.js
