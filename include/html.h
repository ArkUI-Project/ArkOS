#ifndef ARK_HTML_H
#define ARK_HTML_H
#include "ark.h"
#define HTML_TEXT_MAX 16384u
#define HTML_LINK_MAX 64u
typedef struct {
    char text[HTML_TEXT_MAX + 1];
    uint8_t style[HTML_TEXT_MAX], link[HTML_TEXT_MAX];
    char href[HTML_LINK_MAX][256];
    unsigned length, links;
    bool truncated;
} ArkHTML;
/* Deliberately bounded HTML reader. Never executes scripts or loads resources. */
void ark_html_parse(ArkHTML *, const char *, size_t, bool);
bool ark_html_resolve(const char *base, const char *href, char out[256]);
#endif
