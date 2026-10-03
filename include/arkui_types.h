#ifndef ARKUI_TYPES_H
#define ARKUI_TYPES_H
#include <stdint.h>
typedef struct {
    int x, y, w, h;
} ArkUIRect;
/* Native user-space canvas. stride is uint32_t pixels, never bytes. */
typedef struct {
    uint32_t *pixels;
    int width, height, stride;
    ArkUIRect clip;
} ArkUISurface;
#endif
