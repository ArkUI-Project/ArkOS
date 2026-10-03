#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
static char output[8192];
static size_t at;
void strcopy(char *d, const char *s, size_t n) {
    if (n) {
        size_t c = strlen(s);
        if (c >= n)
            c = n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
}
static void write_output(const char *s, size_t n, void *unused) {
    (void)unused;
    if (n > sizeof output - at - 1)
        n = sizeof output - at - 1;
    memcpy(output + at, s, n);
    at += n;
    output[at] = 0;
}
extern size_t ark_wasm_heap_used(void);
int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 3;
    static unsigned char bytes[ARK_WASM_MODULE_MAX];
    size_t n = fread(bytes, 1, sizeof bytes, f);
    fclose(f);
    ArkWasm w = {.write = write_output};
    int r = 0;
    unsigned repeat = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
    for (unsigned i = 0; i < repeat; i++) {
        at = 0;
        r = ark_wasm_run(&w, bytes, n, "main");
        if (ark_wasm_heap_used()) {
            fprintf(stderr, "heap not fully reclaimed: %zu\n", ark_wasm_heap_used());
            return 4;
        }
    }
    printf("status=%d exit=%u type=%u result=%lld memory=%llu gas=%llu error=%s\n%s", r,
           w.exit_code, w.has_result, (long long)(w.has_result == 1 ? (int32_t)w.result : w.result),
           (unsigned long long)w.memory_bytes, (unsigned long long)w.gas_used, w.error, output);
    return 0;
}
