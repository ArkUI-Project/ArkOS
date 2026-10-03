#include "ark.h"
void *memset(void *p, int v, size_t n) {
    void *result = p;
    uint64_t value = (uint8_t)v * 0x0101010101010101ull;
    size_t words = n / 8, tail = n % 8;
    __asm__ volatile("cld;rep stosq" : "+D"(p), "+c"(words) : "a"(value) : "memory");
    __asm__ volatile("rep stosb" : "+D"(p), "+c"(tail) : "a"((uint8_t)v) : "memory");
    return result;
}
void *memcpy(void *d, const void *s, size_t n) {
    void *result = d;
    size_t words = n / 8, tail = n % 8;
    /* The kernel and interrupt entry keep DF clear. State it explicitly here so
     * this primitive also remains safe in stand-alone driver diagnostic kernels.
     * No vector state, alignment requirement, or access beyond n is involved. */
    __asm__ volatile("cld; rep movsq" : "+D"(d), "+S"(s), "+c"(words)::"memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(tail)::"memory");
    return result;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a < b) {
        while (n--)
            *a++ = *b++;
    } else if (a > b) {
        while (n) {
            --n;
            a[n] = b[n];
        }
    }
    return d;
}
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] - y[i];
    return 0;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i])
            return 0;
    }
    return 0;
}
void strcopy(char *d, const char *s, size_t cap) {
    if (!cap)
        return;
    size_t n = 0;
    while (s[n] && n + 1 < cap) {
        d[n] = s[n];
        ++n;
    }
    d[n] = 0;
}
void uint_to_str(uint64_t n, char *out) {
    char b[24];
    int i = 0, j = 0;
    do {
        b[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (i)
        out[j++] = b[--i];
    out[j] = 0;
}
