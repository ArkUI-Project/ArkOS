/* Host integration tests: real shell + real VFS, stubbed hardware/storage. */
#include "ark.h"
#include "storage.h"
#include "shell_extra.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char serial[131072];
static bool mounted, sync_ok = true;
static int sync_calls, reboot_calls, poweroff_calls;
void serial_write(const char *s) {
    size_t n = strlen(serial);
    if (n + strlen(s) < sizeof(serial))
        strcat(serial, s);
}
uint64_t platform_ticks(void) {
    return 366100;
}
void platform_time(int *h, int *m, int *s) {
    *h = 12;
    *m = 34;
    *s = 56;
}
void platform_reboot(void) {
    ++reboot_calls;
}
void platform_poweroff(void) {
    ++poweroff_calls;
}
bool storage_init(void) {
    return false;
}
bool storage_mounted(void) {
    return mounted;
}
const char *storage_status(void) {
    return mounted ? "ArkFS test disk" : "RAM mode (test)";
}
uint64_t storage_capacity_bytes(void) {
    return mounted ? 8388608 : 0;
}
uint64_t storage_used_bytes(void) {
    uint64_t n = 0;
    for (int i = 0; i < VFS_MAX_FILES; ++i)
        if (vfs_files[i].used)
            n += vfs_files[i].size;
    return n;
}
bool storage_sync(void) {
    ++sync_calls;
    return sync_ok;
}
const char *storage_error(void) {
    return "mock I/O failure";
}
void storage_mark_dirty(void) {
}
const char *extfs_status(void) {
    return "No external volumes (host shell test)";
}
bool extfs_path(const char *path) {
    return !strncmp(path, "/mnt/", 5);
}
void strcopy(char *d, const char *s, size_t cap) {
    if (!cap)
        return;
    size_t n = strlen(s);
    if (n >= cap)
        n = cap - 1;
    memmove(d, s, n);
    d[n] = 0;
}
void uint_to_str(uint64_t n, char *out) {
    snprintf(out, 24, "%llu", (unsigned long long)n);
}
static void run(const char *command) {
    serial[0] = 0;
    shell_execute(command);
}
static void content(const char *name, const char *expected) {
    int i = vfs_find(name);
    assert(i >= 0);
    assert(!strcmp(vfs_files[i].data, expected));
}
static void output(const char *expected) {
    if (!strstr(serial, expected)) {
        fprintf(stderr, "Expected [%s] in [%s]\n", expected, serial);
        assert(0);
    }
}
static void seed(const char *name, const char *text) {
    int i = vfs_create(name);
    assert(i >= 0);
    assert(vfs_write(i, text));
}
int main(void) {
    vfs_init();
    BootInfo info = {0};
    info.memory_mib = 256;
    shell_init(&info);
    assert(!strcmp(shell_cwd(), "/home/ark"));
    output("ArkOS 终端");
    run("echo '你好 ArkOS' > '中文 笔记.txt'");
    content("中文 笔记.txt", "你好 ArkOS\n");
    run("echo \"a | b > c\" >> '中文 笔记.txt'");
    content("中文 笔记.txt", "你好 ArkOS\na | b > c\n");
    run("write escape\\ name.txt a\\ b 'c d'");
    content("escape name.txt", "a b c d");
    run("append escape\\ name.txt \"你好\"");
    content("escape name.txt", "a b c d你好");
    run("echo '' > emptyline");
    content("emptyline", "\n");
    seed("poem", "alpha\nbeta\nalpha gamma\n");
    run("cat poem | grep alpha | wc -l > count");
    content("count", "2\n");
    run("cat poem | head -n 2 | tail -n 1 > last");
    content("last", "beta\n");
    run("grep -n -v alpha poem > result");
    content("result", "2:beta\n");
    run("wc -w poem > count");
    content("count", "4\n");
    run("cat poem | tail -n 0 > count");
    content("count", "");
    seed("no_final_newline", "a\nb\nc");
    run("tail -n 1 no_final_newline > result");
    content("result", "c");
    run("head -n 2 no_final_newline > result");
    content("result", "a\nb\n");
    run("mkdir work");
    run("cd work");
    assert(!strcmp(shell_cwd(), "/home/ark/work"));
    run("touch ../work/test");
    assert(vfs_find("/home/ark/work/test") >= 0);
    run("echo data > ./test");
    content("/home/ark/work/test", "data\n");
    run("cp test ../copied");
    content("/home/ark/copied", "data\n");
    run("mv /home/ark/work /home/ark/renamed");
    assert(!strcmp(shell_cwd(), "/home/ark/renamed"));
    run("rmdir .");
    output("current directory");
    assert(vfs_find("/home/ark/renamed") >= 0);
    run("cd .././../ark");
    assert(!strcmp(shell_cwd(), "/home/ark"));
    run("rm renamed");
    output("Use rmdir");
    run("rmdir renamed");
    output("must be empty");
    run("rm renamed/test");
    run("rmdir renamed");
    assert(vfs_find("renamed") < 0);
    run("ls");
    output("中文 笔记.txt");
    assert(strstr(serial, "使用说明.txt") == NULL);
    run("cd /../../../../");
    assert(!strcmp(shell_cwd(), "/"));
    run("cd");
    run("echo 'broken > failfile");
    output("unclosed quote");
    assert(vfs_find("failfile") < 0);
    run("echo x | | wc > failfile");
    output("Syntax error");
    assert(vfs_find("failfile") < 0);
    run("echo x > failfile extra");
    output("redirection must be last");
    assert(vfs_find("failfile") < 0);
    run("echo x | cd /");
    output("cannot be piped");
    assert(!strcmp(shell_cwd(), "/home/ark"));
    run("echo x > /missing/child");
    output("Cannot create");
    assert(vfs_find("/missing/child") < 0);
    seed("script", "# small script\nmkdir scriptdir\ncd scriptdir\necho '脚本输出' > output\ncd "
                   "..\ncat scriptdir/output | wc -l > count\n");
    run("sh script");
    content("scriptdir/output", "脚本输出\n");
    content("count", "1\n");
    seed("recursive", "sh recursive\n");
    run("sh recursive");
    output("nesting limit");
    seed("stopping", "nocommand\necho should-not-run > failfile\n");
    run("sh stopping");
    output("Script stopped at line 1");
    assert(vfs_find("failfile") < 0);
    static char manylines[4096];
    for (int i = 0; i < 65; ++i)
        strcat(manylines, "echo z >> many\n");
    seed("manyscript", manylines);
    run("sh manyscript");
    output("line limit");
    assert(vfs_files[vfs_find("many")].size == 128);
    static char huge[VFS_FILE_CAP];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    seed("big", huge);
    seed("unchanged", "original");
    run("cat big big big > unchanged");
    output("exceeds 32768");
    content("unchanged", "original");
    run("cat big big > unchanged");
    output("exceed 16383");
    content("unchanged", "original");
    run("echo y >> big");
    output("exceed 16383");
    content("big", huge);
    run("uptime");
    output("1h 1m 1s");
    run("date");
    output("RTC time: 12:34:56");
    run("open notes");
    assert(shell_action == 2);
    run("mem");
    output("256 MiB");
    mounted = true;
    sync_ok = false;
    run("shutdown");
    output("cancelled");
    assert(poweroff_calls == 0);
    run("reboot");
    assert(reboot_calls == 0);
    sync_ok = true;
    run("sync");
    output("synchronized");
    run("shutdown");
    assert(poweroff_calls == 1);
    assert(sync_calls >= 4);
    run("clear");
    assert(shell_line_count == 0);
    char boundary[300];
    memset(boundary, 'a', 254);
    strcpy(boundary + 254, "中文");
    shell_print(boundary);
    assert(shell_line_count == 2);
    assert(strlen(shell_lines[0]) == 254);
    assert(!strcmp(shell_lines[1], "中文"));
    for (int i = 0; i < 40; ++i)
        run("echo history");
    assert(!strcmp(shell_history_get(0), "echo history"));
    assert(shell_history_get(31));
    assert(!shell_history_get(32));
    assert(!shell_history_get(-1));
    /* Parser stress: deterministic malformed quotes/operators and UTF-8 bytes. */
    unsigned state = 12345;
    const char alphabet[] = "abc |>'\"\\\t/#xyz012中文";
    for (int i = 0; i < 1000; ++i) {
        char fuzz[200];
        size_t n = (unsigned)i % (sizeof(fuzz) - 1);
        for (size_t j = 0; j < n; ++j) {
            state = state * 1664525u + 1013904223u;
            fuzz[j] = alphabet[state % (sizeof(alphabet) - 1)];
        }
        fuzz[n] = 0;
        run(fuzz);
    }
    puts("host shell/VFS: quoted parser, UTF-8, paths, scripts, pipelines, bounds and sync guards "
         "passed");
    return 0;
}
