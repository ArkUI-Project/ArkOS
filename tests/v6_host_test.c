#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "html.h"
#include "microcode.h"
void strcopy(char *d, const char *s, size_t cap) {
    if (!cap)
        return;
    size_t n = strlen(s);
    if (n >= cap)
        n = cap - 1;
    memcpy(d, s, n);
    d[n] = 0;
}
static ArkHTML doc;
static void html_test(void) {
    const char *text =
        "<head><script>hidden</script></head><h1>标题</h1><p>A &amp; B &#x4f60;&#22909;</p><a "
        "href='/next'>链接</a><script>x()</script><img alt='diagram'>";
    ark_html_parse(&doc, text, strlen(text), true);
    assert(!strcmp(doc.text, "标题\nA & B 你好\n链接[diagram]"));
    assert(doc.links == 1 && !strcmp(doc.href[0], "/next"));
    assert(doc.style[0] == 1);
    char url[256];
    assert(ark_html_resolve("http://example.org/a/b", "../next", url) &&
           !strcmp(url, "http://example.org/next"));
    assert(ark_html_resolve("http://example.org/a/b", "/root/./c/../d", url) &&
           !strcmp(url, "http://example.org/root/d"));
    assert(ark_html_resolve("http://example.org/a/b?old=1#top", "?new=1", url) &&
           !strcmp(url, "http://example.org/a/b?new=1"));
    assert(ark_html_resolve("http://example.org/a/b?old=1#top", "#next", url) &&
           !strcmp(url, "http://example.org/a/b?old=1#next"));
    assert(ark_html_resolve("http://example.org/a/b?old=/dir/", "next", url) &&
           !strcmp(url, "http://example.org/a/next"));
    text = "<a href='?a=1&amp;b=2'>&#x4f60;</a>";
    ark_html_parse(&doc, text, strlen(text), true);
    assert(!strcmp(doc.href[0], "?a=1&b=2") && !strcmp(doc.text, "你"));
    assert(!ark_html_resolve("http://example.org/", "javascript:run()", url));
    assert(!ark_html_resolve("http://example.org/", "https://example.org/", url));
    assert(!ark_html_resolve("http://example.org/", "/bad\r\nheader", url));
    unsigned random = 73;
    char data[1024];
    for (unsigned t = 0; t < 10000; t++) {
        unsigned len = t % sizeof data;
        for (unsigned i = 0; i < len; i++) {
            random = random * 1664525 + 1013904223;
            data[i] = (char)(random >> 24);
        }
        ark_html_parse(&doc, data, len, true);
        assert(doc.length <= HTML_TEXT_MAX && doc.text[doc.length] == 0 &&
               doc.links <= HTML_LINK_MAX);
    }
    puts("PASS bounded HTML, entities, hidden script/style, links, URL traversal and 10000 "
         "malformed documents");
}
static void microcode_test(void) {
    uint32_t patch[512] = {0};
    patch[0] = 1;
    patch[1] = 10;
    patch[3] = 0x906e9;
    patch[5] = 1;
    patch[6] = 2;
    uint32_t sum = 0;
    for (unsigned i = 0; i < 512; i++)
        sum += patch[i];
    patch[4] -= sum;
    assert(intel_microcode_match(patch, sizeof patch, 0x906e9, 2, 9) == 2048);
    assert(!intel_microcode_match(patch, 47, 0x906e9, 2, 9));
    assert(!intel_microcode_match(patch, 2047, 0x906e9, 2, 9));
    assert(!intel_microcode_match(patch, sizeof patch, 0x906ea, 2, 9));
    assert(!intel_microcode_match(patch, sizeof patch, 0x906e9, 1, 9));
    assert(!intel_microcode_match(patch, sizeof patch, 0x906e9, 2, 10));
    assert(!intel_microcode_match(patch, sizeof patch, 0x906e9, 2, 11));
    patch[511] ^= 1;
    assert(!intel_microcode_match(patch, sizeof patch, 0x906e9, 2, 9));
    patch[511] ^= 1;
    patch[7] = 0xfffffff0;
    assert(!intel_microcode_match(patch, sizeof patch, 0x906e9, 2, 9));
    puts("PASS microcode checksum, bounds, signature, platform and no-downgrade (synthetic parser "
         "tests only)");
}
int main(void) {
    html_test();
    microcode_test();
}
