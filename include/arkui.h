#ifndef ARKUI_H
#define ARKUI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arkui_types.h"
#include "arkui_icons.h"
#include "arkui_animation.h"

#define ARKUI_MAX_NODES 128
#define ARKUI_TEXT_CAP 8192
#define ARKUI_AUTO (-1)
#define ARKUI_FILL (-2)
typedef int ArkUIId;
typedef enum {
    ARKUI_VSTACK,
    ARKUI_HSTACK,
    ARKUI_TEXT,
    ARKUI_BUTTON,
    ARKUI_TOGGLE,
    ARKUI_SLIDER,
    ARKUI_SPACER,
    ARKUI_CARD,
    ARKUI_TOOLBAR,
    ARKUI_ICON_BUTTON,
    ARKUI_SYMBOL_VIEW,
    ARKUI_SEPARATOR,
    ARKUI_NAVIGATION
} ArkUIKind;
typedef enum { ARKUI_START, ARKUI_CENTER, ARKUI_END } ArkUIAlign;
typedef enum {
    ARKUI_MATERIAL_NONE,
    ARKUI_MATERIAL_CONTENT,
    ARKUI_MATERIAL_SIDEBAR,
    ARKUI_MATERIAL_CHROME,
    ARKUI_MATERIAL_POPOVER
} ArkUIMaterial;
typedef struct {
    int width, height, min_width, min_height, max_width, max_height;
    int padding, gap, flex;
    ArkUIAlign align;
} ArkUIStyle;
typedef struct {
    ArkUIKind kind;
    ArkUIStyle style;
    ArkUIRect frame, clip;
    int parent, first_child, last_child, next_sibling;
    int action, minimum, maximum;
    int measured_w, measured_h;
    ArkUISymbol symbol;
    ArkUIMaterial material;
    const char *text;
    bool *boolean;
    int *integer;
    bool enabled, hit_enabled, selected, secondary;
} ArkUINode;
typedef struct {
    ArkUINode nodes[ARKUI_MAX_NODES];
    char strings[ARKUI_TEXT_CAP];
    int count, string_count, capture_action, hover_action;
    bool pointer_down, dirty, overflow;
    ArkUIRect bounds;
} ArkUI;
typedef struct {
    void *context;
    /* Every callback MUST honor clip. Coordinates are absolute pixels.
     * Use designated initializers: future callbacks may be appended. */
    void (*rect)(void *, ArkUIRect, int radius, uint32_t rgb, int alpha, ArkUIRect clip);
    void (*text)(void *, int x, int y, const char *, uint32_t rgb, ArkUIRect clip);
    void (*symbol)(void *, int x, int y, int size, ArkUISymbol, uint32_t rgb, ArkUIRect clip);
    /* Optional real backdrop/material renderer. Without it, a tint is painted. */
    void (*material)(void *, ArkUIRect, int radius, ArkUIMaterial, uint32_t tint, int alpha,
                     ArkUIRect clip);
} ArkUIPainter;
typedef struct {
    uint32_t text, secondary, card, control, accent, on_accent, border;
    int card_alpha, control_alpha;
} ArkUITheme;
typedef struct {
    int id, value;
    bool changed;
} ArkUIAction;

/* Root node is always 0 (a VStack). Reset rebuilds the tree but preserves an
 * in-progress pointer capture by stable action ID. Action IDs must be unique. */
void arkui_init(ArkUI *ui);
void arkui_reset(ArkUI *ui);
ArkUIId arkui_vstack(ArkUI *, ArkUIId parent, int padding, int gap);
ArkUIId arkui_hstack(ArkUI *, ArkUIId parent, int padding, int gap);
ArkUIId arkui_card(ArkUI *, ArkUIId parent, int padding, int gap);
/* Horizontal navigation/control surface. Default height 44, centered children. */
ArkUIId arkui_toolbar(ArkUI *, ArkUIId parent, int padding, int gap);
ArkUIId arkui_icon_button(ArkUI *, ArkUIId parent, ArkUISymbol, const char *accessible_label,
                          int action);
ArkUIId arkui_symbol_view(ArkUI *, ArkUIId parent, ArkUISymbol);
ArkUIId arkui_separator(ArkUI *, ArkUIId parent);
ArkUIId arkui_text(ArkUI *, ArkUIId parent, const char *text);
ArkUIId arkui_button(ArkUI *, ArkUIId parent, const char *text, int action);
ArkUIId arkui_navigation(ArkUI *, ArkUIId parent, const char *text, ArkUISymbol, int action);
ArkUIId arkui_toggle(ArkUI *, ArkUIId parent, const char *text, bool *binding, int action);
ArkUIId arkui_slider(ArkUI *, ArkUIId parent, const char *text, int *binding, int minimum,
                     int maximum, int action);
ArkUIId arkui_spacer(ArkUI *, ArkUIId parent, int weight);
ArkUINode *arkui_node(ArkUI *, ArkUIId);
void arkui_size(ArkUI *, ArkUIId, int width, int height);
void arkui_flex(ArkUI *, ArkUIId, int weight);
void arkui_material(ArkUI *, ArkUIId, ArkUIMaterial);
void arkui_layout(ArkUI *, ArkUIRect bounds);
void arkui_draw(ArkUI *, const ArkUIPainter *, const ArkUITheme *);
int arkui_hit_test(const ArkUI *, int x, int y); /* stable action ID, 0 = none */
bool arkui_pointer(ArkUI *, int x, int y, bool down, ArkUIAction *result);
void arkui_cancel_pointer(ArkUI *);
void arkui_invalidate(ArkUI *);
bool arkui_take_dirty(ArkUI *);
ArkUIRect arkui_intersection(ArkUIRect, ArkUIRect);
const char *arkui_hover_label(const ArkUI *); /* copied node label, or "" */
ArkUITheme arkui_theme(bool dark);
/* Plain CPU painter for user-owned XRGB canvas; no hardware/kernel calls.
 * Materials use a tint fallback. Surface and pixels must outlive the painter. */
ArkUIPainter arkui_canvas_painter(ArkUISurface *);

/* Bounded invalidation batching. Overlapping/touching regions coalesce; a ninth
 * disjoint region falls back to the full viewport. Presentation is host-owned. */
#define ARKUI_DAMAGE_CAP 8
typedef struct {
    ArkUIRect bounds, rects[ARKUI_DAMAGE_CAP];
    unsigned count;
    bool full;
} ArkUIDamage;
void arkui_damage_reset(ArkUIDamage *, ArkUIRect bounds);
void arkui_damage_add(ArkUIDamage *, ArkUIRect);
unsigned arkui_damage_take(ArkUIDamage *, ArkUIRect out[ARKUI_DAMAGE_CAP]);
#endif
