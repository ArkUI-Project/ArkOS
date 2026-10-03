/* ArkOS freestanding support for the unmodified wasm3 engine. No libc link. */
#include "ark.h"
#include "ark_api.h"
#include <stdlib.h>
#include <math.h>
#define HEAP_BYTES (8u * 1024u * 1024u)
typedef struct Block {
    size_t size;
    struct Block *next;
    bool available;
    uint8_t pad[15];
} Block;
_Static_assert(sizeof(Block) == 32, "allocator alignment");
static _Alignas(16) uint8_t heap[HEAP_BYTES];
static Block *first;
void *malloc(size_t n) {
    if (!n || n > HEAP_BYTES - sizeof(Block) - 15)
        return 0;
    n = (n + 15) & ~(size_t)15;
    if (!first) {
        first = (Block *)heap;
        *first = (Block){HEAP_BYTES - sizeof(Block), 0, true, {0}};
    }
    for (Block *b = first; b; b = b->next)
        if (b->available && b->size >= n) {
            if (b->size - n >= sizeof(Block) + 16) {
                Block *r = (Block *)((uint8_t *)(b + 1) + n);
                *r = (Block){b->size - n - sizeof(Block), b->next, true, {0}};
                b->size = n;
                b->next = r;
            }
            b->available = false;
            return b + 1;
        }
    return 0;
}
size_t ark_wasm_heap_used(void) {
    size_t n = 0;
    for (Block *b = first; b; b = b->next)
        if (!b->available)
            n += b->size;
    return n;
}
void free(void *p) {
    if (!p)
        return;
    Block *b = (Block *)p - 1;
    b->available = true;
    for (b = first; b && b->next;) {
        if (b->available && b->next->available) {
            b->size += sizeof(Block) + b->next->size;
            b->next = b->next->next;
        } else
            b = b->next;
    }
}
void *calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size)
        return 0;
    size_t n = count * size;
    void *p = malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}
void *realloc(void *p, size_t n) {
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return 0;
    }
    Block *b = (Block *)p - 1;
    if (n <= b->size)
        return p;
    void *q = malloc(n);
    if (!q)
        return 0;
    memcpy(q, p, b->size);
    free(p);
    return q;
}
char *strchr(const char *s, int c) {
    do {
        if (*s == (char)c)
            return (char *)s;
    } while (*s++);
    return 0;
}
int __popcountdi2(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return (int)((x * 0x0101010101010101ull) >> 56);
}
void *__memcpy_chk(void *d, const void *s, size_t n, size_t cap) {
    if (n > cap)
        abort();
    return memcpy(d, s, n);
}
void *__memset_chk(void *d, int c, size_t n, size_t cap) {
    if (n > cap)
        abort();
    return memset(d, c, n);
}
_Noreturn void abort(void) {
#ifdef ARK_API_HOST_TEST
    __builtin_trap();
#else
    ark_exit(125);
    for (;;) {
    }
#endif
}
/* SSE2 is mandatory for x86-64 and preserved by ArkOS on every context switch.
 * Explicit bit rounding preserves signed zero and leaves NaN payloads intact. */
static double integral(double x, int mode) {
    union {
        double f;
        uint64_t u;
    } v = {x};
    unsigned exp = (unsigned)(v.u >> 52) & 2047;
    int e = (int)exp - 1023;
    bool neg = (v.u >> 63) != 0;
    if (e >= 52)
        return x;
    if (e < 0) {
        if (!(v.u & 0x7fffffffffffffffull))
            return x;
        if ((mode < 0 && neg) || (mode > 0 && !neg))
            return neg ? -1.0 : 1.0;
        v.u &= 1ull << 63;
        return v.f;
    }
    uint64_t mask = (1ull << (52 - e)) - 1;
    if (!(v.u & mask))
        return x;
    v.u &= ~mask;
    double r = v.f;
    if ((mode < 0 && neg) || (mode > 0 && !neg))
        r += neg ? -1.0 : 1.0;
    return r;
}
double trunc(double x) {
    return integral(x, 0);
}
float truncf(float x) {
    return (float)integral(x, 0);
}
double floor(double x) {
    return integral(x, -1);
}
float floorf(float x) {
    return (float)integral(x, -1);
}
double ceil(double x) {
    return integral(x, 1);
}
float ceilf(float x) {
    return (float)integral(x, 1);
}
double rint(double x) {
    union {
        double f;
        uint64_t u;
    } v = {x};
    if ((v.u & 0x7fffffffffffffffull) >= 0x4330000000000000ull)
        return x;
    double t = trunc(x), d = x - t;
    if (d > 0.5 || (d == 0.5 && ((int64_t)t & 1)))
        t += 1;
    if (d < -0.5 || (d == -0.5 && ((int64_t)t & 1)))
        t -= 1;
    if (t == 0) {
        v.u &= 1ull << 63;
        return v.f;
    }
    return t;
}
float rintf(float x) {
    return (float)rint(x);
}
double nearbyint(double x) {
    return rint(x);
}
float nearbyintf(float x) {
    return rintf(x);
}
double sqrt(double x) {
    double y;
    __asm__("sqrtsd %1,%0" : "=x"(y) : "x"(x));
    return y;
}
float sqrtf(float x) {
    float y;
    __asm__("sqrtss %1,%0" : "=x"(y) : "x"(x));
    return y;
}
double fabs(double x) {
    union {
        double f;
        uint64_t u;
    } v = {x};
    v.u &= 0x7fffffffffffffffull;
    return v.f;
}
float fabsf(float x) {
    union {
        float f;
        uint32_t u;
    } v = {x};
    v.u &= 0x7fffffffu;
    return v.f;
}
double copysign(double x, double y) {
    union {
        double f;
        uint64_t u;
    } a = {x}, b = {y};
    a.u = (a.u & 0x7fffffffffffffffull) | (b.u & 0x8000000000000000ull);
    return a.f;
}
float copysignf(float x, float y) {
    union {
        float f;
        uint32_t u;
    } a = {x}, b = {y};
    a.u = (a.u & 0x7fffffffu) | (b.u & 0x80000000u);
    return a.f;
}
