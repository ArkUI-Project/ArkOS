#include "ark.h"
#include "storage.h"
#include "extfs.h"
#include "shell_extra.h"
#include "ark_api.h"
#include "ark_catalog.h"
#include "package.h"

/* ArkOS command interpreter with a bounded native application launcher. */
#define COMMAND_CAP 1024
#define PIPE_CAP 32768
#define STAGES_MAX 8
#define ARGS_MAX 48
#define HISTORY_MAX 32
#define SCRIPT_DEPTH 4
#define SCRIPT_LINES 64

static ShellContext default_context;
static ShellContext *current_context = &default_context;
ShellContext *shell_context(void) {
    return current_context;
}
void shell_switch(ShellContext *context) {
    current_context = context ? context : &default_context;
}
static uint64_t boot_memory_mib;
#define cwd (current_context->cwd)
static char session_home[128] = "/home/ark";
void shell_set_home(const char *home) {
    strcopy(session_home, home && home[0] ? home : "/home/ark", sizeof session_home);
}
static char prompt_text[136];
#define history (current_context->history)
#define history_count (current_context->history_count)
#define history_next (current_context->history_next)
static char pipe_text[2][PIPE_CAP + 1];
static char script_text[SCRIPT_DEPTH][VFS_FILE_CAP];
static unsigned script_depth;

typedef struct {
    char *argv[ARGS_MAX];
    int argc;
} Stage;
typedef struct {
    char words[COMMAND_CAP];
    Stage stage[STAGES_MAX];
    int count;
    char *redirect;
    bool append;
} Parsed;
typedef struct {
    char *data;
    size_t size;
    bool overflow;
} Output;

static void add(char *out, size_t cap, const char *text) {
    size_t n = strlen(out);
    if (n < cap)
        strcopy(out + n, text, cap - n);
}
static void number(char *out, size_t cap, uint64_t n) {
    char text[24];
    uint_to_str(n, text);
    add(out, cap, text);
}
static bool fail(const char *message) {
    shell_print(message);
    return false;
}
static bool fs_failure(const char *path, const char *fallback) {
    return fail(extfs_path(path) ? vfs_error() : fallback);
}
static bool white(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static bool same(const char *a, const char *b) {
    return !strcmp(a, b);
}
static char *new_line(void) {
    if (shell_line_count >= SHELL_LINES) {
        memmove(shell_lines[0], shell_lines[1], (SHELL_LINES - 1) * sizeof(shell_lines[0]));
        shell_line_count = SHELL_LINES - 1;
    }
    char *line = shell_lines[shell_line_count++];
    memset(line, 0, SHELL_COLS);
    return line;
}

/* Keep UTF-8 sequences intact across the bounded display-row buffer. */
void shell_print(const char *text) {
    if (!text)
        return;
    serial_write(text);
    size_t total = strlen(text);
    if (!total || text[total - 1] != '\n')
        serial_write("\n");
    char *line = new_line();
    size_t column = 0;
    for (size_t i = 0; text[i];) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r') {
            ++i;
            continue;
        }
        if (c == '\n') {
            ++i;
            if (text[i])
                line = new_line();
            column = 0;
            continue;
        }
        if (c == '\t') {
            unsigned spaces = 4u - (unsigned)(column % 4u);
            while (spaces--) {
                if (column == SHELL_COLS - 1) {
                    line = new_line();
                    column = 0;
                }
                line[column++] = ' ';
                line[column] = 0;
            }
            ++i;
            continue;
        }
        size_t width = c >= 0xc2 && c <= 0xdf   ? 2
                       : c >= 0xe0 && c <= 0xef ? 3
                       : c >= 0xf0 && c <= 0xf4 ? 4
                                                : 1;
        for (size_t j = 1; j < width; ++j) {
            if (((unsigned char)text[i + j] & 0xc0) != 0x80) {
                width = 1;
                break;
            }
        }
        if (column + width >= SHELL_COLS) {
            line = new_line();
            column = 0;
        }
        if (c < 32) {
            line[column++] = '?';
            ++i;
        } else {
            memcpy(line + column, text + i, width);
            column += width;
            i += width;
        }
        line[column] = 0;
    }
}

const char *shell_cwd(void) {
    return cwd;
}
const char *shell_prompt(void) {
    strcopy(prompt_text, "ark:", sizeof(prompt_text));
    add(prompt_text, sizeof(prompt_text), cwd);
    add(prompt_text, sizeof(prompt_text), "$ ");
    return prompt_text;
}
const char *shell_history_get(int age) {
    if (age < 0 || (unsigned)age >= history_count)
        return 0;
    return history[(history_next + HISTORY_MAX - 1u - (unsigned)age) % HISTORY_MAX];
}

static void putn(Output *out, const char *text, size_t n) {
    if (out->overflow)
        return;
    if (n > PIPE_CAP - out->size) {
        out->overflow = true;
        return;
    }
    memcpy(out->data + out->size, text, n);
    out->size += n;
    out->data[out->size] = 0;
}
static void put(Output *out, const char *text) {
    putn(out, text, strlen(text));
}
static void line(Output *out, const char *text) {
    put(out, text);
    put(out, "\n");
}
static void num(Output *out, uint64_t n) {
    char text[24];
    uint_to_str(n, text);
    put(out, text);
}
static bool usage(bool valid, const char *text) {
    return valid || fail(text);
}

/* Normalize from the shell's current directory, without touching storage defaults. */
static bool path(const char *name, char out[128]) {
    if (!name || !*name)
        return fail("Path must not be empty.");
    if (*name == '/')
        strcopy(out, "/", 128);
    else
        strcopy(out, cwd, 128);
    const char *p = name;
    while (*p) {
        while (*p == '/')
            ++p;
        if (!*p)
            break;
        const char *begin = p;
        while (*p && *p != '/')
            ++p;
        size_t count = (size_t)(p - begin);
        if (count == 1 && begin[0] == '.')
            continue;
        if (count == 2 && begin[0] == '.' && begin[1] == '.') {
            size_t n = strlen(out);
            while (n > 1 && out[n - 1] != '/')
                --n;
            if (n > 1)
                --n;
            out[n] = 0;
            continue;
        }
        size_t n = strlen(out);
        bool slash = n > 1;
        if (n + (slash ? 1u : 0u) + count >= 128)
            return fail("Path too long (127 UTF-8 bytes maximum).");
        if (slash)
            out[n++] = '/';
        memcpy(out + n, begin, count);
        out[n + count] = 0;
    }
    return true;
}
static bool directory(const char *name) {
    int i = vfs_find(name);
    return same(name, "/") || (i >= 0 && vfs_entry(i)->is_dir);
}
static const char *basename(const char *name) {
    const char *last = name;
    for (const char *p = name; *p; ++p)
        if (*p == '/' && p[1])
            last = p + 1;
    return last;
}
static int file_arg(const char *arg) {
    char full[128];
    if (!path(arg, full))
        return -1;
    int i = vfs_find(full);
    if (i < 0) {
        fail("File not found.");
        return -1;
    }
    if (vfs_entry(i)->is_dir) {
        fail("Expected a file, received a directory.");
        return -1;
    }
    if (!vfs_read(i)) {
        fail(vfs_error());
        return -1;
    }
    return i;
}
static bool destination(const char *arg, const char *source, char out[128]) {
    if (!path(arg, out))
        return false;
    if (directory(out)) {
        const char *base = basename(source);
        size_t n = strlen(out), b = strlen(base);
        if (n + (n > 1 ? 1u : 0u) + b >= 128)
            return fail("Destination path too long.");
        if (n > 1)
            out[n++] = '/';
        memcpy(out + n, base, b + 1);
    }
    return true;
}

