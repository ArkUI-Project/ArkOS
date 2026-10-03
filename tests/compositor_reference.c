/* Full-pixel shadow and seven-tap material reference from ArkOS 0.11.0.
 * Sampling dimensions follow the 0.12 large-surface specification. */
static void reference_shadow(int x, int y, int w, int h) {
    /* Every layer fully covers this common interior. Compose its eight exact
     * integer blends through channel tables, retaining all corner/edge layers. */
    int cx0 = max(0, x + 24), cx1 = min(sw, x + w - 24), cy0 = max(0, y + 6),
        cy1 = min(sh, y + h + 8);
    if (w <= 48 || h <= 48 || cx0 >= cx1 || cy0 >= cy1) {
        for (int i = 16; i >= 2; i -= 2)
            rr(x - i / 2, y + 7 - i / 2, w + i, h + i, 24 + i / 2, 0x071c3c, 7);
        return;
    }
    static uint8_t channels[3][256];
    static bool ready;
    if (!ready) {
        const unsigned color[3] = {7, 28, 60};
        for (unsigned channel = 0; channel < 3; channel++)
            for (unsigned value = 0; value < 256; value++) {
                unsigned result = value;
                for (unsigned layer = 0; layer < 8; layer++)
                    result = (result * 248 + color[channel] * 7) / 255;
                channels[channel][value] = (uint8_t)result;
            }
        ready = true;
    }
    for (int i = 16; i >= 2; i -= 2) {
        int ax = x - i / 2, ay = y + 7 - i / 2, aw = w + i, ah = h + i,
            r = compositor_radius(aw, ah, 24 + i / 2);
        int x0 = max(0, ax), x1 = min(sw, ax + aw), y0 = max(0, ay), y1 = min(sh, ay + ah);
        compositor_round_region(ax, ay, aw, ah, r, 0x071c3c, 7, x0, y0, x1, min(y1, cy0));
        compositor_round_region(ax, ay, aw, ah, r, 0x071c3c, 7, x0, max(y0, cy1), x1, y1);
        compositor_round_region(ax, ay, aw, ah, r, 0x071c3c, 7, x0, max(y0, cy0), min(x1, cx0),
                                min(y1, cy1));
        compositor_round_region(ax, ay, aw, ah, r, 0x071c3c, 7, max(x0, cx1), max(y0, cy0), x1,
                                min(y1, cy1));
    }
    for (int yy = cy0; yy < cy1; yy++)
        for (int xx = cx0; xx < cx1; xx++) {
            uint32_t c = canvas[yy * sw + xx];
            canvas[yy * sw + xx] = ((uint32_t)channels[0][(c >> 16) & 255] << 16) |
                                   ((uint32_t)channels[1][(c >> 8) & 255] << 8) |
                                   channels[2][c & 255];
        }
}
static void reference_glass_edge(int x, int y, int w, int h, int radius, int tint, int edge_alpha) {
    if (w <= 0 || h <= 0)
        return;
    if (reduced_transparency) {
        rr(x, y, w, h, radius, night ? 0x17243d : 0xf3f9ff, 255);
        stroke(x, y, w, h, radius, night ? 0x53637a : 0xd8e3ee, edge_alpha);
        return;
    }
    int sample = max(4, max((w + 499) / 500, (h + 319) / 320));
    int bw = (w + sample - 1) / sample, bh = (h + sample - 1) / sample;
    if (bw > 500 || bh > 320)
        return;
    int x0 = max(0, x), x1 = min(sw, x + w), y0 = max(0, y), y1 = min(sh, y + h);
    if (x0 >= x1 || y0 >= y1)
        return;
    for (int yy = 0; yy < bh; yy++) {
        const uint32_t *row = canvas + clamp(y + yy * sample + sample / 2, 0, sh - 1) * sw;
        for (int xx = 0; xx < bw; xx++)
            blur_a[yy * bw + xx] = row[clamp(x + xx * sample + sample / 2, 0, sw - 1)];
    }
    for (int yy = 0; yy < bh; yy++)
        compositor_blur_line(blur_a + yy * bw, blur_b + yy * bw, bw, 1, false, 0, 0);
    uint32_t color = night ? 0x17243d : 0xf3f9ff;
    for (int xx = 0; xx < bw; xx++)
        compositor_blur_line(blur_b + xx, blur_a + xx, bh, bw, true, color,
                             (unsigned)clamp(tint, 0, 255));
    /* Visible columns are bounded by the screen, even for a clipped surface. */
    int sample_x[MAX_W];
    for (int xx = x0; xx < x1; xx++) {
        int ax = xx - x, rx = ax < 9 ? ax + 5 : ax > w - 10 ? ax - 5 : ax;
        sample_x[xx - x0] = clamp(rx / sample, 0, bw - 1);
    }
    int r = compositor_radius(w, h, radius);
    for (int yy = y0; yy < y1; yy++) {
        int ay = yy - y, ry = ay < 9 ? ay + 5 : ay > h - 10 ? ay - 5 : ay;
        const uint32_t *source = blur_a + clamp(ry / sample, 0, bh - 1) * bw;
        uint32_t *row = canvas + yy * sw;
        int left = x0, right = x1;
        if (r && (ay < r || ay >= h - r)) {
            left = min(x1, max(x0, x + r));
            right = max(left, min(x1, x + w - r));
        }
        for (int xx = x0; xx < left; xx++) {
            unsigned coverage = rounded_coverage(xx - x, ay, w, h, r);
            if (coverage)
                row[xx] = compositor_mix(row[xx], source[sample_x[xx - x0]], coverage);
        }
        for (int xx = left; xx < right; xx++)
            row[xx] = source[sample_x[xx - x0]];
        for (int xx = right; xx < x1; xx++) {
            unsigned coverage = rounded_coverage(xx - x, ay, w, h, r);
            if (coverage)
                row[xx] = compositor_mix(row[xx], source[sample_x[xx - x0]], coverage);
        }
    }
    stroke(x, y, w, h, radius, 0xffffff, edge_alpha);
    stroke(x + 1, y + 1, w - 2, h - 2, radius - 1, 0xffffff, edge_alpha / 3);
}
