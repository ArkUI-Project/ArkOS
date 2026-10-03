#ifndef ARKUI_ICONS_H
#define ARKUI_ICONS_H
#include "arkui_types.h"
typedef enum {
    ARKUI_APP_TERMINAL,
    ARKUI_APP_FILES,
    ARKUI_APP_NOTES,
    ARKUI_APP_SETTINGS,
    ARKUI_APP_ABOUT,
    ARKUI_APP_CALCULATOR,
    ARKUI_APP_BROWSER,
    ARKUI_APP_CLOCK,
    ARKUI_APP_PAINT,
    ARKUI_APP_MARKDOWN,
    ARKUI_APP_TASKS,
    ARKUI_APP_CAPTURE,
    ARKUI_APP_INSTALLER,
    ARKUI_APP_TODO,
    ARKUI_APP_TIMER,
    ARKUI_APP_WASM,
    ARKUI_APP_PACKAGES,
    ARKUI_APP_CALENDAR,
    ARKUI_APP_REMINDERS,
    ARKUI_APP_PACKAGE_APP,
    ARKUI_APP_COUNT
} ArkUIAppIcon;
typedef enum {
    ARKUI_SYMBOL_BACK,
    ARKUI_SYMBOL_FORWARD,
    ARKUI_SYMBOL_NEW_DOCUMENT,
    ARKUI_SYMBOL_FOLDER,
    ARKUI_SYMBOL_SHARE,
    ARKUI_SYMBOL_TRASH,
    ARKUI_SYMBOL_RENAME,
    ARKUI_SYMBOL_COPY,
    ARKUI_SYMBOL_SEARCH,
    ARKUI_SYMBOL_GRID,
    ARKUI_SYMBOL_LIST,
    ARKUI_SYMBOL_LOCK,
    ARKUI_SYMBOL_USER,
    ARKUI_SYMBOL_NETWORK,
    ARKUI_SYMBOL_MINIMIZE,
    ARKUI_SYMBOL_MAXIMIZE,
    ARKUI_SYMBOL_CLOSE,
    ARKUI_SYMBOL_DROPDOWN,
    ARKUI_SYMBOL_REFRESH,
    ARKUI_SYMBOL_CHECK,
    ARKUI_SYMBOL_MORE,
    ARKUI_SYMBOL_HOME,
    ARKUI_SYMBOL_POWER,
    ARKUI_SYMBOL_SETTINGS,
    ARKUI_SYMBOL_DOCUMENT,
    ARKUI_SYMBOL_POINTER,
    ARKUI_SYMBOL_COUNT
} ArkUISymbol;

ArkUISurface arkui_surface(uint32_t *pixels, int stride, int width, int height);
/* Original geometry. No Apple icon font, images, SF Symbols or trademarks.
 * Cached antialiased app sprites are 96px; symbols use 32px alpha coverage.
 * The destination is XRGB8888, size is 1..512, clipping is always honored. */
void arkui_app_icon(ArkUISurface *, int x, int y, int size, ArkUIAppIcon);
void arkui_symbol(ArkUISurface *, int x, int y, int size, ArkUISymbol, uint32_t rgb);
const char *arkui_symbol_name(ArkUISymbol);
#endif
