#ifndef ARK_PINYIN_H
#define ARK_PINYIN_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct {
    char text[64];
    unsigned consumed;
} ArkPinyinCandidate;
typedef struct {
    unsigned count, total, page;
    ArkPinyinCandidate choices[9];
} ArkPinyinResult;
typedef struct {
    char key[49], text[64];
    uint32_t hits;
} ArkPinyinLearned;
void ark_pinyin_reset_learning(void);
void ark_pinyin_restore(const ArkPinyinLearned *item);
int ark_pinyin_learn(const char *key, const char *text, ArkPinyinLearned *item);
bool ark_pinyin_query(const char *input, unsigned page, ArkPinyinResult *result);
#endif
