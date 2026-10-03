#ifndef ARK_SDK_TEXT_H
#define ARK_SDK_TEXT_H
#include "app.h"
static inline bool app_insert(char *s, size_t *length, size_t *cursor, size_t capacity,
                              const char *text) {
    size_t n = strlen(text);
    if (*length >= capacity || *cursor > *length || n >= capacity - *length)
        return false;
    memmove(s + *cursor + n, s + *cursor, *length - *cursor + 1);
    memcpy(s + *cursor, text, n);
    *length += n;
    *cursor += n;
    return true;
}
static inline bool app_edit(char *s, size_t *length, size_t *cursor, size_t capacity, int key) {
    if (key == KEY_BACKSPACE && *cursor) {
        size_t p = utf8_prev(s, *cursor);
        memmove(s + p, s + *cursor, *length - *cursor + 1);
        *length -= *cursor - p;
        *cursor = p;
    } else if (key == KEY_DELETE && *cursor < *length) {
        const char *p = s + *cursor;
        utf8_decode(&p);
        size_t n = (size_t)(p - s) - *cursor;
        memmove(s + *cursor, p, *length - *cursor - n + 1);
        *length -= n;
    } else if (key == KEY_LEFT && *cursor)
        *cursor = utf8_prev(s, *cursor);
    else if (key == KEY_RIGHT && *cursor < *length) {
        const char *p = s + *cursor;
        utf8_decode(&p);
        *cursor = (size_t)(p - s);
    } else if (key == KEY_HOME)
        *cursor = 0;
    else if (key == KEY_END)
        *cursor = *length;
    else if (key == KEY_ENTER)
        return app_insert(s, length, cursor, capacity, "\n");
    else if (key >= 32 && key < 127) {
        char b[2] = {(char)key, 0};
        return app_insert(s, length, cursor, capacity, b);
    } else
        return false;
    return true;
}
static inline void app_clip(ArkApp *a, int x, int y, const char *s, int width, uint32_t color) {
    char b[512];
    size_t n = utf8_fit(s, width);
    if (n >= sizeof b) {
        n = sizeof b - 1;
        while (n && ((unsigned char)s[n] & 0xc0) == 0x80)
            n--;
    }
    memcpy(b, s, n);
    b[n] = 0;
    app_text(a, x, y, b, color, 1);
}
static inline bool app_codepoint(char *s, size_t *length, size_t *cursor, size_t capacity,
                                 uint32_t cp) {
    char b[5] = {0};
    if (cp < 32 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return false;
    if (cp < 128)
        b[0] = (char)cp;
    else if (cp < 2048) {
        b[0] = (char)(0xc0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 63));
    } else if (cp < 65536) {
        b[0] = (char)(0xe0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 63));
        b[2] = (char)(0x80 | (cp & 63));
    } else {
        b[0] = (char)(0xf0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 63));
        b[2] = (char)(0x80 | ((cp >> 6) & 63));
        b[3] = (char)(0x80 | (cp & 63));
    }
    return app_insert(s, length, cursor, capacity, b);
}
#endif