/* Operators are recognized only outside quotes. Backslash quotes the next byte. */
static bool parse(const char *input, Parsed *p) {
    memset(p, 0, sizeof(*p));
    p->count = 1;
    size_t n = 0;
    const char *s = input;
    bool need_redirect = false, after_redirect = false;
    while (*s) {
        while (white(*s))
            ++s;
        if (!*s || *s == '#')
            break;
        if (*s == '|') {
            if (need_redirect || after_redirect || !p->stage[p->count - 1].argc)
                return fail("Syntax error: empty stage or redirection before pipe.");
            if (p->count == STAGES_MAX)
                return fail("Too many pipeline stages (maximum 8).");
            ++p->count;
            ++s;
            continue;
        }
        if (*s == '>') {
            if (need_redirect || p->redirect || !p->stage[p->count - 1].argc)
                return fail("Syntax error: redirection requires one final destination.");
            ++s;
            p->append = *s == '>';
            if (p->append)
                ++s;
            need_redirect = true;
            continue;
        }
        if (after_redirect)
            return fail("Syntax error: redirection must be last.");
        char *word = p->words + n;
        char quote = 0;
        while (*s) {
            if (!quote && (white(*s) || *s == '|' || *s == '>'))
                break;
            if (*s == '\\' && quote != '\'') {
                ++s;
                if (!*s)
                    return fail("Syntax error: trailing backslash.");
                p->words[n++] = *s++;
            } else if (*s == '\'' || *s == '"') {
                if (!quote)
                    quote = *s++;
                else if (quote == *s) {
                    quote = 0;
                    ++s;
                } else
                    p->words[n++] = *s++;
            } else
                p->words[n++] = *s++;
            if (n >= sizeof(p->words) - 1)
                return fail("Parsed command too long.");
        }
        if (quote)
            return fail("Syntax error: unclosed quote.");
        p->words[n++] = 0;
        if (need_redirect) {
            p->redirect = word;
            need_redirect = false;
            after_redirect = true;
        } else {
            Stage *stage = &p->stage[p->count - 1];
            if (stage->argc == ARGS_MAX)
                return fail("Too many arguments (maximum 48 per stage).");
            stage->argv[stage->argc++] = word;
        }
    }
    if (need_redirect || (p->count > 1 && !p->stage[p->count - 1].argc))
        return fail("Syntax error: incomplete command.");
    return true;
}

static bool child_of(const char *entry, const char *parent) {
    size_t n = strlen(parent);
    const char *rest;
    if (same(parent, "/")) {
        if (*entry != '/')
            return false;
        rest = entry + 1;
    } else {
        if (strncmp(entry, parent, n) || entry[n] != '/')
            return false;
        rest = entry + n + 1;
    }
    if (!*rest)
        return false;
    while (*rest)
        if (*rest++ == '/')
            return false;
    return true;
}
static void listing(Output *out, const VFile *file, bool detailed) {
    if (detailed) {
        put(out, file->is_dir ? "d " : "- ");
        num(out, file->size);
        put(out, "\t");
    }
    put(out, basename(file->name));
    if (file->is_dir)
        put(out, "/");
    put(out, "\n");
}
static bool unsigned_value(const char *s, size_t *value) {
    if (!*s)
        return false;
    size_t v = 0;
    while (*s) {
        if (*s < '0' || *s > '9' || v > 1000000u)
            return false;
        v = v * 10 + (unsigned)(*s++ - '0');
    }
    *value = v;
    return true;
}
static bool input_source(Stage *s, int first, const char *input, const char **text) {
    if (s->argc == first) {
        *text = input;
        return true;
    }
    if (s->argc != first + 1)
        return fail("Expected at most one input file.");
    int i = file_arg(s->argv[first]);
    if (i < 0)
        return false;
    *text = vfs_entry(i)->data;
    return true;
}
static bool contains(const char *line_text, size_t length, const char *pattern) {
    size_t n = strlen(pattern);
    if (n > length)
        return false;
    for (size_t i = 0; i + n <= length; ++i)
        if (!strncmp(line_text + i, pattern, n))
            return true;
    return false;
}
static bool execute(const char *command, bool announce);
static bool script(const char *arg) {
    if (script_depth >= SCRIPT_DEPTH)
        return fail("Script nesting limit reached (4).");
    int index = file_arg(arg);
    if (index < 0)
        return false;
    char *text = script_text[script_depth++];
    strcopy(text, vfs_entry(index)->data, VFS_FILE_CAP);
    unsigned lines = 0;
    bool ok = true;
    while (*text) {
        if (++lines > SCRIPT_LINES) {
            ok = fail("Script line limit reached (64).");
            break;
        }
        char *start = text;
        while (*text && *text != '\n')
            ++text;
        if (*text)
            *text++ = 0;
        size_t n = strlen(start);
        if (n && start[n - 1] == '\r')
            start[n - 1] = 0;
        if (!execute(start, false)) {
            char msg[80] = "Script stopped at line ";
            number(msg, sizeof(msg), lines);
            add(msg, sizeof(msg), ".");
            shell_print(msg);
            ok = false;
            break;
        }
    }
    --script_depth;
    return ok;
}

