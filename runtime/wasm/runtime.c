/* Native ArkOS embedding. Only the imports below are reachable by a module. */
#include "runtime.h"
#include "wasm3.h"
#include "m3_env.h"
#include "ark.h"
static const char terminated[] = "ArkOS: proc_exit";
static bool range(void *mem, uint32_t p, uint32_t n) {
    size_t size = m3_GetMemorySizeAt(mem);
    return mem && p <= size && n <= size - p;
}
static uint32_t load32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void store32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (i * 8));
}
static m3ApiRawFunction(native_log) {
    m3ApiGetArg(uint32_t, p);
    m3ApiGetArg(uint32_t, n);
    ArkWasm *w = m3_GetUserData(runtime);
    if (n > 4096 || !range(_mem, p, n))
        return m3Err_trapOutOfBoundsMemoryAccess;
    if (n > 65536 - w->output_bytes)
        return "ArkOS: output limit";
    if (w->write)
        w->write((char *)_mem + p, n, w->context);
    w->output_bytes += n;
    return m3Err_none;
}
static m3ApiRawFunction(native_ticks) {
    m3ApiReturnType(uint64_t);
    ArkWasm *w = m3_GetUserData(runtime);
    m3ApiReturn(w->ticks ? w->ticks(w->context) : 0);
}
static m3ApiRawFunction(wasi_exit) {
    m3ApiGetArg(uint32_t, code);
    ArkWasm *w = m3_GetUserData(runtime);
    w->exit_code = code;
    return terminated;
}
static m3ApiRawFunction(wasi_write) {
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, fd);
    m3ApiGetArg(uint32_t, iovs);
    m3ApiGetArg(uint32_t, count);
    m3ApiGetArg(uint32_t, written);
    if (fd != 1 && fd != 2)
        m3ApiReturn(8);
    if (count > 64)
        m3ApiReturn(28);
    if (!range(_mem, iovs, count * 8) || !range(_mem, written, 4))
        m3ApiReturn(21);
    uint8_t *mem = _mem;
    ArkWasm *w = m3_GetUserData(runtime);
    uint32_t total = 0;
    /* Validate every vector before any externally visible output. */
    for (unsigned i = 0; i < count; i++) {
        uint32_t p = load32(mem + iovs + 8 * i), n = load32(mem + iovs + 8 * i + 4);
        if (!range(_mem, p, n))
            m3ApiReturn(21);
        if (n > 65536 - total)
            m3ApiReturn(28);
        total += n;
    }
    if (total > 65536 - w->output_bytes)
        m3ApiReturn(51);
    for (unsigned i = 0; i < count; i++) {
        uint32_t p = load32(mem + iovs + 8 * i), n = load32(mem + iovs + 8 * i + 4);
        if (w->write)
            w->write((char *)mem + p, n, w->context);
    }
    w->output_bytes += total;
    store32(mem + written, total);
    m3ApiReturn(0);
}
static m3ApiRawFunction(wasi_sizes) {
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, a);
    m3ApiGetArg(uint32_t, b);
    if (!range(_mem, a, 4) || !range(_mem, b, 4))
        m3ApiReturn(21);
    store32((uint8_t *)_mem + a, 0);
    store32((uint8_t *)_mem + b, 0);
    m3ApiReturn(0);
}
static m3ApiRawFunction(wasi_empty) {
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, a);
    m3ApiGetArg(uint32_t, b);
    (void)a;
    (void)b;
    m3ApiReturn(0);
}
static m3ApiRawFunction(wasi_clock) {
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, id);
    m3ApiGetArg(uint64_t, precision);
    m3ApiGetArg(uint32_t, p);
    (void)precision;
    if (id != 1)
        m3ApiReturn(28);
    if (!range(_mem, p, 8))
        m3ApiReturn(21);
    ArkWasm *w = m3_GetUserData(runtime);
    uint64_t t = (w->ticks ? w->ticks(w->context) : 0) * 10000000ull;
    for (unsigned i = 0; i < 8; i++)
        ((uint8_t *)_mem)[p + i] = (uint8_t)(t >> (i * 8));
    m3ApiReturn(0);
}
static M3Result link_imports(IM3Module m) {
    static const struct {
        const char *space, *name, *signature;
        M3RawCall fn;
    } bindings[] = {{"ark", "log", "v(ii)", native_log},
                    {"ark", "ticks", "I()", native_ticks},
                    {"wasi_snapshot_preview1", "fd_write", "i(iiii)", wasi_write},
                    {"wasi_snapshot_preview1", "proc_exit", "v(i)", wasi_exit},
                    {"wasi_snapshot_preview1", "args_sizes_get", "i(ii)", wasi_sizes},
                    {"wasi_snapshot_preview1", "args_get", "i(ii)", wasi_empty},
                    {"wasi_snapshot_preview1", "environ_sizes_get", "i(ii)", wasi_sizes},
                    {"wasi_snapshot_preview1", "environ_get", "i(ii)", wasi_empty},
                    {"wasi_snapshot_preview1", "clock_time_get", "i(iIi)", wasi_clock}};
    for (unsigned i = 0; i < sizeof bindings / sizeof bindings[0]; i++) {
        M3Result e = m3_LinkRawFunction(m, bindings[i].space, bindings[i].name,
                                        bindings[i].signature, bindings[i].fn);
        if (e && e != m3Err_functionLookupFailed)
            return e;
    }
    for (unsigned i = 0; i < m->numFuncImports; i++)
        if (!m->functions[i].compiled)
            return "ArkOS: unsupported import";
    return m3Err_none;
}
int ark_wasm_run(ArkWasm *w, const uint8_t *bytes, size_t n, const char *entry) {
    w->error[0] = 0;
    w->exit_code = 0;
    w->gas_used = w->memory_bytes = 0;
    w->has_result = w->output_bytes = 0;
    w->result = 0;
    if (!bytes || n < 8 || n > ARK_WASM_MODULE_MAX || !entry || strlen(entry) > 127) {
        strcopy(w->error, "ArkOS: invalid module size or entry", sizeof w->error);
        return -1;
    }
    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = env ? m3_NewRuntime(env, 65536, w) : 0;
    IM3Module mod = 0;
    IM3Function fn = 0;
    bool owned = false;
    M3Result e = rt ? m3_SetResourceLimit(rt, c_m3Limit_GasUnits, 1000000000ull)
                    : "ArkOS: allocation failed";
    if (!e)
        e = m3_SetResourceLimit(rt, c_m3Limit_MemoryBytes, 4u * 1024u * 1024u);
    if (!e)
        e = m3_SetResourceLimit(rt, c_m3Limit_TableElements, 4096);
    if (!e)
        e = m3_ParseModule(env, &mod, bytes, (uint32_t)n);
    if (!e) {
        owned = true;
        e = m3_LoadModule(rt, mod);
    }
    if (!e)
        e = link_imports(mod);
    /* Check ALL function bodies, including unreachable exports, before start. */
    if (!e)
        e = m3_CompileModule(mod);
    if (!e) {
        e = m3_FindFunctionIn(&fn, mod, entry);
        if (e == m3Err_functionLookupFailed && !strcmp(entry, "main"))
            e = m3_FindFunctionIn(&fn, mod, "_start");
    }
    if (!e && (m3_GetArgCount(fn) || m3_GetRetCount(fn) > 1))
        e = "ArkOS: entry must take no arguments and return at most one value";
    if (!e)
        e = m3_RunStart(mod);
    if (!e)
        e = m3_CallV(fn);
    if (!e && m3_GetRetCount(fn)) {
        w->has_result = (unsigned)m3_GetRetType(fn, 0);
        uint64_t bits = 0;
        const void *out[] = {&bits};
        e = m3_GetResults(fn, 1, out);
        w->result = (int64_t)bits;
    }
    if (e == terminated)
        e = m3Err_none;
    if (e)
        strcopy(w->error, e, sizeof w->error);
    if (rt) {
        w->gas_used = m3_GetResourceUsage(rt, c_m3Limit_GasUnits);
        w->memory_bytes = m3_GetResourceUsage(rt, c_m3Limit_MemoryBytes);
    }
    if (mod && !owned)
        m3_FreeModule(mod);
    if (rt)
        m3_FreeRuntime(rt);
    if (env)
        m3_FreeEnvironment(env);
    return e ? -1 : (w->exit_code ? 1 : 0);
}
