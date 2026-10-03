/* Optional wasm32 C example. A wasm32-capable compiler is a DEVELOPMENT tool;
 * the resulting module runs entirely inside ArkOS. The checked-in hello.wasm
 * is rebuilt independently by scripts/make-wasm-examples.py, not this file. */
#include <stdint.h>
__attribute__((import_module("ark"), import_name("log"))) extern void ark_log(uint32_t offset,
                                                                              uint32_t size);
__attribute__((export_name("main"))) int main(void) {
    static const char message[] = "Hello from native ArkOS WASM! 中国 : ;\n";
    ark_log((uint32_t)(uintptr_t)message, sizeof(message) - 1);
    return 42;
}
