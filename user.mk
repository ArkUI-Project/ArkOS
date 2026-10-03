# Independently linked ring-3 programs. Include this fragment from Makefile.
USER_CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -m64 -mno-red-zone -msse2 -mfpmath=sse -mcmodel=small -Iinclude -Isdk
USER_LDFLAGS := -nostdlib -z max-page-size=0x1000 -T user/user.ld
USER_UI_SOURCES := $(filter-out user/pinyin.c user/html.c,$(wildcard user/*.c))
USER_DESKTOP_OBJECTS := $(patsubst user/%.c,build/user/%.o,$(USER_UI_SOURCES)) build/user/lib.o build/user/start.o build/user/alloc.o build/user/pinyin.o
USER_COMMON := build/user/lib.o build/user/start.o build/user/unicode.o build/user/raster.o build/user/arkui.o build/user/arkui_icons.o build/user/arkui_animation.o build/user/motion.o build/user/ribbon.o build/sdk/app.o
NATIVE_APPS := clock paint markdown notes browser calculator todo timer wasm calendar reminders
USER_PROGRAMS := build/user/desktop.elf $(addprefix build/apps/,$(addsuffix .elf,$(NATIVE_APPS)))
.PHONY: user-programs
user-programs: $(USER_PROGRAMS)
build/user build/apps build/sdk:
	mkdir -p $@
build/user/%.o: user/%.c $(wildcard include/*.h) | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@
build/user/desktop.o: $(wildcard user/desktop_*.inc) user.mk
build/user/desktop.o: USER_CFLAGS += -fno-math-errno
build/user/motion.o build/user/ribbon.o: USER_CFLAGS += -fomit-frame-pointer
build/user/motion.o build/user/ribbon.o: user.mk
build/user/unicode.o: USER_CFLAGS += -fno-math-errno
build/user/liquid_glass.o: USER_CFLAGS += -fno-math-errno -fomit-frame-pointer -fno-tree-vectorize -fno-tree-slp-vectorize
build/user/liquid_glass.o: user.mk
build/user/unicode.o: assets/fonts/wqy-microhei.ttc user/unicode_font.h third_party/stb/stb_truetype.h
build/user/lib.o: kernel/lib.c | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@
build/user/start.o: user/start.S | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@
build/sdk/%.o: sdk/%.c sdk/app.h $(wildcard include/*.h) | build/sdk
	$(CC) $(USER_CFLAGS) -c $< -o $@
build/apps/%.o: user/apps/%.c $(wildcard sdk/*.h) $(wildcard include/*.h) | build/apps
	$(CC) $(USER_CFLAGS) -c $< -o $@
build/user/desktop.elf: $(USER_DESKTOP_OBJECTS) user/user.ld
	$(LD) $(USER_LDFLAGS) $(USER_DESKTOP_OBJECTS) -o $@
	python3 scripts/check-native-elf.py $@
build/apps/%.elf: build/apps/%.o $(USER_COMMON) user/user.ld
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) -o $@
	python3 scripts/check-native-elf.py $@

build/user/pinyin.o: user/pinyin_data.h user/ime_lexicon.h

build/apps/browser.elf: build/user/html.o

build/user/alloc.o: kernel/alloc.c include/alloc.h | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@
