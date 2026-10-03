#include "arkui.h"
#include "unicode.h"
#include "raster.h"
#include <limits.h>

/* Bounded declarative views with retained layout and stable input identity.
 * The host owns application state and schedules a redraw after invalidation. */
static int smaller(int a, int b) {
    return a < b ? a : b;
}
static int larger(int a, int b) {
    return a > b ? a : b;
}
static int bounded(int n, int lo, int hi) {
    return larger(lo, smaller(n, hi));
}
static int dimension(int n) {
    return bounded(n, 0, 16384);
}
static bool container(ArkUIKind kind) {
    return kind == ARKUI_VSTACK || kind == ARKUI_HSTACK || kind == ARKUI_CARD ||
           kind == ARKUI_TOOLBAR;
}
static bool horizontal(ArkUIKind kind) {
    return kind == ARKUI_HSTACK || kind == ARKUI_TOOLBAR;
}
static bool contains(ArkUIRect r, int x, int y) {
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && (int64_t)x - r.x < r.w &&
           (int64_t)y - r.y < r.h;
}
ArkUIRect arkui_intersection(ArkUIRect a, ArkUIRect b) {
    int x = larger(a.x, b.x), y = larger(a.y, b.y);
    int64_t ar = (int64_t)a.x + a.w, br = (int64_t)b.x + b.w;
    int64_t ab = (int64_t)a.y + a.h, bb = (int64_t)b.y + b.h;
    int64_t w = (ar < br ? ar : br) - x, h = (ab < bb ? ab : bb) - y;
    return (ArkUIRect){x, y,
                       w < 0         ? 0
                       : w > INT_MAX ? INT_MAX
                                     : (int)w,
                       h < 0         ? 0
                       : h > INT_MAX ? INT_MAX
                                     : (int)h};
}

