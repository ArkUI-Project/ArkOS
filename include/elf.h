#ifndef ARK_ELF_H
#define ARK_ELF_H
#include "process.h"
typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ELFHeader;
typedef struct __attribute__((packed)) {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} ELFProgram;
bool process_validate_elf(const void *image, size_t bytes, uint64_t caps);
#endif
