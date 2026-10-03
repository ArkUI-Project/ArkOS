#ifndef ARK_WASM_CONFIG_H
#define ARK_WASM_CONFIG_H
/* One execution thread per runtime; ask the engine to probe the bounded stack
 * directly instead of caching its address in unavailable ELF TLS. */
#define M3_THREAD_LOCAL
#define M3_HAS_THREAD_LOCAL 0
#define d_m3HasPosixHost 0
#define d_m3HasWin32Host 0
#define d_m3GuardedMemory 0
#define d_m3VerboseErrorMessages 0
#define d_m3EnableValidation 1
#define d_m3SkipStackCheck 0
#define d_m3SkipMemoryBoundsCheck 0
#define d_m3MaxNativeStack (48 * 1024)
#define d_m3MaxLinearMemoryPages 64
#define d_m3HasGasMetering 1
#define d_m3HasMultiMemory 0
#define d_m3HasMemory64 0
#define d_m3HasTypedRefs 0
#define d_m3HasStackSwitching 0
#define d_m3HasSnapshots 0
#define d_m3HasExceptionHandling 0
#define d_m3HasAtomics 0
#define d_m3HasCompactImports 0
#define d_m3HasWideArithmetic 0
#endif
