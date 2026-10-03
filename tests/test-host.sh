#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
export ASAN_OPTIONS=detect_leaks=0
sh tests/test-storage.sh
sh tests/test-input.sh
cc -std=c11 -Wall -Wextra -Werror -g -DARK_STORAGE_HOST_TEST \
 -fsanitize=address,undefined -Iinclude tests/host_shell_test.c \
 user/shell.c kernel/vfs.c -o build/host-shell-test
./build/host-shell-test
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -Iinclude tests/arkui_test.c user/arkui.c user/unicode.c user/arkui_icons.c user/arkui_animation.c user/raster.c user/motion.c user/ribbon.c -lm -o build/arkui-test
./build/arkui-test
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -Iinclude tests/raster_test.c user/raster.c -o build/raster-test
./build/raster-test

cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -Iinclude tests/unicode_render_test.c user/unicode.c -lm -o build/unicode-render-test
./build/unicode-render-test

cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -Iinclude tests/arkui_v5_test.c user/arkui.c user/unicode.c user/arkui_icons.c \
 user/arkui_animation.c user/raster.c user/motion.c user/ribbon.c -lm -o build/arkui-v5-test
./build/arkui-v5-test

sh tests/test-user-apps.sh
