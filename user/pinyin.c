/* ArkOS original native Viterbi/beam decoder. Data is compiled from the pinned
 * Rime vocabulary and unigram frequencies; inference runs entirely in Ring3. */
#include "ark.h"
#include "pinyin.h"
#include "ime_lexicon.h"
#define BEAM 8
#define LEARNED 256
static ArkPinyinLearned learned[LEARNED];
static unsigned learned_count;
typedef struct {
    uint32_t score;
    char text[64];
} Path;
static Path paths[49][BEAM];
static unsigned path_count[49];
static unsigned lower_bound(const char *key) {
    unsigned lo = 0, hi = sizeof ime_lexicon / sizeof *ime_lexicon;
    while (lo < hi) {
        unsigned m = lo + (hi - lo) / 2;
        if (strcmp(ime_keys + ime_lexicon[m].key, key) < 0)
            lo = m + 1;
        else
            hi = m;
    }
    return lo;
}
static void path_add(unsigned at, const char *word, const Path *tail, unsigned cost) {
    size_t n = strlen(word), rest = strlen(tail->text);
    if (n + rest >= 64)
        return;
    Path next = {.score = tail->score + cost};
    memcpy(next.text, word, n);
    memcpy(next.text + n, tail->text, rest + 1);
    unsigned count = path_count[at];
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(paths[at][i].text, next.text)) {
            if (paths[at][i].score <= next.score)
                return;
            for (unsigned j = i; j + 1 < count; j++)
                paths[at][j] = paths[at][j + 1];
            count--;
            break;
        }
    unsigned pos = 0;
    while (pos < count && paths[at][pos].score <= next.score)
        pos++;
    if (pos >= BEAM)
        return;
    if (count < BEAM)
        count++;
    for (unsigned j = count - 1; j > pos; j--)
        paths[at][j] = paths[at][j - 1];
    paths[at][pos] = next;
    path_count[at] = count;
}
void ark_pinyin_reset_learning(void) {
    memset(learned, 0, sizeof learned);
    learned_count = 0;
}
void ark_pinyin_restore(const ArkPinyinLearned *item) {
    if (learned_count >= LEARNED || !item || !item->hits || item->key[48] || item->text[63])
        return;
    for (unsigned i = 0; i < 48 && item->key[i]; i++)
        if (item->key[i] < 'a' || item->key[i] > 'z')
            return;
    learned[learned_count++] = *item;
}
int ark_pinyin_learn(const char *input, const char *text, ArkPinyinLearned *out) {
    char key[49];
    unsigned n = 0;
    for (unsigned i = 0; input[i] && n < 48; i++)
        if (input[i] != '\'')
            key[n++] = input[i];
    key[n] = 0;
    if (!n || !text[0] || strlen(text) >= 64)
        return -1;
    unsigned slot = learned_count;
    for (unsigned i = 0; i < learned_count; i++)
        if (!strcmp(learned[i].key, key) && !strcmp(learned[i].text, text)) {
            slot = i;
            break;
        }
    if (slot == learned_count) {
        if (learned_count < LEARNED)
            learned_count++;
        else {
            slot = 0;
            for (unsigned i = 1; i < LEARNED; i++)
                if (learned[i].hits < learned[slot].hits)
                    slot = i;
        }
        memset(&learned[slot], 0, sizeof learned[slot]);
        strcopy(learned[slot].key, key, 49);
        strcopy(learned[slot].text, text, 64);
    }
    if (learned[slot].hits < 1000000)
        learned[slot].hits++;
    if (out)
        *out = learned[slot];
    return (int)slot;
}
static bool explicit_boundaries(uint64_t input, unsigned offset, unsigned length, uint64_t lex) {
    uint64_t inside = input >> offset;
    uint64_t mask = length < 64 ? ((1ull << length) - 1) : ~0ull;
    return !(inside & mask & ~lex & ~1ull);
}
static void candidate_add(ArkPinyinResult *r, const char *text, unsigned used, unsigned *total) {
    if (strlen(text) >= 64)
        return;
    unsigned index = (*total)++;
    if (index < r->page * 9 || index >= r->page * 9 + 9)
        return;
    ArkPinyinCandidate *c = &r->choices[r->count++];
    strcopy(c->text, text, 64);
    c->consumed = used;
}
bool ark_pinyin_query(const char *input, unsigned page, ArkPinyinResult *r) {
    if (!input || !r || page > 4096)
        return false;
    memset(r, 0, sizeof *r);
    r->page = page;
    char plain[49];
    unsigned original[49], n = 0;
    uint64_t boundaries = 0;
    for (unsigned at = 0; input[at]; at++) {
        if (at >= 48)
            return false;
        char c = input[at];
        if (c == '\'') {
            if (!n)
                return false;
            boundaries |= 1ull << n;
            continue;
        }
        if (c < 'a' || c > 'z')
            return false;
        plain[n] = c;
        original[n++] = at + 1;
    }
    plain[n] = 0;
    if (!n)
        return true;
    memset(path_count, 0, sizeof path_count);
    path_count[n] = 1;
    paths[n][0] = (Path){0, {0}};
    for (int at = (int)n - 1; at >= 0; at--) {
        for (unsigned length = 1; length <= n - (unsigned)at; length++) {
            if (!path_count[at + length])
                continue;
            char key[49];
            memcpy(key, plain + at, length);
            key[length] = 0;
            unsigned first = lower_bound(key), seen = 0;
            for (unsigned i = first; i < sizeof ime_lexicon / sizeof *ime_lexicon &&
                                     !strcmp(ime_keys + ime_lexicon[i].key, key);
                 i++) {
                const ImeLexeme *e = &ime_lexicon[i];
                if (!explicit_boundaries(boundaries, (unsigned)at, length, e->boundaries))
                    continue;
                if (seen++ >= 64)
                    break;
                for (unsigned tail = 0; tail < path_count[at + length]; tail++)
                    path_add((unsigned)at, ime_texts + e->text, &paths[at + length][tail], e->cost);
            }
            for (unsigned i = 0; i < learned_count; i++)
                if (!strcmp(learned[i].key, key))
                    for (unsigned tail = 0; tail < path_count[at + length]; tail++)
                        path_add((unsigned)at, learned[i].text, &paths[at + length][tail],
                                 learned[i].hits >= 9 ? 1 : 600 - learned[i].hits * 60);
        }
    }
    unsigned total = 0;
    for (unsigned i = 0; i < path_count[0]; i++)
        candidate_add(r, paths[0][i].text, original[n - 1], &total);
    for (unsigned length = n; length; length--) {
        char key[49];
        memcpy(key, plain, length);
        key[length] = 0;
        unsigned first = lower_bound(key);
        for (unsigned i = first; i < sizeof ime_lexicon / sizeof *ime_lexicon &&
                                 !strcmp(ime_keys + ime_lexicon[i].key, key);
             i++) {
            const ImeLexeme *e = &ime_lexicon[i];
            if (!explicit_boundaries(boundaries, 0, length, e->boundaries))
                continue;
            const char *text = ime_texts + e->text;
            bool same = false;
            for (unsigned j = 0; j < path_count[0]; j++)
                if (!strcmp(paths[0][j].text, text))
                    same = true;
            if (!same)
                candidate_add(r, text, original[length - 1], &total);
        }
    }
    r->total = total;
    return true;
}
