/* Live native backdrop effect: geometry is cached, composed pixels are fresh. */
typedef struct {
    int16_t dx, dy, cx, cy;
    uint8_t coverage, highlight;
    uint16_t pad;
} GlassLensPixel;
typedef struct {
    int width, height, radius, band;
    bool dispersion;
    GlassLensPixel *pixels;
    size_t capacity;
    uint64_t stamp;
} GlassLensMap;
static GlassLensMap glass_maps[4];
static uint64_t glass_stamp;
static uint32_t *glass_blur;
static size_t glass_capacity;
static uint32_t glass_rings[8][20 * (MAX_W + 128)];
/* Each worker owns its color cache; no shared tag/value race across CPUs. */
static struct {
    uint32_t key, color;
} glass_colors[8][16384];
static uint32_t glass_vibrancy(unsigned worker, uint32_t raw) {
    unsigned slot = (raw ^ (raw >> 11)) & 16383;
    uint32_t key = glass_colors[worker][slot].key;
    if (key == raw)
        return glass_colors[worker][slot].color;
    uint32_t color = liquid_glass_vibrancy(raw);
    glass_colors[worker][slot].key = raw;
    glass_colors[worker][slot].color = color;
    return color;
}
/* The backdrop includes the lens displacement and Gaussian halo outside the
 * shape. Fully opaque client interiors are excluded only after those halos. */
