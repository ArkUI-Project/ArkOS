#include "pinyin.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
void strcopy(char *d, const char *s, size_t n) {
    if (n)
        snprintf(d, n, "%s", s);
}
int main(void) {
    ArkPinyinResult r;
    assert(ark_pinyin_query("nihao", 0, &r) && !strcmp(r.choices[0].text, "你好") &&
           r.choices[0].consumed == 5);
    assert(ark_pinyin_query("nihaoma", 0, &r) && !strcmp(r.choices[0].text, "你好吗"));
    assert(ark_pinyin_query("zhongguo", 0, &r) && !strcmp(r.choices[0].text, "中国"));
    assert(ark_pinyin_query("ni'hao", 0, &r) && r.choices[0].consumed == 6);
    assert(ark_pinyin_query("nv", 0, &r) && r.count);
    assert(ark_pinyin_query("nihaozaijian", 0, &r) &&
           r.choices[0].consumed == strlen("nihaozaijian"));
    assert(ark_pinyin_query("woxiangyaochifan", 0, &r) &&
           r.choices[0].consumed == strlen("woxiangyaochifan"));
    ArkPinyinLearned learned;
    for (unsigned i = 0; i < 9; i++)
        assert(ark_pinyin_learn("huimaoniang", "灰猫娘", &learned) >= 0);
    assert(ark_pinyin_query("huimaoniang", 0, &r) && !strcmp(r.choices[0].text, "灰猫娘"));
    ark_pinyin_reset_learning();
    ark_pinyin_restore(&learned);
    assert(ark_pinyin_query("huimaoniang", 0, &r) && !strcmp(r.choices[0].text, "灰猫娘"));
    unsigned total;
    assert(ark_pinyin_query("shi", 0, &r));
    total = r.total;
    unsigned seen = r.count;
    for (unsigned p = 1; p < (total + 8) / 9; p++) {
        assert(ark_pinyin_query("shi", p, &r));
        assert(r.count <= 9);
        seen += r.count;
    }
    assert(seen == total);
    assert(!ark_pinyin_query("UPPER", 0, &r));
    assert(!ark_pinyin_query("x", 4097, &r));
    char invalid[100];
    memset(invalid, 'a', 99);
    invalid[99] = 0;
    assert(!ark_pinyin_query(invalid, 0, &r));
    puts("PASS native Pinyin dictionary, phrase ranking, segmentation, paging and bounds");
}
