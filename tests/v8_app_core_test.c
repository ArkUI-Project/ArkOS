/* Exercise the shipped parser and UTF-8 editor primitives under ASan/UBSan. */
#define main calculator_program
#include "../user/apps/calculator.c"
#undef main
#include <assert.h>
#include <stdio.h>
#include <string.h>
void strcopy(char *d, const char *s, size_t cap) {
    if (cap) {
        size_t n = strlen(s);
        if (n >= cap)
            n = cap - 1;
        memmove(d, s, n);
        d[n] = 0;
    }
}
void uint_to_str(uint64_t n, char *out) {
    snprintf(out, 24, "%llu", (unsigned long long)n);
}
static void calculation(const char *input, const char *expected) {
    strcopy(text, input, sizeof text);
    eval();
    assert(!strcmp(result, expected));
}
int main(void) {
    calculation("123+45", "168");
    calculation("2*(3+4)", "14");
    calculation("-7/3", "-2");
    calculation("9223372036854775807", "9223372036854775807");
    calculation("0-9223372036854775807-1", "-9223372036854775808");
    const char *bad[] = {
        "1/0",
        "9223372036854775807+1",
        "(0-9223372036854775807-1)/-1",
        "9999999999999999999",
        "2*(3+4",
        "1+",
        "",
        "((((((((((((((((((((((((((((((((((((1))))))))))))))))))))))))))))))))))))"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
        calculation(bad[i], "表达式错误或整数溢出");
    struct {
        unsigned before;
        char b[12];
        unsigned after;
    } guard = {.before = 0x12345678, .after = 0xabcdef12};
    size_t n = 0, c = 0;
    assert(app_codepoint(guard.b, &n, &c, sizeof guard.b, 0x4e2d));
    assert(app_codepoint(guard.b, &n, &c, sizeof guard.b, 0x1f30f));
    assert(n == 7 && c == 7);
    assert(app_edit(guard.b, &n, &c, sizeof guard.b, KEY_BACKSPACE) && n == 3);
    assert(!strcmp(guard.b, "中"));
    assert(!app_codepoint(guard.b, &n, &c, sizeof guard.b, 0xd800));
    assert(!app_codepoint(guard.b, &n, &c, sizeof guard.b, 0x110000));
    assert(!app_insert(guard.b, &n, &c, sizeof guard.b, "123456789"));
    assert(app_edit(guard.b, &n, &c, sizeof guard.b, KEY_HOME));
    assert(app_edit(guard.b, &n, &c, sizeof guard.b, KEY_DELETE) && n == 0);
    n = 100;
    assert(!app_insert(guard.b, &n, &c, sizeof guard.b, "x"));
    assert(guard.before == 0x12345678 && guard.after == 0xabcdef12);
    puts("PASS native calculator precedence/overflow/division/depth and UTF-8 "
         "insertion/deletion/bounds");
    return 0;
}
