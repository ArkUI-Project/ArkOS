#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/test-apps
for app in clock paint markdown; do
    "${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
      -DARK_API_HOST_TEST -Dmain="${app}_main" -Iinclude -Isdk \
      -c "user/apps/$app.c" -o "build/test-apps/$app.o"
done
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DARK_API_HOST_TEST -Iinclude -Isdk tests/user_apps_host_test.c sdk/app.c \
  user/unicode.c user/raster.c build/test-apps/clock.o build/test-apps/paint.o \
  build/test-apps/markdown.o -lm -o build/test-apps/run
ASAN_OPTIONS=detect_leaks=0 ./build/test-apps/run
