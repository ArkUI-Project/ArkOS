.PHONY: check-net-host check-net-vm
check-net-host: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/net_host_test.c -o build/net-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/net-host-test
check-net-vm:
	python3 tests/net_vm_test.py

# Vendored sources only: an ordinary build never fetches dependencies.
BEARSSL_SOURCES := $(wildcard third_party/bearssl/src/*.c third_party/bearssl/src/*/*.c)
BEARSSL_OBJECTS := $(patsubst third_party/bearssl/src/%.c,build/bearssl/%.o,$(BEARSSL_SOURCES))
# Host libc fortification would emit *_chk calls absent from the guest runtime.
BEARSSL_FLAGS := -U_FORTIFY_SOURCE -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 -DBR_RDRAND=0 -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0 -ffunction-sections -fdata-sections
build/bearssl/%.o: third_party/bearssl/src/%.c net.mk
	mkdir -p $(@D)
	$(CC) $(CFLAGS) $(BEARSSL_FLAGS) -Wno-unused-parameter -c $< -o $@
build/kernel.elf: $(BEARSSL_OBJECTS)
LDFLAGS += --gc-sections
