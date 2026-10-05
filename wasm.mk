# wasm3 is statically compiled into one isolated ArkOS Ring3 application.
WASM_SRC := third_party/wasm3/source
WASM_NAMES := m3_bind m3_code m3_compile m3_core m3_env m3_exec m3_function m3_info m3_module m3_parse m3_validate m3_deterministic
WASM_OBJECTS := $(addprefix build/wasm/,$(addsuffix .o,$(WASM_NAMES))) build/wasm/port.o build/wasm/runtime.o
# -Wno-maybe-musttail-local-addr: GCC 13+ reports wasm3's d_m3Op tail-call chain.
# musttail copies its arguments into the successor frame, so no automatic really
# escapes. The vendored upstream sources stay untouched; only this pinned toolchain
# diagnostic is silenced for the interpreter translation units.
WASM_CFLAGS := $(USER_CFLAGS) -ffunction-sections -fdata-sections -include runtime/wasm/config.h -I$(WASM_SRC) -Iruntime/wasm -Wno-unused-parameter -Wno-maybe-musttail-local-addr
build/wasm:
	mkdir -p $@
build/wasm/%.o: $(WASM_SRC)/%.c runtime/wasm/config.h $(wildcard $(WASM_SRC)/*.h) | build/wasm
	$(CC) $(WASM_CFLAGS) -c $< -o $@
build/wasm/port.o: runtime/wasm/port.c | build/wasm
	$(CC) $(WASM_CFLAGS) -c $< -o $@
build/wasm/runtime.o: runtime/wasm/runtime.c runtime/wasm/runtime.h | build/wasm
	$(CC) $(WASM_CFLAGS) -c $< -o $@
build/apps/wasm.o: USER_CFLAGS += -Iruntime/wasm
build/apps/wasm.elf: build/apps/wasm.o $(USER_COMMON) $(WASM_OBJECTS) user/user.ld
	$(LD) $(USER_LDFLAGS) --gc-sections $(filter %.o,$^) -o $@
	python3 scripts/check-native-elf.py $@

build/apps/wasm.o: runtime/wasm/examples.h runtime/wasm/runtime.h
.PHONY: check-wasm-host
check-wasm-host:
	sh tests/test-wasm.sh

.PHONY: check-wasm-vm
check-wasm-vm: iso
	python3 tests/v9_wasm_test.py bios
	python3 tests/v9_wasm_test.py uefi
