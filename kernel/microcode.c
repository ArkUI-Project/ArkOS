#include "microcode.h"
#define UPDATE_MAX (1024u * 1024u)
static uint32_t word(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
size_t intel_microcode_match(const void *blob, size_t bytes, uint32_t signature, uint32_t platform,
                             uint32_t revision) {
    if (!blob || bytes < 48)
        return 0;
    const uint8_t *p = blob;
    uint32_t data = word(p + 28), total = word(p + 32);
    if (!data) {
        data = 2000;
        if (total && total != 2048)
            return 0;
        total = 2048;
    }
    if (word(p) != 1 || word(p + 20) != 1 || !data || (data & 3) || total < 48 ||
        total > UPDATE_MAX || total > bytes || (total & 1023) || data > total - 48)
        return 0;
    if (word(p + 12) != signature || !(word(p + 24) & platform) || word(p + 4) <= revision)
        return 0;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 48 + data; i += 4)
        sum += word(p + i);
    if (sum)
        return 0;
    if (total > 48 + data) {
        uint32_t remain = total - 48 - data;
        if (remain < 20 || (remain & 3))
            return 0;
        const uint8_t *ext = p + 48 + data;
        uint32_t n = word(ext);
        if (n > (remain - 20) / 12 || 20 + n * 12 != remain)
            return 0;
        sum = 0;
        for (uint32_t i = 0; i < remain; i += 4)
            sum += word(ext + i);
        if (sum)
            return 0;
    }
    return total;
}
#ifndef ARK_MICROCODE_HOST_TEST
static uint8_t update[UPDATE_MAX] __attribute__((aligned(16)));
static size_t update_size;
static bool intel_cpu(uint32_t *signature, uint32_t *platform, uint32_t *revision) {
    uint32_t a = 0, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (b != 0x756e6547 || d != 0x49656e69 || c != 0x6c65746e)
        return false;
    a = 1;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    *signature = a;
    if ((c & (1u << 31)) || ((a >> 8) & 15) != 6)
        return false;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(0x17));
    *platform = 1u << ((d >> 18) & 7);
    __asm__ volatile("wrmsr" ::"a"(0), "d"(0), "c"(0x8b));
    a = 1;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(0x8b));
    *revision = d;
    return true;
}
static void apply(void) {
    uint32_t signature, platform, revision;
    if (!update_size || !intel_cpu(&signature, &platform, &revision) ||
        !intel_microcode_match(update, update_size, signature, platform, revision))
        return;
    uint64_t flags;
    __asm__ volatile("pushfq;popq %0;cli" : "=r"(flags)::"memory");
    uint64_t ptr = (uintptr_t)update + 48;
    __asm__ volatile("wrmsr" ::"a"((uint32_t)ptr), "d"((uint32_t)(ptr >> 32)), "c"(0x79)
                     : "memory");
    if (flags & 512)
        __asm__ volatile("sti" ::: "memory");
}
bool microcode_apply_ap(void) {
    if (!update_size)
        return true;
    uint32_t signature, platform, revision;
    if (!intel_cpu(&signature, &platform, &revision))
        return false;
    if (revision >= word(update + 4))
        return true;
    if (!intel_microcode_match(update, update_size, signature, platform, revision))
        return false;
    apply();
    return intel_cpu(&signature, &platform, &revision) && revision >= word(update + 4);
}
void microcode_init(const void *blob, size_t bytes) {
    uint32_t signature, platform, revision;
    if (!intel_cpu(&signature, &platform, &revision)) {
        serial_write("[microcode] Unsupported vendor or virtual CPU; no update attempted\n");
        return;
    }
    char n[24];
    serial_write("[microcode] Intel revision=");
    uint_to_str(revision, n);
    serial_write(n);
    serial_write("\n");
    if (!blob || !bytes) {
        serial_write("[microcode] Firmware revision retained; no vendor module supplied\n");
        return;
    }
    if (bytes > 16 * 1024 * 1024)
        return;
    const uint8_t *p = blob;
    size_t offset = 0;
    uint32_t best = revision;
    while (bytes - offset >= 48) {
        uint32_t size = word(p + offset + 32);
        if (!word(p + offset + 28))
            size = 2048;
        if (!size || size > bytes - offset)
            break;
        size_t valid = intel_microcode_match(p + offset, size, signature, platform, best);
        if (valid) {
            memcpy(update, p + offset, valid);
            update_size = valid;
            best = word(p + offset + 4);
        }
        offset += size;
    }
    if (!update_size) {
        serial_write("[microcode] No newer matching valid primary-signature update\n");
        return;
    }
    apply();
    uint32_t after = 0;
    if (intel_cpu(&signature, &platform, &after) && after == best)
        serial_write("[microcode] Early BSP update verified; APs apply before online\n");
    else {
        serial_write("[microcode] CPU rejected update; firmware revision retained\n");
        update_size = 0;
    }
}
#endif
