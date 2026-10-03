A64_CC ?= aarch64-linux-gnu-gcc
A64_LD ?= aarch64-linux-gnu-ld
A64_FLAGS := -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -mgeneral-regs-only -mno-outline-atomics
.PHONY: arm64 run-arm64
arm64: build/arm64/arkos-arm64.elf
build/arm64:
	mkdir -p $@
build/arm64/%.o: arm64/%.c | build/arm64
	$(A64_CC) $(A64_FLAGS) -c $< -o $@
build/arm64/start.o: arm64/start.S | build/arm64
	$(A64_CC) $(A64_FLAGS) -c $< -o $@
build/arm64/arkos-arm64.elf: build/arm64/start.o build/arm64/kernel.o build/arm64/mmu.o build/arm64/exception.o arm64/linker.ld
	$(A64_LD) -nostdlib -T arm64/linker.ld $(filter %.o,$^) -o $@
run-arm64: arm64
	qemu-system-aarch64 -machine virt,virtualization=off -cpu cortex-a72 -m 512M -smp 4 -nographic -kernel build/arm64/arkos-arm64.elf

build/arm64/exception.o: arm64/exception.S | build/arm64
	$(A64_CC) $(A64_FLAGS) -c $< -o $@
