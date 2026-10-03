#ifndef ARK_CATALOG_H
#define ARK_CATALOG_H
#include "ark_api.h"
#include "package.h"
/* Stable permission IDs are independent of desktop icon IDs. */
#define ARK_CATALOG(X)                                                                             \
    X(clock, "Clock", "时钟", 7, 4u, 4u)                                                           \
    X(paint, "Paint", "画板", 8, 6u, 6u)                                                           \
    X(markdown, "Markdown", "Markdown", 9, 6u, 6u)                                                 \
    X(notes, "Notes", "文本编辑", 2, 6u, 4u)                                                       \
    X(browser, "Browser", "浏览器", 6, 12u, 4u)                                                    \
    X(calculator, "Calculator", "计算器", 5, 4u, 4u)                                               \
    X(todo, "Todo", "待办事项", 13, 6u, 4u)                                                        \
    X(timer, "Timer", "计时器", 14, 36u, 36u)                                                      \
    X(wasm, "WASM", "应用运行器", 15, 6u, 4u)                                                      \
    X(calendar, "Calendar", "日历", 17, 4u, 4u)                                                    \
    X(reminders, "Reminders", "提醒事项", 18, 36u, 36u)
#define ARK_CATALOG_COUNT 11u
typedef struct {
    const char *program, *title, *label;
    unsigned desktop, maximum, defaults;
} ArkCatalogEntry;
#define ARK_CATALOG_ROW(p, t, l, d, m, g) {#p, t, l, d, m, g},
static const ArkCatalogEntry ark_catalog[ARK_CATALOG_COUNT] = {ARK_CATALOG(ARK_CATALOG_ROW)};
#undef ARK_CATALOG_ROW
static inline int ark_catalog_desktop(int id) {
    for (unsigned i = 0; i < ARK_CATALOG_COUNT; i++)
        if (ark_catalog[i].desktop == (unsigned)id)
            return (int)i;
    return -1;
}
/* Installed package IDs are enumerated by the package service. */
static inline unsigned ark_catalog_package_desktop(unsigned slot) {
    return ARK_PACKAGE_DESKTOP_FIRST + slot;
}
#endif