static bool glass_opaque_client;
typedef struct {
    int x, y, w, h, bw, bh, pad, r, x0, x1, y0, y1;
    unsigned tint, edge;
    uint32_t color;
    bool shadow, opaque;
    int sx0, sx1, sy0, sy1;
    GlassLensMap *map;
} GlassWork;
static GlassWork glass_work;
static uint64_t glass_stage_ms[4];
static uint64_t glass_gpu_stage_ms[6];
static int glass_gpu = -1;
static bool glass_gpu_render(int x, int y, int w, int h, int radius, int tint, int edge,
                             bool dispersion) {
    if (glass_gpu < 0) {
        ArkGlassRequest q = {.op = ARK_GLASS_QUERY};
        glass_gpu = ark_call(ARK_SYS_COMPOSITOR, &q, sizeof q) == 0;
    }
    if (!glass_gpu)
        return false;
    ArkGlassRequest q = {
        .op = ARK_GLASS_RENDER,
        .flags = (dispersion ? ARK_GLASS_DISPERSION : 0) | (shadow_pending ? ARK_GLASS_SHADOW : 0),
        .pixels = (uintptr_t)canvas,
        .width = (uint32_t)sw,
        .height = (uint32_t)sh,
        .stride = (uint32_t)sw,
        .radius = (uint32_t)radius,
        .rect = {x, y, w, h},
        .color = night ? 0x17243d : 0xf3f9ff,
        .tint = (uint32_t)clamp(tint, 0, 255),
        .edge = (uint32_t)clamp(edge, 0, 255),
        .shadow = {shadow_x0, shadow_y0, shadow_x1 - shadow_x0, shadow_y1 - shadow_y0}};
    if (!(q.flags & ARK_GLASS_SHADOW))
        q.shadow = (ArkRect){0};
    uint64_t start = platform_millis();
    int64_t rc = ark_call(ARK_SYS_COMPOSITOR, &q, sizeof q);
    glass_stage_ms[3] += platform_millis() - start;
    if (rc < 0) {
        glass_gpu = 0;
        serial_write("[glass] GPU unavailable; using CPU compositor\n");
        return false;
    }
    glass_gpu_stage_ms[0] = q.validate_ms;
    glass_gpu_stage_ms[1] = q.stage_ms;
    glass_gpu_stage_ms[2] = q.upload_ms;
    glass_gpu_stage_ms[3] = q.submit_ms;
    glass_gpu_stage_ms[4] = q.readback_ms;
    glass_gpu_stage_ms[5] = q.copy_ms;
    if (q.jobs == 1)
        serial_write("[glass] live GPU blur/refraction/dispersion active\n");
    shadow_pending = false;
    return true;
}
static bool glass_buffer(size_t bytes) {
    bytes = (bytes + 4095) & ~(size_t)4095;
    if (bytes <= glass_capacity)
        return true;
    uint32_t *p = ark_memory(bytes);
    if (!p)
        return false;
    if (glass_blur)
        (void)ark_memory_release(glass_blur, glass_capacity);
    glass_blur = p;
    glass_capacity = bytes;
    return true;
}
static size_t glass_map_row(const GlassLensMap *m, int y) {
    int full = m->band * 2 >= m->height ? m->height : m->band;
    if (y < full)
        return (size_t)y * m->width;
    int middle = m->height - m->band * 2;
    if (y < m->height - m->band)
        return (size_t)m->band * m->width + (size_t)(y - m->band) * m->band * 2;
    return (size_t)m->band * m->width + (size_t)middle * m->band * 2 +
           (size_t)(y - (m->height - m->band)) * m->width;
}
static GlassLensPixel *glass_pixel(GlassLensMap *m, int x, int y) {
    size_t row = glass_map_row(m, y);
    if (m->band * 2 >= m->height || y < m->band || y >= m->height - m->band)
        return m->pixels + row + x;
    if (x < m->band)
        return m->pixels + row + x;
    if (x >= m->width - m->band)
        return m->pixels + row + m->band + x - (m->width - m->band);
    return 0;
}
static GlassLensMap *glass_map(int w, int h, int radius, bool dispersion) {
    for (unsigned i = 0; i < 4; i++) {
        GlassLensMap *m = &glass_maps[i];
        if (m->pixels && m->width == w && m->height == h && m->radius == radius &&
            m->dispersion == dispersion) {
            m->stamp = ++glass_stamp;
            return m;
        }
    }
    unsigned slot = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (!glass_maps[i].pixels) {
            slot = i;
            break;
        }
        if (glass_maps[i].stamp < glass_maps[slot].stamp)
            slot = i;
    }
    int band = min(w / 2, max(radius, 12) + 1);
    if (band < 1)
        band = 1;
    size_t count = band * 2 >= h ? (size_t)w * h
                                 : (size_t)w * band * 2 + (size_t)(h - band * 2) * band * 2,
           need = (count * sizeof(GlassLensPixel) + 4095) & ~(size_t)4095;
    GlassLensMap *m = &glass_maps[slot];
    if (need > m->capacity) {
        GlassLensPixel *p = ark_memory(need);
        if (!p)
            return 0;
        if (m->pixels)
            (void)ark_memory_release(m->pixels, m->capacity);
        m->pixels = p;
        m->capacity = need;
    }
    m->width = w;
    m->height = h;
    m->radius = radius;
    m->band = band;
    m->dispersion = dispersion;
    m->stamp = ++glass_stamp;
    for (int y = 0; y < h; y++) {
        int edge = band * 2 >= h || y < band || y >= h - band ? w : band;
        for (int part = 0; part < (edge == w ? 1 : 2); part++)
            for (int col = 0; col < edge; col++) {
                int x = part ? w - band + col : col;
                GlassLensPixel *p = glass_pixel(m, x, y);
                LiquidLens lens;
                liquid_glass_lens(x + .5f, y + .5f, (float)w, (float)h, (float)radius, 12, 24,
                                  false, dispersion, &lens);
                unsigned cover = rounded_coverage(x, y, w, h, radius),
                         inner = rounded_coverage(x - 1, y - 1, w - 2, h - 2, max(0, radius - 1));
                *p = (GlassLensPixel){.dx = (int16_t)(lens.dx * 256),
                                      .dy = (int16_t)(lens.dy * 256),
                                      .cx = (int16_t)(lens.dispersion_x * 256),
                                      .cy = (int16_t)(lens.dispersion_y * 256),
                                      .coverage = (uint8_t)cover,
                                      .highlight = (uint8_t)((cover > inner ? cover - inner : 0) *
                                                             lens.highlight * .5f)};
            }
    }
    return m;
}
static void glass_source_spans(int y, bool horizontal, int *left, int *right) {
    *left = glass_work.bw;
    *right = glass_work.bw;
    int inset = horizontal ? 6 : 0, pad = glass_work.pad;
    if (glass_work.opaque && y >= pad + 98 + inset && y < pad + glass_work.h - 45 - inset) {
        *left = min(glass_work.bw, pad + 45);
        *right = max(*left, pad + glass_work.w - 45);
    }
}
static void glass_gaussian_spans(uint32_t *dst, const uint32_t *const rows[13], int left, int right,
                                 int width) {
    liquid_glass_gaussian_row(dst, rows, left);
    if (right < width) {
        const uint32_t *tail[13];
        for (unsigned k = 0; k < 13; k++)
            tail[k] = rows[k] + right;
        liquid_glass_gaussian_row(dst + right, tail, width - right);
    }
}
static void glass_process_rows(unsigned stage, unsigned worker, int first, int last) {
    int w = glass_work.bw, h = glass_work.bh, pad = glass_work.pad;
    uint32_t *ring = glass_rings[worker];
    if (stage == 0) {
        for (int y = first; y < last; y++) {
            uint32_t line[MAX_W + 140];
            int sy = clamp(glass_work.y + y - pad, 0, sh - 1);
            uint32_t previous = 0, last_color = 0;
            bool known = false;
            int left, right;
            glass_source_spans(y, true, &left, &right);
            for (int x = -6; x < w + 6; x++) {
                if (x == left + 6 && right > left + 12) {
                    x = right - 6;
                    known = false;
                }
                int sx = clamp(glass_work.x + x - pad, 0, sw - 1);
                uint32_t raw = canvas[(size_t)sy * sw + sx];
                if (glass_work.shadow && sx >= glass_work.sx0 && sx < glass_work.sx1 &&
                    sy >= glass_work.sy0 && sy < glass_work.sy1)
                    raw = shadow_color(raw);
                if (!known || raw != previous) {
                    previous = raw;
                    last_color = glass_vibrancy(worker, raw);
                    known = true;
                }
                line[x + 6] = last_color;
            }
            const uint32_t *rows[13];
            for (unsigned k = 0; k < 13; k++)
                rows[k] = line + k;
            glass_gaussian_spans(glass_blur + (size_t)y * w, rows, left, right, w);
        }
        return;
    }
    if (stage == 1) {
        for (int i = 0; i < 13; i++)
            memcpy(ring + (size_t)i * w, glass_blur + (size_t)clamp(first - 6 + i, 0, h - 1) * w,
                   (size_t)w * 4);
        for (int i = 0; i < 7; i++)
            memcpy(ring + (size_t)(13 + i) * w, glass_blur + (size_t)clamp(last + i, 0, h - 1) * w,
                   (size_t)w * 4);
        return;
    }
    for (int y = first; y < last; y++) {
        const uint32_t *rows[13];
        unsigned head = (unsigned)(y - first) % 13;
        for (unsigned k = 0; k < 13; k++)
            rows[k] = ring + (size_t)((head + k) % 13) * w;
        int left, right;
        glass_source_spans(y, false, &left, &right);
        glass_gaussian_spans(glass_blur + (size_t)y * w, rows, left, right, w);
        int next = y + 7;
        memcpy(ring + (size_t)head * w,
               next < last ? glass_blur + (size_t)next * w : ring + (size_t)(13 + next - last) * w,
               (size_t)w * 4);
    }
    /* Tint follows refraction; constant XRGB tint commutes with opaque sampling. */
    for (int y = first; y < last; y++) {
        int left, right;
        glass_source_spans(y, false, &left, &right);
        raster_blend_span(glass_blur + (size_t)y * w, left, glass_work.color, glass_work.tint);
        if (right < w)
            raster_blend_span(glass_blur + (size_t)y * w + right, w - right, glass_work.color,
                              glass_work.tint);
    }
}
static uint32_t glass_plus(uint32_t c, unsigned light) {
    unsigned r = ((c >> 16) & 255) + light, g = ((c >> 8) & 255) + light, b = (c & 255) + light;
    return ((uint32_t)min(255, (int)r) << 16) | ((uint32_t)min(255, (int)g) << 8) |
           (uint32_t)min(255, (int)b);
}
static void glass_expand_rows(int first, int last) {
    GlassLensMap *m = glass_work.map;
    int w = glass_work.w, h = glass_work.h, bw = glass_work.bw, pad = glass_work.pad;
    for (int yy = first; yy < last; yy++) {
        int ay = yy - glass_work.y;
        uint32_t *dst = canvas + (size_t)yy * sw;
        const uint32_t *source = glass_blur + (size_t)(ay + pad) * bw + pad;
        int left = glass_work.x0, right = glass_work.x1, begin = max(left, glass_work.x + m->band),
            end = min(right, glass_work.x + w - m->band);
        bool middle = ay >= 12 && ay < h - 12 && begin < end;
        if (middle) {
            int cut0 = begin, cut1 = end;
            if (glass_work.opaque && ay >= 68 && ay < h - 15) {
                cut0 = clamp(glass_work.x + 15, begin, end);
                cut1 = clamp(glass_work.x + w - 15, cut0, end);
            } else
                cut0 = end;
            if (cut0 > begin)
                memcpy(dst + begin, source + begin - glass_work.x, (size_t)(cut0 - begin) * 4);
            if (cut1 < end)
                memcpy(dst + cut1, source + cut1 - glass_work.x, (size_t)(end - cut1) * 4);
        }
        for (int part = 0; part < (middle ? 2 : 1); part++) {
            int a = part ? max(left, end) : left,
                b = middle ? (part ? right : min(right, begin)) : right;
            for (int xx = a; xx < b; xx++) {
                int ax = xx - glass_work.x;
                if (glass_work.opaque && ay >= 68 && ay < h - 15 && ax >= 15 && ax < w - 15)
                    continue;
                GlassLensPixel *p = glass_pixel(m, ax, ay);
                if (!p || !p->coverage)
                    continue;
                uint32_t c = liquid_glass_dispersion(glass_blur, bw, bw, glass_work.bh,
                                                     (ax + pad) * 256 + p->dx,
                                                     (ay + pad) * 256 + p->dy, p->cx, p->cy);
                if (p->highlight && glass_work.edge)
                    c = glass_plus(c, (unsigned)p->highlight * glass_work.edge / 255);
                dst[xx] = p->coverage == 255 ? c : compositor_mix(dst[xx], c, p->coverage);
            }
        }
    }
}
static void glass_edge(int x, int y, int w, int h, int radius, int tint, int edge_alpha) {
    if (w <= 0 || h <= 0 || w > MAX_W || h > MAX_H)
        return;
    int r = compositor_radius(w, h, radius), x0 = max(0, x), x1 = min(sw, x + w), y0 = max(0, y),
        y1 = min(sh, y + h);
    if (x0 >= x1 || y0 >= y1) {
        shadow_pending = false;
        return;
    }
    if (shadow_pending && radius > 24) {
        for (int yy = shadow_y0; yy < shadow_y1; yy++)
            for (int xx = shadow_x0; xx < shadow_x1; xx++)
                canvas[(size_t)yy * sw + xx] = shadow_color(canvas[(size_t)yy * sw + xx]);
        shadow_pending = false;
    }
    if (reduced_transparency) {
        rr(x, y, w, h, r, night ? 0x17243d : 0xf3f9ff, 255);
        stroke(x, y, w, h, r, night ? 0x53637a : 0xd8e3ee, edge_alpha);
        shadow_pending = false;
        return;
    }
    bool dispersion = h <= 90 && radius >= 24;
    int pad = dispersion ? 56 : 32, bw = w + pad * 2, bh = h + pad * 2;
    if (glass_gpu_render(x, y, w, h, r, tint, edge_alpha, dispersion))
        return;
    GlassLensMap *m = glass_map(w, h, r, dispersion);
    if (!m || !glass_buffer((size_t)bw * bh * 4)) {
        rr(x, y, w, h, r, night ? 0x17243d : 0xf3f9ff, 255);
        shadow_pending = false;
        notice("内存不足，透明效果暂不可用");
        return;
    }
    glass_work = (GlassWork){.x = x,
                             .y = y,
                             .w = w,
                             .h = h,
                             .bw = bw,
                             .bh = bh,
                             .pad = pad,
                             .r = r,
                             .x0 = x0,
                             .x1 = x1,
                             .y0 = y0,
                             .y1 = y1,
                             .tint = (unsigned)clamp(tint, 0, 255),
                             .edge = (unsigned)clamp(edge_alpha, 0, 255),
                             .color = night ? 0x17243d : 0xf3f9ff,
                             .shadow = shadow_pending,
                             .opaque = glass_opaque_client,
                             .sx0 = shadow_x0,
                             .sx1 = shadow_x1,
                             .sy0 = shadow_y0,
                             .sy1 = shadow_y1,
                             .map = m};
    for (unsigned stage = 0; stage < 3; stage++) {
        uint64_t before = platform_millis();
        compositor_glass_stage(stage);
        glass_stage_ms[stage] += platform_millis() - before;
    }
    shadow_pending = false;
    uint64_t before = platform_millis();
    compositor_glass_parallel((GpuRect){x0, y0, x1 - x0, y1 - y0});
    glass_stage_ms[3] += platform_millis() - before;
}
static void glass(int x, int y, int w, int h, int radius, int tint) {
    glass_edge(x, y, w, h, radius, tint, 80);
}

