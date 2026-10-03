/* ACPI-described HPET monotonic clock. Independent of delayed/missed IRQs.
 * PIT remains the scheduler interrupt source; no firmware or host service. */
#include "mmio.h"
extern const uint8_t *platform_acpi_rsdp(void);
static volatile uint64_t *counter;
static uint64_t origin, period_fs, ticks_per_second;
static bool checksum(const uint8_t *p, unsigned size) {
    uint8_t sum = 0;
    for (unsigned i = 0; i < size; i++)
        sum += p[i];
    return !sum;
}
static const uint8_t *table(uint64_t address) {
    if (address < 0x100000 || address > 0xffff0000)
        return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)address;
    uint32_t size;
    memcpy(&size, p + 4, 4);
    return size >= 36 && size <= 65536 && checksum(p, size) ? p : 0;
}
void clock_init(void) {
    const uint8_t *r = platform_acpi_rsdp();
    if (!r || strncmp((const char *)r, "RSD PTR ", 8) || !checksum(r, 20))
        return;
    uint64_t address = 0;
    unsigned stride = 4;
    if (r[15] >= 2 && checksum(r, 36)) {
        memcpy(&address, r + 24, 8);
        stride = 8;
    }
    if (!address) {
        uint32_t low;
        memcpy(&low, r + 16, 4);
        address = low;
        stride = 4;
    }
    const uint8_t *root = table(address);
    if (!root)
        return;
    uint32_t size;
    memcpy(&size, root + 4, 4);
    if (strncmp((const char *)root, stride == 8 ? "XSDT" : "RSDT", 4) || (size - 36) % stride)
        return;
    for (unsigned i = 36; i + stride <= size; i += stride) {
        address = 0;
        memcpy(&address, root + i, stride);
        const uint8_t *t = table(address);
        if (!t || strncmp((const char *)t, "HPET", 4))
            continue;
        uint32_t length;
        memcpy(&length, t + 4, 4);
        if (length < 56 || t[40] != 0)
            return;
        uint64_t base;
        memcpy(&base, t + 44, 8);
        if (!base || (base & 7) || !platform_map_mmio(base, 1024))
            return;
        volatile uint64_t *regs = (volatile uint64_t *)(uintptr_t)base;
        uint64_t caps = regs[0];
        period_fs = caps >> 32;
        if (!(caps & (1u << 13)) || period_fs < 1000 || period_fs > 100000000)
            return;
        ticks_per_second = 1000000000000000ull / period_fs;
        regs[0x10 / 8] |= 1;
        counter = regs + 0xf0 / 8;
        origin = *counter;
        serial_write("[clock] ACPI HPET 64-bit monotonic milliseconds\n");
        return;
    }
}
uint64_t clock_millis(void) {
    if (!counter)
        return UINT64_MAX;
    uint64_t elapsed = *counter - origin;
    return elapsed / ticks_per_second * 1000 +
           (elapsed % ticks_per_second) * period_fs / 1000000000000ull;
}