static ArkUIId add(ArkUI *ui, int parent, ArkUIKind kind, const char *text, int action) {
    if (action < 0 || ui->count >= ARKUI_MAX_NODES || parent < 0 || parent >= ui->count ||
        !container(ui->nodes[parent].kind)) {
        ui->overflow = true;
        return -1;
    }
    if (action)
        for (int i = 0; i < ui->count; i++)
            if (ui->nodes[i].action == action) {
                ui->overflow = true;
                return -1;
            }
    int length = 0;
    if (text && ui->string_count >= ARKUI_TEXT_CAP) {
        ui->overflow = true;
        return -1;
    }
    if (text)
        while (text[length]) {
            if (length >= ARKUI_TEXT_CAP - ui->string_count - 1) {
                ui->overflow = true;
                return -1;
            }
            length++;
        }
    ArkUIId id = ui->count++;
    ArkUINode *n = &ui->nodes[id];
    *n = (ArkUINode){.kind = kind,
                     .parent = parent,
                     .first_child = -1,
                     .last_child = -1,
                     .next_sibling = -1,
                     .action = action,
                     .enabled = true,
                     .style = {.width = ARKUI_AUTO,
                               .height = ARKUI_AUTO,
                               .max_width = 16384,
                               .max_height = 16384,
                               .align = ARKUI_START}};
    if (text) {
        char *copy = &ui->strings[ui->string_count];
        for (int i = 0; i <= length; i++)
            copy[i] = text[i];
        n->text = copy;
        ui->string_count += length + 1;
    } else
        n->text = "";
    ArkUINode *p = &ui->nodes[parent];
    if (p->last_child >= 0)
        ui->nodes[p->last_child].next_sibling = id;
    else
        p->first_child = id;
    p->last_child = id;
    ui->dirty = true;
    return id;
}
void arkui_reset(ArkUI *ui) {
    ui->count = 1;
    ui->string_count = 0;
    ui->overflow = false;
    ui->dirty = true;
    ui->nodes[0] = (ArkUINode){
        .kind = ARKUI_VSTACK,
        .parent = -1,
        .first_child = -1,
        .last_child = -1,
        .next_sibling = -1,
        .enabled = true,
        .text = "",
        .style = {
            .width = ARKUI_FILL, .height = ARKUI_FILL, .max_width = 16384, .max_height = 16384}};
}
void arkui_init(ArkUI *ui) {
    ui->capture_action = 0;
    ui->hover_action = 0;
    ui->pointer_down = false;
    ui->bounds = (ArkUIRect){0, 0, 0, 0};
    arkui_reset(ui);
}
static ArkUIId stack(ArkUI *ui, int parent, ArkUIKind kind, int padding, int gap) {
    int id = add(ui, parent, kind, 0, 0);
    if (id >= 0) {
        ui->nodes[id].style.padding = dimension(padding);
        ui->nodes[id].style.gap = dimension(gap);
    }
    return id;
}
ArkUIId arkui_vstack(ArkUI *u, int p, int pad, int gap) {
    return stack(u, p, ARKUI_VSTACK, pad, gap);
}
ArkUIId arkui_hstack(ArkUI *u, int p, int pad, int gap) {
    return stack(u, p, ARKUI_HSTACK, pad, gap);
}
ArkUIId arkui_card(ArkUI *u, int p, int pad, int gap) {
    return stack(u, p, ARKUI_CARD, pad, gap);
}
ArkUIId arkui_toolbar(ArkUI *u, int p, int pad, int gap) {
    int id = stack(u, p, ARKUI_TOOLBAR, pad, gap);
    if (id >= 0) {
        u->nodes[id].style.height = 44;
        u->nodes[id].style.align = ARKUI_CENTER;
        u->nodes[id].material = ARKUI_MATERIAL_CHROME;
    }
    return id;
}
ArkUIId arkui_icon_button(ArkUI *u, int p, ArkUISymbol symbol, const char *label, int action) {
    if (symbol < 0 || symbol >= ARKUI_SYMBOL_COUNT) {
        u->overflow = true;
        return -1;
    }
    int id = add(u, p, ARKUI_ICON_BUTTON, label ? label : arkui_symbol_name(symbol), action);
    if (id >= 0)
        u->nodes[id].symbol = symbol;
    return id;
}
ArkUIId arkui_symbol_view(ArkUI *u, int p, ArkUISymbol symbol) {
    if (symbol < 0 || symbol >= ARKUI_SYMBOL_COUNT) {
        u->overflow = true;
        return -1;
    }
    int id = add(u, p, ARKUI_SYMBOL_VIEW, 0, 0);
    if (id >= 0)
        u->nodes[id].symbol = symbol;
    return id;
}
ArkUIId arkui_separator(ArkUI *u, int p) {
    return add(u, p, ARKUI_SEPARATOR, 0, 0);
}
ArkUIId arkui_text(ArkUI *u, int p, const char *s) {
    return add(u, p, ARKUI_TEXT, s, 0);
}
ArkUIId arkui_button(ArkUI *u, int p, const char *s, int action) {
    return add(u, p, ARKUI_BUTTON, s, action);
}
ArkUIId arkui_navigation(ArkUI *u, int p, const char *s, ArkUISymbol icon, int action) {
    int id = add(u, p, ARKUI_NAVIGATION, s, action);
    if (id >= 0) {
        u->nodes[id].symbol = icon;
        u->nodes[id].style.height = 40;
    }
    return id;
}
ArkUIId arkui_toggle(ArkUI *u, int p, const char *s, bool *binding, int action) {
    int id = add(u, p, ARKUI_TOGGLE, s, action);
    if (id >= 0) {
        u->nodes[id].boolean = binding;
        u->nodes[id].enabled = binding != 0;
    }
    return id;
}
ArkUIId arkui_slider(ArkUI *u, int p, const char *s, int *binding, int low, int high, int action) {
    int id = add(u, p, ARKUI_SLIDER, s, action);
    if (id >= 0) {
        ArkUINode *n = &u->nodes[id];
        n->integer = binding;
        n->minimum = low;
        n->maximum = larger(low, high);
        n->enabled = binding && high > low;
        if (binding)
            *binding = bounded(*binding, n->minimum, n->maximum);
    }
    return id;
}
ArkUIId arkui_spacer(ArkUI *u, int p, int weight) {
    int id = add(u, p, ARKUI_SPACER, 0, 0);
    if (id >= 0)
        u->nodes[id].style.flex = bounded(weight, 1, 1024);
    return id;
}
ArkUINode *arkui_node(ArkUI *ui, int id) {
    return id >= 0 && id < ui->count ? &ui->nodes[id] : 0;
}
void arkui_size(ArkUI *ui, int id, int w, int h) {
    ArkUINode *n = arkui_node(ui, id);
    if (!n)
        return;
    n->style.width = w < 0 ? (w == ARKUI_FILL ? ARKUI_FILL : ARKUI_AUTO) : dimension(w);
    n->style.height = h < 0 ? (h == ARKUI_FILL ? ARKUI_FILL : ARKUI_AUTO) : dimension(h);
    ui->dirty = true;
}
void arkui_flex(ArkUI *ui, int id, int weight) {
    ArkUINode *n = arkui_node(ui, id);
    if (n) {
        n->style.flex = bounded(weight, 0, 1024);
        ui->dirty = true;
    }
}
void arkui_material(ArkUI *ui, int id, ArkUIMaterial material) {
    ArkUINode *n = arkui_node(ui, id);
    if (!n || material < ARKUI_MATERIAL_NONE || material > ARKUI_MATERIAL_POPOVER)
        return;
    n->material = material;
    ui->dirty = true;
}
static int constrained(int natural, int explicit_size, int minimum, int maximum) {
    minimum = dimension(minimum);
    maximum = larger(minimum, dimension(maximum));
    return bounded(explicit_size >= 0 ? dimension(explicit_size) : natural, minimum, maximum);
}
static void measure(ArkUI *ui, int id) {
    ArkUINode *n = &ui->nodes[id];
    int w = utf8_width(n->text), h = 22;
    if (container(n->kind)) {
        w = h = 0;
        int count = 0;
        for (int c = n->first_child; c >= 0; c = ui->nodes[c].next_sibling) {
            measure(ui, c);
            ArkUINode *child = &ui->nodes[c];
            if (horizontal(n->kind)) {
                w += child->measured_w;
                h = larger(h, child->measured_h);
            } else {
                h += child->measured_h;
                w = larger(w, child->measured_w);
            }
            count++;
        }
        int gaps = larger(0, count - 1) * dimension(n->style.gap);
        if (horizontal(n->kind))
            w += gaps;
        else
            h += gaps;
        w += dimension(n->style.padding) * 2;
        h += dimension(n->style.padding) * 2;
    } else if (n->kind == ARKUI_BUTTON) {
        w += 28;
        h = 36;
    } else if (n->kind == ARKUI_ICON_BUTTON)
        w = h = 36;
    else if (n->kind == ARKUI_SYMBOL_VIEW)
        w = h = 20;
    else if (n->kind == ARKUI_SEPARATOR) {
        bool row = horizontal(ui->nodes[n->parent].kind);
        w = row ? 1 : 24;
        h = row ? 24 : 1;
    } else if (n->kind == ARKUI_TOGGLE) {
        w += 76;
        h = 40;
    } else if (n->kind == ARKUI_SLIDER) {
        w = larger(w + 56, 160);
        h = 58;
    } else if (n->kind == ARKUI_SPACER)
        w = h = 0;
    else
        for (const char *s = n->text; *s; s++)
            if (*s == '\n')
                h += 22;
    n->measured_w = constrained(w, n->style.width, n->style.min_width, n->style.max_width);
    n->measured_h = constrained(h, n->style.height, n->style.min_height, n->style.max_height);
}
static void arrange(ArkUI *ui, int id, ArkUIRect frame, ArkUIRect clip, bool parent_enabled) {
    ArkUINode *n = &ui->nodes[id];
    n->frame = frame;
    n->clip = arkui_intersection(frame, clip);
    /* Descendants of disabled containers never receive input. */
    n->hit_enabled = parent_enabled && n->enabled;
    if (!container(n->kind))
        return;
    int padding = smaller(dimension(n->style.padding), smaller(frame.w, frame.h) / 2);
    ArkUIRect inner = {frame.x + padding, frame.y + padding, larger(0, frame.w - 2 * padding),
                       larger(0, frame.h - 2 * padding)};
    bool is_horizontal = horizontal(n->kind);
    int main = is_horizontal ? inner.w : inner.h, cross = is_horizontal ? inner.h : inner.w;
    int count = 0, natural = 0, weights = 0;
    for (int c = n->first_child; c >= 0; c = ui->nodes[c].next_sibling) {
        ArkUINode *child = &ui->nodes[c];
        count++;
        natural += is_horizontal ? child->measured_w : child->measured_h;
        int requested = is_horizontal ? child->style.width : child->style.height;
        weights += bounded(child->style.flex, 0, 1024) + (requested == ARKUI_FILL ? 1 : 0);
    }
    int gap = count > 1 ? smaller(dimension(n->style.gap), main / (count - 1)) : 0;
    int available = larger(0, main - gap * larger(0, count - 1));
    int extra = larger(0, available - natural), left = available, position = 0, previous_scaled = 0,
        sum = 0;
    ArkUIRect child_clip = arkui_intersection(n->clip, inner);
    for (int c = n->first_child; c >= 0; c = ui->nodes[c].next_sibling) {
        ArkUINode *child = &ui->nodes[c];
        int size = is_horizontal ? child->measured_w : child->measured_h;
        int requested = is_horizontal ? child->style.width : child->style.height;
        int weight = bounded(child->style.flex, 0, 1024) + (requested == ARKUI_FILL ? 1 : 0);
        if (natural > available) {
            sum += size;
            int scaled = (int)((int64_t)sum * available / natural);
            size = scaled - previous_scaled;
            previous_scaled = scaled;
        } else if (weights && weight) {
            int share = (int)((int64_t)extra * weight / weights);
            int maximum = is_horizontal ? child->style.max_width : child->style.max_height;
            share = smaller(share, larger(0, dimension(maximum) - size));
            size += share;
            extra -= share;
            weights -= weight;
        }
        size = smaller(size, left);
        left -= size;
        int cross_request = is_horizontal ? child->style.height : child->style.width;
        int cross_size = is_horizontal ? child->measured_h : child->measured_w;
        /* VStack/Card children stretch horizontally; HStack centers vertically. */
        if (cross_request == ARKUI_FILL || (!is_horizontal && cross_request == ARKUI_AUTO))
            cross_size = cross;
        int cross_max = is_horizontal ? child->style.max_height : child->style.max_width;
        cross_size = smaller(cross, smaller(cross_size, dimension(cross_max)));
        int shift = n->style.align == ARKUI_END      ? cross - cross_size
                    : n->style.align == ARKUI_CENTER ? (cross - cross_size) / 2
                                                     : 0;
        ArkUIRect child_frame =
            is_horizontal ? (ArkUIRect){inner.x + position, inner.y + shift, size, cross_size}
                          : (ArkUIRect){inner.x + shift, inner.y + position, cross_size, size};
        arrange(ui, c, child_frame, child_clip, n->hit_enabled);
        position += size + gap;
    }
}
void arkui_layout(ArkUI *ui, ArkUIRect bounds) {
    bounds.x = bounded(bounds.x, -1048576, 1048576);
    bounds.y = bounded(bounds.y, -1048576, 1048576);
    bounds.w = dimension(bounds.w);
    bounds.h = dimension(bounds.h);
    ui->bounds = bounds;
    measure(ui, 0);
    arrange(ui, 0, bounds, bounds, true);
    ui->dirty = true;
}
static ArkUINode *action_node(ArkUI *ui, int action) {
    if (!action)
        return 0;
    for (int i = 0; i < ui->count; i++)
        if (ui->nodes[i].action == action)
            return &ui->nodes[i];
    return 0;
}
int arkui_hit_test(const ArkUI *ui, int x, int y) {
    for (int i = ui->count - 1; i >= 0; i--) {
        const ArkUINode *n = &ui->nodes[i];
        if (n->hit_enabled && n->action && contains(n->clip, x, y) && contains(n->frame, x, y))
            return n->action;
    }
    return 0;
}
static bool set_slider(ArkUINode *n, int x, ArkUIAction *result) {
    if (!n->integer || n->maximum <= n->minimum)
        return false;
    int width = larger(1, n->frame.w - 20), offset = bounded(x - n->frame.x - 10, 0, width);
    int value = (int)((int64_t)n->minimum +
                      (((int64_t)n->maximum - n->minimum) * offset + width / 2) / width);
    if (*n->integer == value)
        return false;
    *n->integer = value;
    *result = (ArkUIAction){n->action, value, true};
    return true;
}
bool arkui_pointer(ArkUI *ui, int x, int y, bool down, ArkUIAction *result) {
    ArkUIAction ignored = {0};
    if (!result)
        result = &ignored;
    *result = (ArkUIAction){0};
    int hit = arkui_hit_test(ui, x, y);
    bool consumed = ui->capture_action != 0;
    if (hit != ui->hover_action) {
        ui->hover_action = hit;
        ui->dirty = true;
    }
    if (down && !ui->pointer_down) {
        ui->capture_action = hit;
        consumed = hit != 0;
        ui->dirty = true;
    }
    ArkUINode *n = action_node(ui, ui->capture_action);
    if (n && n->hit_enabled) {
        if (down && n->kind == ARKUI_SLIDER && set_slider(n, x, result))
            ui->dirty = true;
        if (!down && ui->pointer_down && hit == n->action) {
            if (n->kind == ARKUI_TOGGLE && n->boolean) {
                *n->boolean = !*n->boolean;
                *result = (ArkUIAction){n->action, *n->boolean, true};
            } else if (n->kind == ARKUI_BUTTON || n->kind == ARKUI_ICON_BUTTON ||
                       n->kind == ARKUI_NAVIGATION)
                *result = (ArkUIAction){n->action, 0, true};
        }
    }
    if (!down && ui->pointer_down) {
        ui->capture_action = 0;
        ui->dirty = true;
    }
    ui->pointer_down = down;
    return consumed;
}
void arkui_cancel_pointer(ArkUI *ui) {
    ui->capture_action = 0;
    ui->pointer_down = false;
    ui->dirty = true;
}
void arkui_invalidate(ArkUI *ui) {
    ui->dirty = true;
}
bool arkui_take_dirty(ArkUI *ui) {
    bool dirty = ui->dirty;
    ui->dirty = false;
    return dirty;
}
const char *arkui_hover_label(const ArkUI *ui) {
    if (ui)
        for (int i = 0; i < ui->count; i++)
            if (ui->nodes[i].action && ui->nodes[i].action == ui->hover_action)
                return ui->nodes[i].text;
    return "";
}
ArkUITheme arkui_theme(bool dark) {
    return dark ? (ArkUITheme){0xe8edf5, 0xa2adbd, 0x252a33, 0x4b5260, 0x458fff,
                               0xffffff, 0x7d8797, 242,      150}
                : (ArkUITheme){0x202b3a, 0x657386, 0xf8fafc, 0xdbe3ec, 0x1979eb,
                               0xffffff, 0x8f9daf, 248,      150};
}

