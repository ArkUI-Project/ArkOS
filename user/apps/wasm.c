#include "app.h"
#include "app_permissions.h"
#include "text.h"
#include "runtime.h"
#include "examples.h"
static ArkApp app;
static uint32_t pixels[800 * 500];
static ArkWasm runtime;
static uint8_t module_bytes[ARK_WASM_MODULE_MAX];
static char target[128] = "builtin:hello", console[8192],
            status[192] = "选择示例，或输入 .wasm 路径 / blob:名称";
static int scroll;
static size_t length = 13, cursor = 13, output_length;
static bool held, pending;
static const struct {
    const char *name;
    const uint8_t *bytes;
    size_t size;
} examples[] = {{"hello", wasm_hello, sizeof wasm_hello},
                {"fibonacci", wasm_fibonacci, sizeof wasm_fibonacci},
                {"memory", wasm_memory, sizeof wasm_memory},
                {"float", wasm_float, sizeof wasm_float},
                {"wasi", wasm_wasi, sizeof wasm_wasi},
                {"out_of_bounds", wasm_out_of_bounds, sizeof wasm_out_of_bounds},
                {"loop", wasm_loop, sizeof wasm_loop}};
static void output(const char *s, size_t n, void *ctx) {
    (void)ctx;
    for (size_t i = 0; i < n && output_length + 1 < sizeof console; i++) {
        unsigned char c = (unsigned char)s[i];
        console[output_length++] = (c == '\n' || c == '\t' || c >= 32) ? (char)c : '?';
    }
    console[output_length] = 0;
    char b[256];
    while (n) {
        size_t part = n < sizeof b - 1 ? n : sizeof b - 1;
        memcpy(b, s, part);
        b[part] = 0;
        app_log(b);
        s += part;
        n -= part;
    }
}
static uint64_t ticks(void *ctx) {
    (void)ctx;
    return ark_call(ARK_SYS_TICKS, 0, 0);
}
static void draw(void) {
    int w = app_width(&app), h = app_height(&app), cols = (w - 48) / 108;
    if (cols < 1)
        cols = 1;
    int top = 211 + (6 / cols) * 40;
    app_clear(&app, 0xf4f6fb);
    app_cursor(&app, 24, 104, w - 184, 42, ARK_CURSOR_TEXT);
    app_text(&app, 24, 20, "WebAssembly", 0x343164, 2);
    app_text(&app, 26, 64, "打开应用文件，或运行下方示例", 0x697b94, 1);
    app_round(&app, 24, 104, w - 184, 42, 10, 0xffffff);
    app_clip(&app, 36, 116, target, w - 208, 0x343164);
    app_button(&app, app_width(&app) - 148, 104, 124, 42, pending ? "等待授权" : "运行", true);
    const char *labels[] = {"Hello", "递归计算", "内存", "浮点", "WASI", "越界保护", "循环保护"};
    for (unsigned i = 0; i < 7; i++)
        app_button(&app, 24 + (int)(i % (unsigned)cols) * 108, 161 + (int)(i / (unsigned)cols) * 40,
                   100, 34, labels[i], false);
    app_round(&app, 24, top, w - 48, h - top - 79, 12, app.night ? 0x0c1320 : 0x19233a);
    const char *p = console;
    int y = top + 14, row = 0;
    while (*p && y < h - 93) {
        char line[128];
        unsigned n = 0;
        while (*p && *p != '\n' && n < sizeof line - 1)
            line[n++] = *p++;
        line[n] = 0;
        if (row++ < scroll) {
            if (*p == '\n')
                p++;
            continue;
        }
        app_clip(&app, 36, y, line, w - 72, 0xf2f5fc);
        if (*p == '\n')
            p++;
        y += 22;
    }
    app_clip(&app, 26, h - 65, status, w - 52, 0x4b5c79);
    app_text(&app, 26, 470, "Enter 运行 · Ctrl+S 保存示例文件", 0x79899f, 1);
    app_text_input(&app, !pending, 36, 116, 20);
    app_present(&app);
}
static int load(const char *source, const uint8_t **bytes, size_t *n) {
    if (!strncmp(source, "builtin:", 8)) {
        for (unsigned i = 0; i < sizeof examples / sizeof examples[0]; i++)
            if (!strcmp(source + 8, examples[i].name)) {
                *bytes = examples[i].bytes;
                *n = examples[i].size;
                return 0;
            }
        strcopy(status, "未知内置示例", sizeof status);
        return -1;
    }
    int64_t grant = app_request_permission(ARK_CAP_FILES);
    if (grant < 0) {
        strcopy(status, grant == -11 ? "请在系统弹窗中授权文件访问" : "文件访问被拒绝",
                sizeof status);
        return grant == -11 ? 1 : -1;
    }
    if (!strncmp(source, "blob:", 5)) {
        ArkBlobRequest r = {.op = ARK_BLOB_READ,
                            .capacity = sizeof module_bytes,
                            .buffer = (uintptr_t)module_bytes};
        strcopy(r.name, source + 5, sizeof r.name);
        for (unsigned i = 0;; i++) {
            ArkBlobRequest meta = {.op = ARK_BLOB_LIST, .index = i};
            if (ark_call(ARK_SYS_BLOB, &meta, sizeof meta) < 0)
                break;
            if (!strcmp(meta.name, r.name)) {
                r.capacity = meta.size;
                break;
            }
        }
        if (r.capacity > sizeof module_bytes || ark_call(ARK_SYS_BLOB, &r, sizeof r) < 0) {
            strcopy(status, r.error[0] ? r.error : "无法读取应用文件", sizeof status);
            return -1;
        }
        *bytes = module_bytes;
        *n = r.count;
        return 0;
    }
    ArkFileRequest r = {.op = ARK_FILE_STAT};
    strcopy(r.path, source, sizeof r.path);
    if (ark_file(&r) < 0 || r.info.flags & ARK_FILE_DIRECTORY || r.info.size < 8 ||
        r.info.size > sizeof module_bytes) {
        strcopy(status, "无法读取模块，或超过 512 KiB", sizeof status);
        return -1;
    }
    size_t size = (size_t)r.info.size, at = 0;
    while (at < size) {
        r.op = ARK_FILE_READ_BYTES;
        r.offset = at;
        r.capacity = (uint32_t)(size - at > ARK_FILE_MAX ? ARK_FILE_MAX : size - at);
        r.buffer = (uintptr_t)(module_bytes + at);
        if (ark_file(&r) < 0 || !r.count || r.count > r.capacity) {
            strcopy(status, "模块读取失败", sizeof status);
            return -1;
        }
        at += r.count;
    }
    *bytes = module_bytes;
    *n = size;
    return 0;
}
static void run(void) {
    char source[128], entry[128] = "main";
    strcopy(source, target, sizeof source);
    for (unsigned i = 0; source[i]; i++)
        if (source[i] == '#') {
            strcopy(entry, source + i + 1, sizeof entry);
            source[i] = 0;
            break;
        }
    const uint8_t *bytes = 0;
    size_t n = 0;
    int r = load(source, &bytes, &n);
    pending = r == 1;
    if (r) {
        draw();
        return;
    }
    output_length = 0;
    console[0] = 0;
    strcopy(status, "正在验证并执行…", sizeof status);
    draw();
    runtime.write = output;
    runtime.ticks = ticks;
    r = ark_wasm_run(&runtime, bytes, n, entry);
    if (r < 0) {
        strcopy(status, runtime.error, sizeof status);
        app_log("[wasm] trap: ");
        app_log(status);
        app_log("\n");
    } else {
        strcopy(status, "执行完成 · ", sizeof status);
        char value[32];
        uint_to_str(runtime.exit_code, value);
        size_t at = strlen(status);
        strcopy(status + at, "exit=", sizeof status - at);
        at = strlen(status);
        strcopy(status + at, value, sizeof status - at);
        if (runtime.has_result) {
            int64_t val = runtime.has_result == 1 ? (int32_t)runtime.result : runtime.result;
            char b[64] = "result=";
            if (runtime.has_result >= 3)
                strcopy(b, "float result bits=", sizeof b);
            size_t pos = strlen(b);
            if (val < 0) {
                b[pos++] = '-';
                uint_to_str((uint64_t)(-(val + 1)) + 1, b + pos);
            } else
                uint_to_str((uint64_t)val, b + pos);
            output("\n", 1, 0);
            output(b, strlen(b), 0);
            output("\n", 1, 0);
        }
        app_log("[wasm] completed ");
        app_log(source);
        app_log(" ");
        app_log(status);
        app_log("\n");
    }
    draw();
}
static void launch(void) {
    ArkLaunchInfo info = {0};
    if (app_launch_info(&info) >= 0 && info.argument[0]) {
        strcopy(target, info.argument, sizeof target);
        length = cursor = strlen(target);
        run();
    }
}
static void save_example(void) {
    int64_t g = app_request_permission(ARK_CAP_FILES);
    if (g < 0) {
        strcopy(status, g == -11 ? "授权后请再按 Ctrl+S" : "文件访问被拒绝", sizeof status);
        return;
    }
    ArkBlobRequest q = {
        .op = ARK_BLOB_WRITE, .buffer = (uintptr_t)wasm_hello, .capacity = sizeof wasm_hello};
    strcopy(q.name, "hello.wasm", sizeof q.name);
    strcopy(status,
            ark_call(ARK_SYS_BLOB, &q, sizeof q) < 0 ? "保存失败"
                                                     : "已保存；输入 blob:hello.wasm 可从磁盘执行",
            sizeof status);
}
int main(void) {
    if (!app_open(&app, "WASM", pixels, 800, 500))
        return 1;
    draw();
    app_log("[app] WASM ring3 ready\n");
    launch();
    for (;;) {
        ArkEvent e;
        bool dirty = false;
        while (app_event(&app, &e)) {
            if (e.type == ARK_EV_OPEN) {
                launch();
                dirty = true;
            }
            if (e.type == ARK_EV_SCROLL) {
                scroll += e.y;
                if (scroll < 0)
                    scroll = 0;
                dirty = true;
            }
            if (e.type == ARK_EV_TEXT && !pending) {
                app_codepoint(target, &length, &cursor, sizeof target, (uint32_t)e.key);
                dirty = true;
            }
            if (e.type == ARK_EV_KEY) {
                if (e.key == KEY_ENTER)
                    run();
                else if (e.key == 19)
                    save_example();
                else if (!pending)
                    app_edit(target, &length, &cursor, sizeof target, e.key);
                dirty = true;
            }
            if (e.type == ARK_EV_POINTER) {
                bool down = e.buttons & 1;
                if (app_released(&app, &e)) {
                    int cols = (app_width(&app) - 48) / 108;
                    if (cols < 1)
                        cols = 1;
                    if (app_click_hit(&app, &e, app_width(&app) - 148, 104, 124, 42))
                        run();
                    for (unsigned i = 0; i < 7; i++)
                        if (app_click_hit(&app, &e, 24 + (int)(i % (unsigned)cols) * 108,
                                          161 + (int)(i / (unsigned)cols) * 40, 100, 34)) {
                            strcopy(target, "builtin:", sizeof target);
                            strcopy(target + 8, examples[i].name, sizeof target - 8);
                            length = cursor = strlen(target);
                            run();
                        }
                    dirty = true;
                }
                held = down;
            }
        }
        if (pending)
            run();
        else if (dirty || app.dirty)
            draw();
        app_wait(pending ? 10 : 100);
    }
}
