/* Hardware entropy only. Absence/failure is reported to security protocols. */
#include "tls.h"
bool platform_secure_random(void *output, size_t length) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (!(c & (1u << 30)))
        return false;
    uint8_t *p = output;
    while (length) {
        uint64_t value = 0;
        unsigned char ok = 0;
        for (unsigned retry = 0; retry < 32 && !ok; retry++)
            __asm__ volatile("rdrand %0;setc %1" : "=r"(value), "=qm"(ok));
        if (!ok) {
            memset(output, 0, (size_t)(p - (uint8_t *)output));
            return false;
        }
        size_t n = length < 8 ? length : 8;
        memcpy(p, &value, n);
        p += n;
        length -= n;
    }
    return true;
}