static bool builtin(Stage *s, const char *input, Output *out) {
    char **a = s->argv;
    const char *cmd = a[0];
    int argc = s->argc;

    if (same(cmd, "stat")) {
        if (!usage(argc == 2, "Usage: stat FILE"))
            return false;
        int i = file_arg(a[1]);
        if (i < 0)
            return false;
        const VFile *f = vfs_entry(i);
        line(out, f->name);
        put(out, "bytes: ");
        num(out, f->size);
        line(out, "");
        return true;
    }
    if (same(cmd, "find")) {
        if (!usage(argc <= 2, "Usage: find [DIR]"))
            return false;
        char p[128];
        if (!path(argc == 2 ? a[1] : cwd, p))
            return false;
        size_t n = strlen(p);
        for (int i = 0; i < 128; i++) {
            const VFile *f = vfs_entry(i);
            if (f && f->used && !strncmp(f->name, p, n) &&
                (n == 1 || !f->name[n] || f->name[n] == '/'))
                line(out, f->name);
        }
        return true;
    }
    if (same(cmd, "sort") || same(cmd, "uniq")) {
        const char *source;
        if (!input_source(s, 1, input, &source))
            return false;
        static char data[PIPE_CAP + 1];
        static char *rows[1024];
        strcopy(data, source, sizeof data);
        unsigned count = 0;
        char *p = data;
        while (*p) {
            if (count == 1024)
                return fail("At most 1024 lines.");
            rows[count++] = p;
            while (*p && *p != '\n')
                p++;
            if (*p)
                *p++ = 0;
        }
        if (same(cmd, "sort"))
            for (unsigned i = 1; i < count; i++) {
                char *row = rows[i];
                unsigned j = i;
                while (j && strcmp(rows[j - 1], row) > 0) {
                    rows[j] = rows[j - 1];
                    j--;
                }
                rows[j] = row;
            }
        for (unsigned i = 0; i < count; i++)
            if (same(cmd, "sort") || !i || strcmp(rows[i], rows[i - 1]))
                line(out, rows[i]);
        return true;
    }
#ifndef ARK_STORAGE_HOST_TEST
    if (same(cmd, "ps") || same(cmd, "lscpu")) {
        ArkTaskInfo tasks[32];
        ArkTaskRequest q = {.op = ARK_TASK_LIST, .buffer = (uintptr_t)tasks, .capacity = 32};
        if (ark_tasks(&q) < 0)
            return fail("Task service denied.");
        put(out, "Online CPUs: ");
        num(out, q.online_cpus);
        line(out, "");
        if (same(cmd, "ps")) {
            line(out, "TID  PID  UID  STATE  KiB  NAME");
            for (unsigned i = 0; i < q.count; i++) {
                num(out, tasks[i].tid);
                put(out, "  ");
                num(out, tasks[i].pid);
                put(out, "  ");
                num(out, tasks[i].uid);
                put(out, "  ");
                num(out, tasks[i].state);
                put(out, "  ");
                num(out, tasks[i].mapped_bytes / 1024);
                put(out, "  ");
                line(out, tasks[i].name);
            }
        }
        return true;
    }
    if (same(cmd, "kill")) {
        size_t tid;
        if (!usage(argc == 2 && unsigned_value(a[1], &tid) && tid <= UINT32_MAX, "Usage: kill TID"))
            return false;
        ArkTaskRequest q = {.op = ARK_TASK_KILL, .tid = (uint32_t)tid};
        return ark_tasks(&q) >= 0 || fail("Task protected or not found.");
    }
    if (same(cmd, "id") || same(cmd, "whoami")) {
        ArkAccountRequest q = {0};
        if (ark_account(&q) < 0)
            return fail("No active user.");
        if (same(cmd, "id")) {
            put(out, "uid=");
            num(out, q.uid);
            put(out, " ");
        }
        line(out, q.user);
        return true;
    }
    if (same(cmd, "wasm")) {
        if (!usage(argc <= 2, "Usage: wasm [FILE.wasm | builtin:hello | blob:NAME][#export]"))
            return false;
        ArkSpawn q = {0};
        strcopy(q.program, "wasm", sizeof q.program);
        if (argc == 2) {
            if (!strncmp(a[1], "builtin:", 8) || !strncmp(a[1], "blob:", 5))
                strcopy(q.argument, a[1], sizeof q.argument);
            else if (!path(a[1], q.argument))
                return false;
        }
        if (ark_spawn(&q) < 0)
            return fail("WASM app permission denied or process limit.");
        shell_action = 15;
        line(out, "WASM started in an independent ArkOS process.");
        return true;
    }
    if (same(cmd, "run")) {
        if (!usage(argc == 2 || argc == 3, "Usage: run APP [argument]"))
            return false;
        ArkSpawn q = {0};
        strcopy(q.program, a[1], sizeof q.program);
        if (argc == 3)
            strcopy(q.argument, a[2], sizeof q.argument);
        if (ark_spawn(&q) < 0)
            return fail("Unknown app, permission denied or process limit.");
        put(out, "PID ");
        num(out, q.pid);
        line(out, "");
        for (unsigned i = 0; i < ARK_CATALOG_COUNT; i++)
            if (same(q.program, ark_catalog[i].program) || same(q.program, ark_catalog[i].title))
                shell_action = (int)ark_catalog[i].desktop;
        return true;
    }
    if (same(cmd, "dev")) {
        /* Device inventory and loadable .arco drivers. Listing needs only an
         * active session; install and remove need SYSTEM plus an admin
         * account, and the kernel re-verifies every image against the
         * protected manifest before it maps or runs a byte. */
        ArkDeviceRequest d = {0};
        if (argc == 1 || same(a[1], "list")) {
            unsigned count = 0;
            for (unsigned i = 0;; i++) {
                d = (ArkDeviceRequest){.op = ARK_DEV_ENUMERATE, .index = i};
                int64_t r = ark_device(&d);
                if (r == -2)
                    break;
                if (r < 0)
                    return fail(d.error[0] ? d.error : "Device inventory is unavailable.");
                count++;
                put(out, "dev ");
                num(out, i);
                put(out, " ");
                line(out, d.info.name);
                put(out, "    class ");
                num(out, d.info.class_id);
                put(out, "  state ");
                num(out, d.info.state);
                put(out, "  ");
                line(out, d.info.detail);
            }
            if (!count)
                line(out, "No devices reported.");
            return true;
        }
        ArkDriverRequest q = {0};
        if (same(a[1], "drivers")) {
            unsigned count = 0;
            for (unsigned i = 0;; i++) {
                q = (ArkDriverRequest){.op = ARK_DRV_LIST, .index = i};
                int64_t r = ark_driver(&q);
                if (r == -2)
                    break;
                if (r < 0)
                    return fail(q.error[0] ? q.error : "Driver list is unavailable.");
                count++;
                put(out, q.info.name);
                put(out, "  v");
                num(out, q.info.version >> 16);
                put(out, ".");
                num(out, (q.info.version >> 8) & 255u);
                put(out, ".");
                num(out, q.info.version & 255u);
                put(out, "  ");
                line(out, q.info.state == ARK_DRV_STATE_LOADED ? "loaded" :
                            q.info.state == ARK_DRV_STATE_DISABLED ? "disabled" : "failed");
                put(out, "    ");
                line(out, q.info.detail);
            }
            if (!count)
                line(out, "No loadable drivers installed.");
            return true;
        }
        if (same(a[1], "query")) {
            if (!usage(argc == 3, "Usage: dev query NAME"))
                return false;
            q = (ArkDriverRequest){.op = ARK_DRV_QUERY};
            strcopy(q.name, a[2], sizeof q.name);
            if (ark_driver(&q) < 0)
                return fail(q.error[0] ? q.error : "Driver is not installed.");
            put(out, q.info.name);
            put(out, "  state ");
            num(out, q.info.state);
            put(out, "  bytes ");
            num(out, q.info.image_bytes);
            line(out, "");
            put(out, "sha256 ");
            char hex[65];
            for (unsigned i = 0; i < 32; i++)
                for (unsigned j = 0; j < 2; j++) {
                    unsigned nibble = (q.info.sha256[i] >> (j ? 0 : 4)) & 15u;
                    hex[i * 2 + j] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
                }
            hex[64] = 0;
            line(out, hex);
            return true;
        }
        if (same(a[1], "install")) {
            if (!usage(argc == 3, "Usage: dev install PATH.arco|blob:NAME"))
                return false;
            q = (ArkDriverRequest){.op = ARK_DRV_INSTALL};
            strcopy(q.path, a[2], sizeof q.path);
            if (ark_driver(&q) < 0)
                return fail(q.error[0] ? q.error : "Driver installation refused.");
            put(out, "Installed ");
            line(out, q.info.name);
            return true;
        }
        if (same(a[1], "remove")) {
            if (!usage(argc == 3, "Usage: dev remove NAME"))
                return false;
            q = (ArkDriverRequest){.op = ARK_DRV_REMOVE};
            strcopy(q.name, a[2], sizeof q.name);
            if (ark_driver(&q) < 0)
                return fail(q.error[0] ? q.error : "Driver removal refused.");
            put(out, "Removed ");
            line(out, q.name);
            return true;
        }
        if (same(a[1], "blk")) {
            /* `dev blk UNIT read|write LBA COUNT` drives a real block transfer
             * for diagnostics and driver gates. WRITE is system-only; the
             * kernel re-checks the node, the range and the ArkFS-system-volume
             * exclusion, so this cannot touch the running system disk. */
            if (!usage(argc == 6, "Usage: dev blk UNIT read|write LBA COUNT"))
                return false;
            size_t unit, lba, sectors;
            if (!unsigned_value(a[2], &unit) || unit > 1 || !unsigned_value(a[4], &lba) ||
                !unsigned_value(a[5], &sectors) || !sectors || sectors > ARK_DEV_READ_SECTORS)
                return fail("dev blk: bad unit, LBA or sector count.");
            bool write = same(a[3], "write");
            if (!write && !same(a[3], "read"))
                return fail("Usage: dev blk UNIT read|write LBA COUNT");
            uint32_t index = UINT32_MAX;
            for (unsigned i = 0;; i++) {
                ArkDeviceRequest scan = {.op = ARK_DEV_ENUMERATE, .index = i};
                int64_t r = ark_device(&scan);
                if (r == -2)
                    break;
                if (r < 0)
                    return fail(scan.error[0] ? scan.error : "Device inventory is unavailable.");
                if (scan.info.class_id == ARK_DEV_CLASS_BLOCK && scan.info.unit == unit) {
                    index = scan.info.index;
                    break;
                }
            }
            if (index == UINT32_MAX)
                return fail("dev blk: no such block unit.");
            static uint8_t io[ARK_DEV_READ_SECTORS * 512u];
            memset(io, 0, sizeof io);
            ArkDeviceRequest d = {0};
            d.op = write ? ARK_DEV_WRITE : ARK_DEV_READ;
            d.index = index;
            d.buffer = (uint64_t)(uintptr_t)io;
            d.offset = lba;
            d.sectors = (uint32_t)sectors;
            d.capacity = (uint32_t)(sectors * 512u);
            if (ark_device(&d) < 0)
                return fail(d.error[0] ? d.error : "Block request failed.");
            put(out, write ? "Wrote " : "Read ");
            num(out, d.count);
            line(out, " sectors.");
            return true;
        }
        return fail("Usage: dev [list|drivers|query NAME|install PATH|remove NAME|blk UNIT read|write LBA COUNT]");
    }
    if (same(cmd, "pkg")) {
        if (!usage(argc >= 2, "Usage: pkg list|info|install|upgrade|remove|grant|run ..."))
            return false;
        ArkPackageRequest q = {0};
        if (same(a[1], "list")) {
            if (argc != 2)
                return fail("Usage: pkg list");
            unsigned count = 0;
            for (unsigned i = 0;; i++) {
                q = (ArkPackageRequest){.op = ARK_PACKAGE_LIST, .index = i};
                int64_t r = ark_package(&q);
                if (r == -2)
                    break;
                if (r < 0)
                    return fail(q.error);
                count++;
                put(out, q.info.id);
                put(out, " ");
                num(out, q.info.major);
                put(out, ".");
                num(out, q.info.minor);
                put(out, ".");
                num(out, q.info.patch);
                put(out, "  ");
                line(out, q.info.title);
            }
            if (!count)
                line(out, "No installed packages.");
            return true;
        }
        if (!usage(argc == 3 || argc == 4, "Usage: pkg info|install|upgrade FILE.arkpkg; pkg "
                                           "remove|run ID; pkg grant ID MASK"))
            return false;
        if (same(a[1], "run")) {
            q = (ArkPackageRequest){.op = ARK_PACKAGE_FIND};
            strcopy(q.id, a[2], sizeof q.id);
            if (ark_package(&q) < 0)
                return fail("Package is not installed.");
            if (q.info.installed == 2) {
                shell_action = (int)q.info.slot;
                line(out, "Opening application.");
                return true;
            }
            ArkSpawn r = {0};
            strcopy(r.program, "pkg.", sizeof r.program);
            strcopy(r.program + 4, a[2], sizeof r.program - 4);
            if (argc == 4)
                strcopy(r.argument, a[3], sizeof r.argument);
            if (ark_spawn(&r) < 0)
                return fail(r.error[0] ? r.error : "Application cannot be started.");
            shell_action = ARK_PACKAGE_DESKTOP_FIRST + (int)q.info.slot;
            put(out, "PID ");
            num(out, r.pid);
            line(out, "");
            return true;
        }
        if (same(a[1], "info") || same(a[1], "install") || same(a[1], "upgrade")) {
            if (argc != 3)
                return fail("Package file operation expects one source.");
            q.op = same(a[1], "info")      ? ARK_PACKAGE_INSPECT
                   : same(a[1], "install") ? ARK_PACKAGE_INSTALL
                                           : ARK_PACKAGE_UPGRADE;
            if (!strncmp(a[2], "blob:", 5))
                strcopy(q.path, a[2], sizeof q.path);
            else if (!path(a[2], q.path))
                return false;
        } else {
            strcopy(q.id, a[2], sizeof q.id);
            if (same(a[1], "remove") && argc == 3)
                q.op = ARK_PACKAGE_REMOVE;
            else if (same(a[1], "grant") && argc == 4) {
                q.op = ARK_PACKAGE_GRANTS;
                unsigned mask = 0;
                for (const char *p = a[3]; *p; p++) {
                    if (*p < '0' || *p > '9' || mask > 46)
                        return fail(
                            "Permission mask must be decimal: UI=4 files=2 network=8 activity=32.");
                    mask = mask * 10 + (unsigned)(*p - '0');
                }
                q.grants = mask;
            } else
                return fail("Unknown package operation or invalid arguments.");
        }
        if (ark_package(&q) < 0)
            return fail(q.error[0] ? q.error : "Package operation failed.");
        put(out, q.info.id);
        put(out, " ");
        num(out, q.info.major);
        put(out, ".");
        num(out, q.info.minor);
        put(out, ".");
        num(out, q.info.patch);
        put(out, " permissions=");
        num(out, q.info.grants);
        put(out, " declared=");
        num(out, q.info.maximum);
        line(out, "");
        if (q.op == ARK_PACKAGE_INSPECT) {
            line(out, q.info.title);
            line(out, q.info.summary);
            line(out, "Local unsigned package; install starts with UI permission only.");
        }
        return true;
    }
    if (same(cmd, "reg")) {
        if (!usage(argc >= 3, "Usage: reg get|list|delete KEY | reg set KEY int|string VALUE"))
            return false;
        ArkRegistryRequest q = {0};
        if (strlen(a[2]) >= sizeof q.key)
            return fail("Configuration key is too long.");
        strcopy(q.key, a[2], 128);
        if (same(a[1], "get") && argc == 3)
            q.op = ARK_REG_GET;
        else if (same(a[1], "list") && argc == 3)
            q.op = ARK_REG_LIST;
        else if (same(a[1], "delete") && argc == 3)
            q.op = ARK_REG_DELETE;
        else if (same(a[1], "set") && argc == 5) {
            q.op = ARK_REG_SET;
            if (same(a[3], "int")) {
                uint64_t value = 0;
                bool negative = a[4][0] == '-';
                const char *digits = a[4] + negative;
                uint64_t limit = (uint64_t)INT64_MAX + (negative ? 1u : 0u);
                if (!*digits)
                    return fail("Integer value is empty.");
                for (; *digits; digits++) {
                    if (*digits < '0' || *digits > '9' ||
                        value > (limit - (unsigned)(*digits - '0')) / 10)
                        return fail("Integer is outside the signed 64-bit range.");
                    value = value * 10 + (unsigned)(*digits - '0');
                }
                q.type = ARK_REG_INTEGER;
                q.integer = negative ? (int64_t)(0u - (uint64_t)value) : (int64_t)value;
            } else if (same(a[3], "string")) {
                q.type = ARK_REG_STRING;
                q.length = (unsigned)strlen(a[4]);
                if (q.length >= 512)
                    return fail("String exceeds 511 bytes.");
                memcpy(q.value, a[4], q.length);
            } else
                return fail("Value type must be int or string.");
        } else
            return fail("Usage: reg get|list|delete KEY | reg set KEY int|string VALUE");
        unsigned op = q.op, index = 0;
        char prefix[128];
        strcopy(prefix, q.key, 128);
        do {
            int64_t result = ark_registry(&q);
            if (result == -2 && op == ARK_REG_LIST)
                break;
            if (result < 0)
                return fail(q.error[0]
                                ? q.error
                                : "Configuration key missing, access denied or disk unavailable.");
            if (op == ARK_REG_SET || op == ARK_REG_DELETE) {
                line(out, "Configuration saved.");
                break;
            }
            put(out, q.key);
            put(out, " = ");
            if (q.type == ARK_REG_INTEGER) {
                if (q.integer < 0) {
                    put(out, "-");
                    num(out, 0u - (uint64_t)q.integer);
                } else
                    num(out, (uint64_t)q.integer);
            } else if (q.type == ARK_REG_STRING) {
                char value[513];
                memcpy(value, q.value, q.length);
                value[q.length] = 0;
                put(out, value);
            } else {
                put(out, "binary ");
                num(out, q.length);
                put(out, " bytes");
            }
            line(out, "");
            q = (ArkRegistryRequest){.op = op, .index = ++index};
            strcopy(q.key, prefix, 128);
        } while (op == ARK_REG_LIST);
        return true;
    }
    if (same(cmd, "net")) {
        ArkNetworkRequest q = {0};
        if (ark_network(&q) < 0)
            return fail("Network service denied.");
        line(out, q.present ? "E1000 native device" : "No supported NIC");
        line(out, q.link ? "link up" : "link down");
        put(out, "IPv4 ");
        for (unsigned i = 0; i < 4; i++) {
            num(out, (q.ipv4 >> (24 - i * 8)) & 255);
            if (i < 3)
                put(out, ".");
        }
        line(out, "");
        put(out, "RX ");
        num(out, q.rx_packets);
        put(out, " TX ");
        num(out, q.tx_packets);
        line(out, "");
        return true;
    }
    if (same(cmd, "blobs")) {
        for (unsigned i = 0;; i++) {
            ArkBlobRequest q = {.op = ARK_BLOB_LIST, .index = i};
            if (ark_call(ARK_SYS_BLOB, &q, sizeof q) < 0)
                break;
            num(out, q.size);
            put(out, "  ");
            line(out, q.name);
        }
        return true;
    }

#endif
    if (same(cmd, "help")) {
        if (!usage(argc == 1, "Usage: help"))
            return false;
        line(out, "ArkOS native shell 0.12 - built-in commands");
        line(out, "help | about | uname [-a|-r] | clear | history");
        line(out, "pwd | cd [directory] | ls [-l] [path]");
        line(out, "mkdir <dir> | rmdir <dir> | touch <file> | rm <file>");
        line(out, "cat [file...] | write <file> [text...] | append <file> [text...]");
        line(out, "cp <source> <dest> | mv <source> <dest> | echo [text...]");
        line(out, "head|tail [-n count] [file] | grep [-n] [-v] <text> [file]");
        line(out, "wc [-l|-w|-c] [file] | sh <script> (64 lines, depth 4)");
        line(out, "df | mounts | sync | mem | uptime | date");
        line(out, "open "
                  "files|notes|settings|about|browser|tasks|capture|installer|todo|timer|wasm|"
                  "calendar|reminders");
        line(out, "wasm [FILE.wasm | builtin:hello | blob:NAME][#export]");
        line(out, "ps | kill TID | id | whoami | lscpu | net | blobs | run <native-app>");
        line(out, "pkg list|info|install|upgrade|remove|grant|run (ArkPkg v1)");
        line(out, "reg get|list|delete KEY | reg set KEY int|string VALUE");
        line(out, "stat FILE | find [DIR] | sort [FILE] | uniq [FILE]");
        line(out, "reboot | shutdown");
        line(out, "Quotes, backslash escapes, > and >>, up to 8 pipeline stages.");
        line(out, "Example: cat hello.txt | grep Ark | wc -l");
        line(out, "Native text pipelines: 32768 bytes; file data: 16383 bytes.");
        line(out, "Home: 64 entries; external cache: 64. Text view limit: 16383 bytes.");
        line(out, "External paths: /mnt/fat32 (read/write), /mnt/ntfs (read-only).");
        line(out, "Native ELF applications use ArkPkg; text pipelines are bounded.");
        return true;
    }
    if (same(cmd, "about")) {
        if (!usage(argc == 1, "Usage: about"))
            return false;
        line(out, "ArkOS 0.13.0+mouse1 - independent experimental x86_64 kernel");
        line(out, "Native desktop, UTF-8 text, storage and an integrated command interpreter.");
        line(out, "Ring3 process isolation, capability checks, native SMP. No Linux ABI.");
        line(out, storage_status());
        return true;
    }
    if (same(cmd, "uname")) {
        if (!usage(argc == 1 || (argc == 2 && (same(a[1], "-a") || same(a[1], "-r"))),
                   "Usage: uname [-a|-r]"))
            return false;
        line(out,
             argc == 2 && same(a[1], "-r") ? "0.13.0+mouse1" : "ArkOS 0.13.0+mouse1 x86_64 native");
        return true;
    }
    if (same(cmd, "echo")) {
        for (int i = 1; i < argc; ++i) {
            if (i > 1)
                put(out, " ");
            put(out, a[i]);
        }
        put(out, "\n");
        return true;
    }
    if (same(cmd, "pwd")) {
        if (!usage(argc == 1, "Usage: pwd"))
            return false;
        line(out, cwd);
        return true;
    }
    if (same(cmd, "cd")) {
        if (!usage(argc <= 2, "Usage: cd [directory]"))
            return false;
        char full[128];
        if (!path(argc == 2 ? a[1] : session_home, full))
            return false;
        if (!directory(full))
            return fail("Directory not found.");
        strcopy(cwd, full, sizeof(cwd));
        return true;
    }
    if (same(cmd, "ls")) {
        bool detailed = argc > 1 && same(a[1], "-l");
        int first = detailed ? 2 : 1;
        if (!usage(argc <= first + 1, "Usage: ls [-l] [path]"))
            return false;
        char full[128];
        if (!path(argc == first + 1 ? a[first] : cwd, full))
            return false;
        int index = vfs_find(full);
        if (index < 0 && !same(full, "/"))
            return fail("Path not found.");
        if (!directory(full))
            listing(out, vfs_entry(index), detailed);
        else {
            if (!vfs_list(full))
                return fail(vfs_error());
            for (int i = 0; i < vfs_entry_limit(); ++i)
                if (vfs_entry(i)->used && child_of(vfs_entry(i)->name, full))
                    listing(out, vfs_entry(i), detailed);
        }
        return true;
    }
    if (same(cmd, "touch") || same(cmd, "mkdir") || same(cmd, "rm") || same(cmd, "rmdir")) {
        if (!usage(argc == 2, "Usage: touch|mkdir|rm|rmdir <path>"))
            return false;
        char full[128];
        if (!path(a[1], full))
            return false;
        int index = vfs_find(full);
        if (same(cmd, "mkdir"))
            return vfs_mkdir(full) ||
                   fs_failure(full, "Cannot create directory: check parent, name and free slots.");
        if (same(cmd, "touch")) {
            if (index >= 0)
                return !vfs_entry(index)->is_dir || fail("Cannot touch a directory.");
            return vfs_create(full) >= 0 ||
                   fs_failure(full, "Cannot create file: check parent, name and free slots.");
        }
        if (index < 0)
            return fail("Path not found.");
        if (same(cmd, "rm") && vfs_entry(index)->is_dir)
            return fail("Use rmdir to remove an empty directory.");
        if (same(cmd, "rmdir")) {
            if (!vfs_entry(index)->is_dir)
                return fail("Not a directory.");
            size_t n = strlen(full);
            if (!strncmp(cwd, full, n) && (cwd[n] == 0 || cwd[n] == '/'))
                return fail("Cannot remove the current directory or its parent.");
        }
        return vfs_remove(full) ||
               fs_failure(full, "Cannot remove path (directory must be empty; root is protected).");
    }
    if (same(cmd, "write") || same(cmd, "append")) {
        if (!usage(argc >= 2, "Usage: write|append <file> [text...]"))
            return false;
        char full[128];
        if (!path(a[1], full))
            return false;
        int index = vfs_find(full);
        if (index >= 0 && vfs_entry(index)->is_dir)
            return fail("Cannot write a directory.");
        if (same(cmd, "append") && index >= 0) {
            if (!vfs_read(index))
                return fail(vfs_error());
            put(out, vfs_entry(index)->data);
        }
        for (int i = 2; i < argc; ++i) {
            if (i > 2)
                put(out, " ");
            put(out, a[i]);
        }
        if (out->overflow || out->size >= VFS_FILE_CAP)
            return fail("File content limit is 16383 bytes; file unchanged.");
        if (index < 0)
            index = vfs_create(full);
        if (index < 0)
            return fs_failure(full, "Cannot create file: check parent, name and free slots.");
        if (!vfs_write(index, out->data))
            return fail(vfs_error());
        out->size = 0;
        out->data[0] = 0;
        return true;
    }
    if (same(cmd, "cp") || same(cmd, "mv")) {
        if (!usage(argc == 3, "Usage: cp|mv <source> <destination>"))
            return false;
        char from[128], to[128];
        if (!path(a[1], from) || !destination(a[2], from, to))
            return false;
        if (same(cmd, "cp"))
            return vfs_copy(from, to) ||
                   fs_failure(
                       extfs_path(to) ? to : from,
                       "Copy failed: file source, valid destination and free space required.");
        char newcwd[128];
        strcopy(newcwd, cwd, sizeof(newcwd));
        size_t n = strlen(from);
        if (!strncmp(cwd, from, n) && (cwd[n] == 0 || cwd[n] == '/')) {
            if (strlen(to) + strlen(cwd + n) >= sizeof(newcwd))
                return fail("Moved current directory path would be too long.");
            strcopy(newcwd, to, sizeof(newcwd));
            add(newcwd, sizeof(newcwd), cwd + n);
        }
        if (!vfs_rename(from, to))
            return fs_failure(
                extfs_path(to) ? to : from,
                "Move failed: destination must be unused and outside the source subtree.");
        strcopy(cwd, newcwd, sizeof(cwd));
        return true;
    }
    if (same(cmd, "cat")) {
        if (argc == 1)
            put(out, input);
        for (int j = 1; j < argc; ++j) {
            int i = file_arg(a[j]);
            if (i < 0)
                return false;
            putn(out, vfs_entry(i)->data, vfs_entry(i)->size);
        }
        return true;
    }
    if (same(cmd, "head") || same(cmd, "tail")) {
        size_t count = 10;
        int first = 1;
        if (argc > 1 && same(a[1], "-n")) {
            if (!usage(argc >= 3 && unsigned_value(a[2], &count),
                       "Usage: head|tail [-n count] [file]"))
                return false;
            first = 3;
        }
        const char *text;
        if (!input_source(s, first, input, &text))
            return false;
        size_t length = strlen(text);
        if (!count || !length)
            return true;
        if (same(cmd, "head")) {
            size_t n = 0, lines = 0;
            while (n < length) {
                if (text[n++] == '\n' && ++lines == count)
                    break;
            }
            putn(out, text, n);
        } else {
            size_t start = length, lines = 0;
            if (start && text[start - 1] == '\n')
                --start;
            while (start) {
                if (text[start - 1] == '\n' && ++lines == count)
                    break;
                --start;
            }
            putn(out, text + start, length - start);
        }
        return true;
    }
    if (same(cmd, "grep")) {
        int first = 1;
        bool invert = false, numbered = false;
        while (first < argc && (same(a[first], "-v") || same(a[first], "-n"))) {
            if (same(a[first], "-v"))
                invert = true;
            else
                numbered = true;
            ++first;
        }
        if (!usage(first < argc, "Usage: grep [-n] [-v] <literal-text> [file]"))
            return false;
        const char *pattern = a[first++], *text;
        if (!input_source(s, first, input, &text))
            return false;
        unsigned line_no = 0;
        while (*text) {
            const char *start = text;
            while (*text && *text != '\n')
                ++text;
            size_t n = (size_t)(text - start);
            ++line_no;
            bool match = contains(start, n, pattern);
            if (match != invert) {
                if (numbered) {
                    num(out, line_no);
                    put(out, ":");
                }
                putn(out, start, n);
                put(out, "\n");
            }
            if (*text)
                ++text;
        }
        return true;
    }
    if (same(cmd, "wc")) {
        int first = 1;
        char mode = 0;
        if (argc > 1 && (same(a[1], "-l") || same(a[1], "-w") || same(a[1], "-c"))) {
            mode = a[1][1];
            first = 2;
        }
        const char *text;
        if (!input_source(s, first, input, &text))
            return false;
        uint64_t lines = 0, words = 0, bytes = 0;
        bool word = false;
        while (text[bytes]) {
            char c = text[bytes++];
            if (c == '\n')
                ++lines;
            if (white(c))
                word = false;
            else if (!word) {
                ++words;
                word = true;
            }
        }
        if (mode == 'l')
            num(out, lines);
        else if (mode == 'w')
            num(out, words);
        else if (mode == 'c')
            num(out, bytes);
        else {
            num(out, lines);
            put(out, " ");
            num(out, words);
            put(out, " ");
            num(out, bytes);
        }
        put(out, "\n");
        return true;
    }
    if (same(cmd, "history")) {
        if (!usage(argc == 1, "Usage: history"))
            return false;
        for (unsigned i = 0; i < history_count; ++i) {
            num(out, i + 1);
            put(out, "  ");
            line(out, shell_history_get((int)(history_count - 1 - i)));
        }
        return true;
    }
    if (same(cmd, "sh")) {
        if (!usage(argc == 2, "Usage: sh <script-file>"))
            return false;
        return script(a[1]);
    }
    if (same(cmd, "sync")) {
        if (!usage(argc == 1, "Usage: sync"))
            return false;
        if (!vfs_sync()) {
            shell_print(vfs_error());
            return fail("Sync failed; modified files remain in memory.");
        }
        line(out, "Mounted filesystems synchronized.");
        if (!storage_mounted())
            line(out, "The home filesystem is RAM-only; its files are not persistent.");
        return true;
    }
    if (same(cmd, "mounts")) {
        if (!usage(argc == 1, "Usage: mounts"))
            return false;
        line(out, storage_status());
        line(out, extfs_status());
        line(out, "External volumes are discovered automatically at boot. NTFS is read-only.");
        return true;
    }
    if (same(cmd, "df")) {
        if (!usage(argc == 1, "Usage: df"))
            return false;
        line(out, storage_status());
        line(out, extfs_status());
        put(out, "ArkFS logical content limit bytes: ");
        num(out, storage_capacity_bytes());
        put(out, "\nUsed content bytes: ");
        num(out, storage_used_bytes());
        unsigned entries = 0;
        for (int i = 0; i < VFS_MAX_FILES; ++i)
            if (vfs_entry(i)->used)
                ++entries;
        put(out, "\nEntries: ");
        num(out, entries);
        put(out, "/64 (directories included)\n");
        line(out, "Per-file limit: 16383 bytes. Run sync to confirm durable writes.");
        return true;
    }
    if (same(cmd, "mem")) {
        if (!usage(argc == 1, "Usage: mem"))
            return false;
        put(out, "Boot-reported memory: ");
        num(out, boot_memory_mib);
        line(out, " MiB");
        line(out, "Static filesystem: 64 entries, 16383 content bytes each.");
        line(out, "This is boot information, not a live free-memory counter.");
        return true;
    }
    if (same(cmd, "uptime")) {
        if (!usage(argc == 1, "Usage: uptime"))
            return false;
        uint64_t seconds = platform_ticks() / 100;
        num(out, seconds / 3600);
        put(out, "h ");
        num(out, (seconds / 60) % 60);
        put(out, "m ");
        num(out, seconds % 60);
        line(out, "s");
        return true;
    }
    if (same(cmd, "date")) {
        if (!usage(argc == 1, "Usage: date"))
            return false;
        int h, m, sec;
        platform_time(&h, &m, &sec);
        if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59)
            return fail("RTC clock unavailable.");
        char clock[] = "RTC time: 00:00:00";
        clock[10] = (char)('0' + h / 10);
        clock[11] = (char)('0' + h % 10);
        clock[13] = (char)('0' + m / 10);
        clock[14] = (char)('0' + m % 10);
        clock[16] = (char)('0' + sec / 10);
        clock[17] = (char)('0' + sec % 10);
        line(out, clock);
        line(out, "Calendar date and timezone conversion are not implemented.");
        return true;
    }
    if (same(cmd, "clear")) {
        if (!usage(argc == 1, "Usage: clear"))
            return false;
        memset(shell_lines, 0, sizeof(shell_lines));
        shell_line_count = 0;
        serial_write("\033[2J\033[H");
        return true;
    }
    if (same(cmd, "open")) {
        if (!usage(argc == 2, "Usage: open files|notes|settings|about"))
            return false;
        if (same(a[1], "files"))
            shell_action = 1;
        else if (same(a[1], "notes"))
            shell_action = 2;
        else if (same(a[1], "settings"))
            shell_action = 3;
        else if (same(a[1], "about"))
            shell_action = 4;
        else if (same(a[1], "browser"))
            shell_action = 6;
        else if (same(a[1], "tasks"))
            shell_action = 10;
        else if (same(a[1], "capture"))
            shell_action = 11;
        else if (same(a[1], "installer"))
            shell_action = 12;
        else if (same(a[1], "packages"))
            shell_action = 16;
        else if (same(a[1], "wasm"))
            shell_action = 15;
        else if (same(a[1], "todo"))
            shell_action = 13;
        else if (same(a[1], "timer"))
            shell_action = 14;
        else
            return fail("Usage: open files|notes|settings|about");
        return true;
    }
    if (same(cmd, "reboot") || same(cmd, "shutdown")) {
        if (!usage(argc == 1, "Usage: reboot|shutdown"))
            return false;
        if (!vfs_sync()) {
            shell_print(vfs_error());
            return fail("Shutdown cancelled because filesystem sync failed.");
        }
        if (!storage_mounted())
            shell_print("Home filesystem is RAM-only; its files will be lost.");
        if (same(cmd, "reboot")) {
            shell_print("Rebooting...");
            platform_reboot();
        } else {
            shell_print("Powering off...");
            platform_poweroff();
        }
        return fail("Platform power control returned; use your VM or hardware controls.");
    }
    shell_print(cmd);
    return fail("Unknown command. Type help for native commands and pkg.");
}