static void paint_rect(const ArkUIPainter *p, ArkUIRect r, int radius, uint32_t rgb, int alpha,
                       ArkUIRect clip) {
    clip = arkui_intersection(r, clip);
    if (r.w > 0 && r.h > 0 && clip.w > 0 && clip.h > 0 && p->rect)
        p->rect(p->context, r, radius, rgb, alpha, clip);
}
static void paint_text(const ArkUIPainter *p, int x, int y, const char *text, uint32_t rgb,
                       ArkUIRect clip) {
    if (clip.w > 0 && clip.h > 0 && p->text)
        p->text(p->context, x, y, text, rgb, clip);
}
static void paint_material(const ArkUIPainter *p, ArkUIRect r, ArkUIRect clip,
                           ArkUIMaterial material, const ArkUITheme *theme) {
    int radius = material == ARKUI_MATERIAL_CONTENT  ? 0
                 : material == ARKUI_MATERIAL_CHROME ? 12
                                                     : 16;
    int alpha = material == ARKUI_MATERIAL_CONTENT   ? 255
                : material == ARKUI_MATERIAL_SIDEBAR ? 210
                : material == ARKUI_MATERIAL_POPOVER ? 238
                                                     : 170;
    if (p->material)
        p->material(p->context, r, radius, material, theme->card, alpha, clip);
    else
        paint_rect(p, r, radius, theme->card, alpha, clip);
}
static void signed_string(int number, char out[16]) {
    uint32_t n;
    int at = 0;
    if (number < 0) {
        out[at++] = '-';
        n = (uint32_t)(-(int64_t)number);
    } else
        n = (uint32_t)number;
    char reversed[12];
    int count = 0;
    do {
        reversed[count++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (count)
        out[at++] = reversed[--count];
    out[at] = 0;
}
void arkui_draw(ArkUI *ui, const ArkUIPainter *p, const ArkUITheme *theme) {
    if (!p || !theme)
        return;
    for (int i = 0; i < ui->count; i++) {
        ArkUINode *n = &ui->nodes[i];
        ArkUIRect r = n->frame, clip = n->clip;
        if (clip.w <= 0 || clip.h <= 0)
            continue;
        uint32_t color = n->secondary || !n->hit_enabled ? theme->secondary : theme->text;
        bool pressed = ui->pointer_down && ui->capture_action == n->action && n->action;
        bool hover = ui->hover_action == n->action && n->action;
        if (n->material != ARKUI_MATERIAL_NONE)
            paint_material(p, r, clip, n->material, theme);
        if (n->kind == ARKUI_CARD && n->material == ARKUI_MATERIAL_NONE)
            paint_rect(p, r, 14, theme->card, theme->card_alpha, clip);
        else if (n->kind == ARKUI_TEXT)
            paint_text(p, r.x, r.y, n->text, color, clip);
        else if (n->kind == ARKUI_ICON_BUTTON || n->kind == ARKUI_SYMBOL_VIEW) {
            if (n->kind == ARKUI_ICON_BUTTON && (hover || pressed || n->selected))
                paint_rect(p, r, 9, n->selected || pressed ? theme->accent : theme->control,
                           n->selected || pressed ? 230 : theme->control_alpha, clip);
            int size = smaller(20, smaller(r.w, r.h));
            if (p->symbol && size > 0)
                p->symbol(p->context, r.x + (r.w - size) / 2, r.y + (r.h - size) / 2, size,
                          n->symbol, n->selected || pressed ? theme->on_accent : color, clip);
        } else if (n->kind == ARKUI_SEPARATOR)
            paint_rect(p, r, 0, theme->border, 90, clip);
        else if (n->kind == ARKUI_NAVIGATION) {
            if (hover || pressed)
                paint_rect(p, r, 9, theme->control, theme->control_alpha, clip);
            if (p->symbol) {
                p->symbol(p->context, r.x + 4, r.y + (r.h - 24) / 2, 24, n->symbol,
                          theme->secondary, clip);
                p->symbol(p->context, r.x + r.w - 20, r.y + (r.h - 16) / 2, 16,
                          ARKUI_SYMBOL_FORWARD, theme->secondary, clip);
            }
            paint_text(p, r.x + 40, r.y + (r.h - 20) / 2, n->text, color, clip);
        } else if (n->kind == ARKUI_BUTTON) {
            paint_rect(p, r, 10, n->selected || pressed ? theme->accent : theme->control,
                       n->selected || pressed
                           ? 240
                           : smaller(255, theme->control_alpha + (hover ? 32 : 0)),
                       clip);
            paint_text(p, r.x + larger(4, (r.w - utf8_width(n->text)) / 2), r.y + (r.h - 20) / 2,
                       n->text, n->selected || pressed ? theme->on_accent : color, clip);
        } else if (n->kind == ARKUI_TOGGLE) {
            ArkUIRect text_clip =
                arkui_intersection(clip, (ArkUIRect){r.x, r.y, larger(0, r.w - 65), r.h});
            paint_text(p, r.x, r.y + (r.h - 20) / 2, n->text, color, text_clip);
            bool value = n->boolean && *n->boolean;
            ArkUIRect track = {r.x + larger(0, r.w - 52), r.y + (r.h - 28) / 2, smaller(r.w, 52),
                               28};
            paint_rect(p, track, 14, value ? theme->accent : theme->border, 235, clip);
            ArkUIRect knob = {track.x + (value ? larger(3, track.w - 25) : 3), track.y + 3, 22, 22};
            paint_rect(p, knob, 11, 0xffffff, 255, clip);
        } else if (n->kind == ARKUI_SLIDER) {
            char value[16];
            signed_string(n->integer ? *n->integer : n->minimum, value);
            int number_width = utf8_width(value);
            paint_text(p, r.x, r.y, n->text, color,
                       arkui_intersection(
                           clip, (ArkUIRect){r.x, r.y, larger(0, r.w - number_width - 12), 22}));
            paint_text(p, r.x + r.w - number_width, r.y, value, theme->secondary, clip);
            int width = larger(0, r.w - 20), x = 0;
            if (n->maximum > n->minimum && n->integer)
                x = (int)(((int64_t)bounded(*n->integer, n->minimum, n->maximum) - n->minimum) *
                          width / ((int64_t)n->maximum - n->minimum));
            ArkUIRect track = {r.x + 10, r.y + 40, width, 5};
            paint_rect(p, track, 3, theme->border, 140, clip);
            paint_rect(p, (ArkUIRect){track.x, track.y, x, 5}, 3, theme->accent, 255, clip);
            paint_rect(p, (ArkUIRect){track.x + x - 9, track.y - 7, 18, 18}, 9, theme->accent, 255,
                       clip);
            paint_rect(p, (ArkUIRect){track.x + x - 6, track.y - 4, 12, 12}, 6, 0xffffff, 255,
                       clip);
        }
    }
    ui->dirty = false;
}

static uint32_t blend_pixel(uint32_t old, uint32_t rgb, unsigned alpha) {
    unsigned inv = 255 - alpha;
    unsigned r = (((old >> 16) & 255) * inv + ((rgb >> 16) & 255) * alpha + 127) / 255;
    unsigned g = (((old >> 8) & 255) * inv + ((rgb >> 8) & 255) * alpha + 127) / 255;
    unsigned b = ((old & 255) * inv + (rgb & 255) * alpha + 127) / 255;
    return (r << 16) | (g << 8) | b;
}
static bool valid_surface(const ArkUISurface *s) {
    return s && s->pixels && s->width > 0 && s->height > 0 && s->width <= 16384 &&
           s->height <= 16384 && s->stride >= s->width && s->stride <= 65536;
}
static ArkUIRect surface_clip(const ArkUISurface *s, ArkUIRect clip) {
    return arkui_intersection(arkui_intersection(s->clip, clip),
                              (ArkUIRect){0, 0, s->width, s->height});
}
static void canvas_rect(void *context, ArkUIRect r, int radius, uint32_t rgb, int alpha,
                        ArkUIRect clip) {
    ArkUISurface *s = context;
    if (!valid_surface(s) || r.w <= 0 || r.h <= 0)
        return;
    clip = arkui_intersection(surface_clip(s, clip), r);
    alpha = bounded(alpha, 0, 255);
    for (int y = clip.y; y < clip.y + clip.h; y++)
        for (int x = clip.x; x < clip.x + clip.w; x++) {
            unsigned cover = raster_round_coverage(x - r.x, y - r.y, r.w, r.h, radius);
            unsigned a = (cover * (unsigned)alpha + 127) / 255;
            uint32_t *pixel = &s->pixels[(size_t)y * s->stride + x];
            if (a)
                *pixel = blend_pixel(*pixel, rgb, a);
        }
}
static void canvas_text(void *context, int x, int y, const char *text, uint32_t rgb,
                        ArkUIRect clip) {
    ArkUISurface *s = context;
    if (!valid_surface(s))
        return;
    clip = surface_clip(s, clip);
    if (clip.w <= 0 || clip.h <= 0)
        return;
    unicode_draw(s->pixels + (size_t)clip.y * s->stride + clip.x, s->stride, clip.w, clip.h,
                 x - clip.x, y - clip.y, text, rgb, 1);
}
static void canvas_symbol(void *context, int x, int y, int size, ArkUISymbol symbol, uint32_t rgb,
                          ArkUIRect clip) {
    ArkUISurface *s = context;
    if (!valid_surface(s))
        return;
    ArkUISurface limited = *s;
    limited.clip = surface_clip(s, clip);
    arkui_symbol(&limited, x, y, size, symbol, rgb);
}
ArkUIPainter arkui_canvas_painter(ArkUISurface *surface) {
    return (ArkUIPainter){
        .context = surface, .rect = canvas_rect, .text = canvas_text, .symbol = canvas_symbol};
}
void arkui_damage_reset(ArkUIDamage *damage, ArkUIRect bounds) {
    if (!damage)
        return;
    bounds.x = bounded(bounds.x, -1048576, 1048576);
    bounds.y = bounded(bounds.y, -1048576, 1048576);
    bounds.w = dimension(bounds.w);
    bounds.h = dimension(bounds.h);
    *damage = (ArkUIDamage){.bounds = bounds};
}
void arkui_damage_add(ArkUIDamage *damage, ArkUIRect rect) {
    if (!damage || damage->full)
        return;
    rect = arkui_intersection(rect, damage->bounds);
    if (rect.w <= 0 || rect.h <= 0)
        return;
    /* Restart after merging so an expanded rectangle also joins earlier ones. */
    for (unsigned i = 0; i < damage->count;) {
        ArkUIRect old = damage->rects[i];
        if (rect.x <= old.x + old.w && old.x <= rect.x + rect.w && rect.y <= old.y + old.h &&
            old.y <= rect.y + rect.h) {
            int right = larger(rect.x + rect.w, old.x + old.w),
                bottom = larger(rect.y + rect.h, old.y + old.h);
            rect.x = smaller(rect.x, old.x);
            rect.y = smaller(rect.y, old.y);
            rect.w = right - rect.x;
            rect.h = bottom - rect.y;
            damage->rects[i] = damage->rects[--damage->count];
            i = 0;
        } else
            i++;
    }
    if (damage->count == ARKUI_DAMAGE_CAP) {
        damage->full = true;
        damage->count = 1;
        damage->rects[0] = damage->bounds;
    } else
        damage->rects[damage->count++] = rect;
}
unsigned arkui_damage_take(ArkUIDamage *damage, ArkUIRect out[ARKUI_DAMAGE_CAP]) {
    if (!damage || !out)
        return 0;
    unsigned count = damage->count;
    for (unsigned i = 0; i < count; i++)
        out[i] = damage->rects[i];
    damage->count = 0;
    damage->full = false;
    return count;
}
