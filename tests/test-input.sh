#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Iinclude tests/input_host_test.c -Wl,--gc-sections -o build/input_host_test
./build/input_host_test
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Iinclude tests/virtio_pointer_host_test.c -Wl,--gc-sections -o build/virtio_pointer_host_test
./build/virtio_pointer_host_test
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Iinclude tests/spice_mouse_host_test.c -Wl,--gc-sections -o build/spice_mouse_host_test
./build/spice_mouse_host_test
