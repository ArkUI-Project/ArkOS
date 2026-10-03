#ifndef ARK_WASM_RUNTIME_H
#define ARK_WASM_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#define ARK_WASM_MODULE_MAX (512u * 1024u)
typedef struct {
    void (*write)(const char *, size_t, void *);
    uint64_t (*ticks)(void *);
    void *context;
    char error[192];
    uint32_t exit_code;
    uint64_t gas_used, memory_bytes;
    int64_t result;
    unsigned has_result, output_bytes;
} ArkWasm;
int ark_wasm_run(ArkWasm *, const uint8_t *, size_t, const char *entry);
#endif
