#ifndef ARK_ALLOC_H
#define ARK_ALLOC_H
#include <stddef.h>
void *ark_alloc(size_t bytes);
void *ark_realloc(void *pointer, size_t bytes);
void ark_free(void *pointer);
#endif
