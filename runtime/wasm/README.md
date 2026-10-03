# ArkOS wasm3 port

`config.h`: build policy and limits. `port.c`: an 8MiB bounded/coalescing allocator,
minimal memory/string/compiler helpers, freestanding scalar mathematics.
`runtime.c`: eager validation, explicit imports, memory/table/execution limits,
start/export execution, trap reporting and cleanup. `runtime.h`: embedding API.
`examples.h`: generated standard bytecode used by the native application.

The upstream source in `third_party/wasm3/source/` is unchanged. Only the engine
translation units listed in `wasm.mk` are linked. Upstream POSIX/Windows hosts,
WASI system-call adapters, libc imports, CLI, socket APIs and snapshot system are
not linked. The final link is `ld -nostdlib`; no `libc`, `libm`, Linux ABI or host
bridge is present. Headers from the build toolchain provide C declarations only.

`scripts/check-native-elf.py` rejects dynamic loading, TLS sections/segments and
writable executable segments. A real guest test caught wasm3's default TLS stack
cache, which does not work on ArkOS ABI v1. The supported no-TLS configuration
uses the existing explicit stack budget instead; it does not disable protection.

This port is an experimental runtime, not a security certification or complete
WASI implementation. See `docs/WASM.md` for the contract and `tests/wasm_test.py`
and `tests/v9_wasm_test.py` for executable evidence.
