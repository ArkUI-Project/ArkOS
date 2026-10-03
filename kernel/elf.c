/* Shared admission checks for embedded images and ArkOS packages. */
#include "elf.h"
#define IMAGE_LIMIT (PROCESS_USER_BASE + 512ull * 1024 * 1024)
#define ELF_LIMIT (32u * 1024u * 1024u)
#define USER_STACK_BYTES (128u * 1024u)
static bool segment_ok(const ELFProgram *p, size_t bytes) {
    return p->memsz && p->filesz <= p->memsz && p->offset <= bytes &&
           p->filesz <= bytes - p->offset && p->vaddr >= PROCESS_USER_BASE &&
           p->vaddr < IMAGE_LIMIT && p->memsz <= IMAGE_LIMIT - p->vaddr && !(p->vaddr & 4095) &&
           !(p->offset & 4095) && p->align == 4096 && !(p->flags & ~7u) && (p->flags & 4) &&
           ((p->flags & 3) != 3);
}
bool process_validate_elf(const void *image, size_t bytes, uint64_t caps) {
    if (!image || bytes < sizeof(ELFHeader) || bytes > ELF_LIMIT)
        return false;
    const ELFHeader *h = image;
    if (h->ident[0] != 0x7f || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F' ||
        h->ident[4] != 2 || h->ident[5] != 1 || h->ident[6] != 1 || h->ident[7] != 0 ||
        h->type != 2 || h->machine != 62 || h->version != 1 || h->ehsize != sizeof(*h) ||
        h->phentsize != sizeof(ELFProgram) || !h->phnum || h->phnum > 32 || h->phoff > bytes ||
        h->phnum * sizeof(ELFProgram) > bytes - h->phoff)
        return false;
    const ELFProgram *ph = (const ELFProgram *)((const uint8_t *)image + h->phoff);
    uint64_t required = USER_STACK_BYTES;
    bool entry_ok = false;
    unsigned loads = 0;
    for (unsigned i = 0; i < h->phnum; i++) {
        const ELFProgram *p = &ph[i];
        if (p->type == 2 || p->type == 3 || p->type == 7 ||
            (p->type == 0x6474e551 && (p->flags & 1)))
            return false;
        if (p->type != 1)
            continue;
        if (!p->memsz) {
            if (p->filesz)
                return false;
            continue;
        }
        if (!segment_ok(p, bytes))
            return false;
        uint64_t end = (p->vaddr + p->memsz + 4095) & ~4095ull;
        required += end - p->vaddr;
        loads++;
        if ((p->flags & 1) && h->entry >= p->vaddr && h->entry < p->vaddr + p->filesz)
            entry_ok = true;
        for (unsigned j = 0; j < i; j++)
            if (ph[j].type == 1 && ph[j].memsz) {
                uint64_t other_end = (ph[j].vaddr + ph[j].memsz + 4095) & ~4095ull;
                if (p->vaddr < other_end && ph[j].vaddr < end)
                    return false;
            }
    }
    uint64_t limit = (caps & PROCESS_CAP_SYSTEM) ? 512ull * 1024 * 1024 : 64ull * 1024 * 1024;
    if (!loads || !entry_ok || required > limit)
        return false;
    return true;
}
