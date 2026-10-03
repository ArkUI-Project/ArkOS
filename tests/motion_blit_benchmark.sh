#!/usr/bin/env sh
# Native scalar renderer only. VM FPS must be measured in a booted ArkOS VM.
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/tests
cc -std=c11 -O2 -Wall -Wextra -Werror -fno-builtin -mgeneral-regs-only \
  -Iinclude tests/motion_blit_benchmark.c user/motion.c user/raster.c \
  -o build/tests/motion_blit_benchmark
./build/tests/motion_blit_benchmark "$@"
