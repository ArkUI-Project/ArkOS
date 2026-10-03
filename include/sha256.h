#ifndef ARK_SHA256_H
#define ARK_SHA256_H
#include <stddef.h>
#include <stdint.h>
void ark_sha256(const void *data, size_t bytes, uint8_t digest[32]);
#endif