/* LiquidButton interaction uses the upstream .5/300 spring, 4px growth,
 * bounded tanh translation and Plus highlight; hit geometry stays stable. */
static struct {
    LiquidSpring progress, offset_x, offset_y;
    ClickRegion region;
    int start_x, start_y;
} glass_press;
static void glass_press_begin(void) {
    if (!click_has_region || !click_capture.liquid)
        return;
    glass_press.region = click_capture;
    glass_press.start_x = mx;
    glass_press.start_y = my;
    glass_press.progress = glass_press.offset_x = glass_press.offset_y = (LiquidSpring){0};
    liquid_spring_target(&glass_press.progress, 1, ark_millis());
    dirty = true;
}
static void glass_press_tick(uint64_t now) {
    LiquidSpring *s = &glass_press.progress;
    if (!s->active && s->target == 0 && !glass_press.offset_x.active &&
        !glass_press.offset_y.active)
        return;
    ClickRegion *r = &glass_press.region;
    float target = pointer_was_down && click_pending && drag < 0 && resize_app < 0 &&
                           in(mx, my, r->x, r->y, r->w, r->h)
                       ? 1
                       : 0;
    if (reduced_motion) {
        s->value = s->target = target;
        s->velocity = 0;
        s->active = false;
        return;
    }
    if (target) {
        glass_press.offset_x =
            (LiquidSpring){.value = (float)(mx - glass_press.start_x), .at = now};
        glass_press.offset_y =
            (LiquidSpring){.value = (float)(my - glass_press.start_y), .at = now};
    } else if (s->target) {
        liquid_spring_target(&glass_press.offset_x, 0, now);
        liquid_spring_target(&glass_press.offset_y, 0, now);
    }
    if (target != s->target)
        liquid_spring_target(s, target, now);
    bool changed = liquid_spring_update(s, now);
    changed |= liquid_spring_update(&glass_press.offset_x, now);
    changed |= liquid_spring_update(&glass_press.offset_y, now);
    if (changed || s->active)
        dirty = true;
}
static bool glass_button_transform(int *x, int *y, int *w, int *h, float *progress) {
    ClickRegion *r = &glass_press.region;
    if (r->owner != click_owner || r->x != *x || r->y != *y || r->w != *w || r->h != *h ||
        glass_press.progress.value <= 0)
        return false;
    float p = glass_press.progress.value, dx = glass_press.offset_x.value,
          dy = glass_press.offset_y.value, size = (float)min(*w, *h), large = (float)max(*w, *h),
          length = __builtin_sqrtf(dx * dx + dy * dy), scale = 1 + 4.f / (*h) * p;
    float sx = scale + (length ? 4.f / (*h) * __builtin_fabsf(dx * dx / (length * large)) *
                                     min(*w, *h) / (float)(*h)
                               : 0),
          sy = scale + (length ? 4.f / (*h) * __builtin_fabsf(dy * dy / (length * large)) *
                                     min(*w, *h) / (float)(*w)
                               : 0);
    int width = (int)(*w * sx + .5f), height = (int)(*h * sy + .5f);
    *x -= (width - *w) / 2;
    *y -= (height - *h) / 2;
    *x += (int)(size * liquid_glass_tanh(.05f * dx / size));
    *y += (int)(size * liquid_glass_tanh(.05f * dy / size));
    *w = width;
    *h = height;
    *progress = p;
    return true;
}
static void glass_button_highlight(int x, int y, int w, int h, float progress) {
    float radius = min(w, h) * 1.5f,
          px = clamp(glass_press.start_x + (int)glass_press.offset_x.value, x, x + w) - x,
          py = clamp(glass_press.start_y + (int)glass_press.offset_y.value, y, y + h) - y;
    for (int yy = max(0, y); yy < min(sh, y + h); yy++)
        for (int xx = max(0, x); xx < min(sw, x + w); xx++) {
            unsigned cover = rounded_coverage(xx - x, yy - y, w, h, h / 2);
            if (!cover)
                continue;
            float dx = xx - x - px, dy = yy - y - py,
                  t = (radius - __builtin_sqrtf(dx * dx + dy * dy)) / (radius * .5f);
            if (t < 0)
                t = 0;
            if (t > 1)
                t = 1;
            t = t * t * (3 - 2 * t);
            canvas[(size_t)yy * sw + xx] =
                glass_plus(canvas[(size_t)yy * sw + xx],
                           (unsigned)((.08f + .15f * t) * 255 * progress * cover / 255));
        }
}