static bool special(const char *cmd) {
    return same(cmd, "sh") || same(cmd, "cd") || same(cmd, "clear") || same(cmd, "open") ||
           same(cmd, "reboot") || same(cmd, "shutdown");
}
static bool execute(const char *command, bool announce) {
    if (!command)
        return true;
    size_t length = 0;
    while (length < COMMAND_CAP && command[length])
        ++length;
    if (length >= COMMAND_CAP)
        return fail("Command too long (maximum 1023 UTF-8 bytes).");
    while (white(*command))
        ++command;
    if (!*command || *command == '#')
        return true;
    if (announce) {
        strcopy(history[history_next], command, COMMAND_CAP);
        history_next = (history_next + 1) % HISTORY_MAX;
        if (history_count < HISTORY_MAX)
            ++history_count;
        char shown[COMMAND_CAP + 140] = {0};
        strcopy(shown, shell_prompt(), sizeof(shown));
        add(shown, sizeof(shown), command);
        shell_print(shown);
    }
    Parsed parsed;
    if (!parse(command, &parsed))
        return false;
    if (!parsed.stage[0].argc)
        return true;
    if (parsed.count > 1 || parsed.redirect)
        for (int i = 0; i < parsed.count; ++i)
            if (special(parsed.stage[i].argv[0]))
                return fail("This interactive command cannot be piped or redirected.");
    /* sh executes complete nested commands and must not borrow the global pipe output. */
    if (same(parsed.stage[0].argv[0], "sh")) {
        if (!usage(parsed.stage[0].argc == 2, "Usage: sh <script-file>"))
            return false;
        return script(parsed.stage[0].argv[1]);
    }
    const char *input = "";
    Output out = {0};
    for (int i = 0; i < parsed.count; ++i) {
        out.data = pipe_text[i % 2];
        out.size = 0;
        out.overflow = false;
        out.data[0] = 0;
        if (!builtin(&parsed.stage[i], input, &out))
            return false;
        if (out.overflow)
            return fail("Pipeline output exceeds 32768 bytes; destination unchanged.");
        input = out.data;
    }
    if (parsed.redirect) {
        char full[128];
        if (!path(parsed.redirect, full))
            return false;
        int index = vfs_find(full);
        if (index >= 0 && vfs_entry(index)->is_dir)
            return fail("Cannot redirect output to a directory.");
        if (parsed.append && index >= 0 && !vfs_read(index))
            return fail(vfs_error());
        size_t oldsize = parsed.append && index >= 0 ? vfs_entry(index)->size : 0;
        if (oldsize + out.size >= VFS_FILE_CAP)
            return fail("Redirected file would exceed 16383 bytes; file unchanged.");
        char *combined = pipe_text[parsed.count % 2];
        if (oldsize)
            memcpy(combined, vfs_entry(index)->data, oldsize);
        memcpy(combined + oldsize, out.data, out.size + 1);
        if (index < 0)
            index = vfs_create(full);
        if (index < 0)
            return fs_failure(full,
                              "Cannot create redirected file: check parent, name and free slots.");
        if (!vfs_write(index, combined))
            return fail(vfs_error());
    } else if (out.size)
        shell_print(out.data);
    return true;
}

void shell_execute(const char *command) {
    (void)execute(command, true);
}
void shell_init(const BootInfo *info) {
    boot_memory_mib = info ? info->memory_mib : 0;
    shell_line_count = shell_action = 0;
    history_count = history_next = script_depth = 0;
    memset(shell_lines, 0, sizeof(shell_lines));
    memset(history, 0, sizeof(history));
    memset(pipe_text, 0, sizeof pipe_text);
    memset(script_text, 0, sizeof script_text);
    memset(prompt_text, 0, sizeof prompt_text);
    strcopy(cwd, directory(session_home) ? session_home : "/", sizeof(cwd));
    shell_print("ArkOS 终端 0.13.0+mouse1");
    shell_print("输入 help 查看命令帮助。");
    shell_print(storage_status());
    shell_print(extfs_status());
}
