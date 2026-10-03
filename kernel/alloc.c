/* Original coalescing metadata arena. Exhaustion is a recoverable memory error. */
#include "ark.h"
#include "alloc.h"
static uint8_t metadata[4 * 1024 * 1024] __attribute__((aligned(16)));
typedef struct Block {
    size_t bytes;
    struct Block *next;
    bool free;
    uint8_t pad[7];
} Block;
static Block *first;
void *ark_alloc(size_t n) {
    if (!n || n > sizeof metadata - sizeof(Block))
        return 0;
    n = (n + 15) & ~(size_t)15;
    if (!first) {
        first = (Block *)metadata;
        *first = (Block){sizeof metadata - sizeof(Block), 0, true, {0}};
    }
    for (Block *b = first; b; b = b->next)
        if (b->free && b->bytes >= n) {
            if (b->bytes >= n + sizeof(Block) + 16) {
                Block *next = (Block *)((uint8_t *)(b + 1) + n);
                *next = (Block){b->bytes - n - sizeof(Block), b->next, true, {0}};
                b->next = next;
                b->bytes = n;
            }
            b->free = false;
            memset(b + 1, 0, b->bytes);
            return b + 1;
        }
    return 0;
}
void ark_free(void *p) {
    if (!p)
        return;
    Block *b = (Block *)p - 1;
    b->free = true;
    for (Block *a = first; a && a->next;) {
        if (a->free && a->next->free) {
            a->bytes += sizeof(Block) + a->next->bytes;
            a->next = a->next->next;
        } else
            a = a->next;
    }
}
void *ark_realloc(void *p, size_t n) {
    if (!p)
        return ark_alloc(n);
    if (!n) {
        ark_free(p);
        return 0;
    }
    Block *b = (Block *)p - 1;
    if (n <= b->bytes)
        return p;
    void *next = ark_alloc(n);
    if (!next)
        return 0;
    memcpy(next, p, b->bytes);
    ark_free(p);
    return next;
}
