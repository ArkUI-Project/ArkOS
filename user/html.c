#include "html.h"
static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}
static bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}
static void emit(ArkHTML *d, char c, unsigned style, unsigned link) {
    if (d->length == HTML_TEXT_MAX) {
        d->truncated = true;
        return;
    }
    unsigned n = d->length++;
    d->text[n] = c;
    d->text[n + 1] = 0;
    d->style[n] = (uint8_t)style;
    d->link[n] = (uint8_t)link;
}
static void newline(ArkHTML *d) {
    if (d->length && d->text[d->length - 1] != '\n')
        emit(d, '\n', 0, 0);
}
static unsigned entity(const char *s, size_t n, unsigned *value) {
    size_t i = 1;
    while (i < n && i < 12 && s[i] != ';' && !space(s[i]) && s[i] != '<')
        i++;
    if (i >= n || s[i] != ';')
        return 0;
    struct {
        const char *name;
        unsigned value;
    } names[] = {{"amp", 38},  {"lt", 60},   {"gt", 62},   {"quot", 34},
                 {"apos", 39}, {"nbsp", 32}, {"copy", 169}};
    for (unsigned k = 0; k < sizeof names / sizeof *names; k++)
        if (strlen(names[k].name) == i - 1 && !strncmp(s + 1, names[k].name, i - 1)) {
            *value = names[k].value;
            return (unsigned)i + 1;
        }
    if (i < 3 || s[1] != '#')
        return 0;
    unsigned base = 10, v = 0;
    size_t at = 2;
    if (s[at] == 'x' || s[at] == 'X') {
        base = 16;
        at++;
    }
    if (at == i)
        return 0;
    for (; at < i; at++) {
        char c = lower(s[at]);
        unsigned digit = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                         : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                                                : 99;
        if (digit >= base || v > 0x10ffff / base)
            return 0;
        v = v * base + digit;
    }
    if (!v || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
        v = 0xfffd;
    *value = v;
    return (unsigned)i + 1;
}
static void codepoint(ArkHTML *d, unsigned c, unsigned st, unsigned link) {
    if (c < 128)
        emit(d, (char)c, st, link);
    else if (c < 2048) {
        emit(d, (char)(0xc0 | (c >> 6)), st, link);
        emit(d, (char)(0x80 | (c & 63)), st, link);
    } else if (c < 65536) {
        emit(d, (char)(0xe0 | (c >> 12)), st, link);
        emit(d, (char)(0x80 | ((c >> 6) & 63)), st, link);
        emit(d, (char)(0x80 | (c & 63)), st, link);
    } else {
        emit(d, (char)(0xf0 | (c >> 18)), st, link);
        emit(d, (char)(0x80 | ((c >> 12) & 63)), st, link);
        emit(d, (char)(0x80 | ((c >> 6) & 63)), st, link);
        emit(d, (char)(0x80 | (c & 63)), st, link);
    }
}
static void attribute(const char *p, const char *end, const char *name, char out[256]) {
    out[0] = 0;
    while (p < end) {
        while (p < end && (space(*p) || *p == '/'))
            p++;
        const char *key = p;
        while (p < end && !space(*p) && *p != '=' && *p != '>')
            p++;
        size_t len = (size_t)(p - key);
        if (!len) {
            p++;
            continue;
        }
        while (p < end && space(*p))
            p++;
        if (p == end || *p != '=')
            continue;
        p++;
        while (p < end && space(*p))
            p++;
        char quote = 0;
        if (p < end && (*p == '\'' || *p == '"'))
            quote = *p++;
        const char *value = p;
        while (p < end && (quote ? *p != quote : !space(*p) && *p != '>'))
            p++;
        size_t bytes = (size_t)(p - value);
        if (quote && p < end)
            p++;
        bool equal = len == strlen(name);
        for (size_t i = 0; equal && i < len; i++)
            if (lower(key[i]) != name[i])
                equal = false;
        if (equal) {
            if (bytes > 255)
                return;
            size_t in = 0, write = 0;
            while (in < bytes) {
                unsigned cp = 0, used = value[in] == '&' ? entity(value + in, bytes - in, &cp) : 0;
                if (!used) {
                    out[write++] = value[in++];
                    continue;
                }
                in += used;
                if (cp < 128)
                    out[write++] = (char)cp;
                else if (cp < 2048) {
                    out[write++] = (char)(0xc0 | (cp >> 6));
                    out[write++] = (char)(0x80 | (cp & 63));
                } else if (cp < 65536) {
                    out[write++] = (char)(0xe0 | (cp >> 12));
                    out[write++] = (char)(0x80 | ((cp >> 6) & 63));
                    out[write++] = (char)(0x80 | (cp & 63));
                } else {
                    out[write++] = (char)(0xf0 | (cp >> 18));
                    out[write++] = (char)(0x80 | ((cp >> 12) & 63));
                    out[write++] = (char)(0x80 | ((cp >> 6) & 63));
                    out[write++] = (char)(0x80 | (cp & 63));
                }
            }
            out[write] = 0;
            return;
        }
    }
}
void ark_html_parse(ArkHTML *d, const char *s, size_t bytes, bool html) {
    memset(d, 0, sizeof *d);
    unsigned style = 0, link = 0;
    bool pre = false;
    char skip[16] = {0};
    if (bytes > HTML_TEXT_MAX) {
        bytes = HTML_TEXT_MAX;
        d->truncated = true;
    }
    for (size_t i = 0; i < bytes;) {
        if (html && s[i] == '<') {
            if (i + 4 <= bytes && !strncmp(s + i, "<!--", 4)) {
                i += 4;
                while (i + 3 <= bytes && strncmp(s + i, "-->", 3))
                    i++;
                i = i + 3 <= bytes ? i + 3 : bytes;
                continue;
            }
            size_t at = i + 1;
            bool closing = at < bytes && s[at] == '/';
            if (closing)
                at++;
            while (at < bytes && space(s[at]))
                at++;
            char tag[16];
            unsigned k = 0;
            while (at < bytes &&
                   ((s[at] >= 'a' && s[at] <= 'z') || (s[at] >= 'A' && s[at] <= 'Z') ||
                    (s[at] >= '0' && s[at] <= '9'))) {
                if (k < 15)
                    tag[k++] = lower(s[at]);
                at++;
            }
            tag[k] = 0;
            size_t end = at;
            char quote = 0;
            while (end < bytes) {
                char c = s[end];
                if (quote) {
                    if (c == quote)
                        quote = 0;
                } else if (c == '\'' || c == '"')
                    quote = c;
                else if (c == '>')
                    break;
                end++;
            }
            if (end == bytes) {
                i = bytes;
                continue;
            }
            const char *attrs = s + at;
            i = end + 1;
            if (skip[0]) {
                if (closing && !strcmp(tag, skip))
                    skip[0] = 0;
                continue;
            }
            if (!closing &&
                (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "head"))) {
                strcopy(skip, tag, sizeof skip);
                continue;
            }
            bool heading = tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2];
            if (heading || !strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "br") ||
                !strcmp(tag, "li") || !strcmp(tag, "tr") || !strcmp(tag, "pre") ||
                !strcmp(tag, "hr"))
                newline(d);
            if (heading)
                style = closing ? 0 : 1;
            if (!strcmp(tag, "b") || !strcmp(tag, "strong"))
                style = closing ? 0 : 1;
            if (!strcmp(tag, "pre"))
                pre = !closing;
            if (!strcmp(tag, "li") && !closing) {
                emit(d, '*', style, 0);
                emit(d, ' ', style, 0);
            }
            if (!strcmp(tag, "a")) {
                link = 0;
                if (!closing && d->links < HTML_LINK_MAX) {
                    attribute(attrs, s + end, "href", d->href[d->links]);
                    if (d->href[d->links][0])
                        link = ++d->links;
                }
            }
            if (!strcmp(tag, "img") && !closing) {
                char alt[256];
                attribute(attrs, s + end, "alt", alt);
                if (alt[0]) {
                    emit(d, '[', 0, 0);
                    for (unsigned j = 0; alt[j]; j++)
                        emit(d, alt[j], 0, 0);
                    emit(d, ']', 0, 0);
                }
            }
            continue;
        }
        if (skip[0]) {
            i++;
            continue;
        }
        if (html && s[i] == '&') {
            unsigned cp;
            unsigned n = entity(s + i, bytes - i, &cp);
            if (n) {
                codepoint(d, cp, style, link);
                i += n;
                continue;
            }
        }
        char c = s[i++];
        if (html && !pre && space(c)) {
            if (!d->length || space(d->text[d->length - 1]))
                continue;
            c = ' ';
        }
        if ((unsigned char)c >= 32 || c == '\n' || c == '\t')
            emit(d, c, style, link);
    }
}
bool ark_html_resolve(const char *base, const char *href, char out[256]) {
    if (!href || !href[0] || strlen(href) > 255 || strlen(base) > 255)
        return false;
    for (const char *p = href; *p; p++)
        if ((unsigned char)*p < 33 || *p == '\\')
            return false;
    if (!strncmp(href, "http://", 7)) {
        strcopy(out, href, 256);
        return true;
    }
    for (const char *p = href; *p && *p != '/' && *p != '?' && *p != '#'; p++)
        if (*p == ':')
            return false;
    if (strncmp(base, "http://", 7))
        return false;
    if (href[0] == '?' || href[0] == '#') {
        size_t keep = 0;
        while (base[keep] && base[keep] != '#' && (href[0] == '#' || base[keep] != '?'))
            keep++;
        if (keep + strlen(href) > 255)
            return false;
        memcpy(out, base, keep);
        strcopy(out + keep, href, 256 - keep);
        return true;
    }
    char joined[512];
    size_t prefix = 7;
    while (base[prefix] && base[prefix] != '/' && base[prefix] != '?' && base[prefix] != '#')
        prefix++;
    if (href[0] == '/' && href[1] == '/') {
        if (strlen(href) + 5 > 255)
            return false;
        strcopy(out, "http:", 256);
        strcopy(out + 5, href, 251);
        return true;
    }
    size_t n = prefix;
    if (href[0] != '/') {
        n = prefix;
        while (base[n] && base[n] != '?' && base[n] != '#')
            n++;
        while (n > prefix && base[n - 1] != '/')
            n--;
        if (n == prefix) {
            memcpy(joined, base, prefix);
            joined[n++] = '/';
        } else
            memcpy(joined, base, n);
    } else
        memcpy(joined, base, n);
    if (n + strlen(href) >= sizeof joined)
        return false;
    strcopy(joined + n, href, sizeof joined - n);
    /* Canonicalize dot segments; never traverse into the authority. */
    size_t read = prefix, write = prefix;
    memcpy(out, joined, prefix);
    size_t marks[128];
    unsigned depth = 0;
    while (joined[read] && joined[read] != '?' && joined[read] != '#') {
        if (joined[read] == '/') {
            read++;
            continue;
        }
        size_t first = read;
        while (joined[read] && joined[read] != '/' && joined[read] != '?' && joined[read] != '#')
            read++;
        size_t len = read - first;
        if (len == 1 && joined[first] == '.')
            continue;
        if (len == 2 && joined[first] == '.' && joined[first + 1] == '.') {
            if (depth)
                write = marks[--depth];
            continue;
        }
        if (write + 1 + len > 255 || depth == 128)
            return false;
        marks[depth++] = write;
        out[write++] = '/';
        memcpy(out + write, joined + first, len);
        write += len;
    }
    if (write == prefix || (read > 0 && joined[read - 1] == '/')) {
        if (write == 255)
            return false;
        out[write++] = '/';
    }
    size_t tail = strlen(joined + read);
    if (write + tail > 255)
        return false;
    memcpy(out + write, joined + read, tail + 1);
    return true;
}
