#ifndef AXQUARTZ_H
#define AXQUARTZ_H

/*
 * ============================================================
 * AxQuartz -- user-space 2D drawing layer ("Quartz 2D analog")
 * ============================================================
 *
 * AxQuartz is the APPLICATION-SIDE counterpart of the kernel's
 * lib/gfxlib.c.  gfxlib renders into the kernel backbuffer for
 * the compositor (menubar, dock, titlebars); AxQuartz renders
 * into an application window canvas obtained from ax_surface().
 *
 * Every primitive is a byte-for-byte visual match of its
 * gfxlib sibling (same gradients, same anti-aliasing, same
 * gloss), so app content looks exactly like the Lion chrome
 * the compositor draws around it.
 *
 * Design rules inherited from gfxlib.c:
 *   * colours are 0xAARRGGBB (AX_ARGB / AX_RGB from libax.h);
 *   * *_alpha primitives BLEND with the destination, opaque
 *     primitives replace the pixels;
 *   * gfx_hline/pixel-style raw stores are alpha-blind -- use
 *     the *_alpha variants for any translucent colour;
 *   * everything is integer-only (no FPU/SSE), apps build with
 *     -mno-sse.
 *
 * Target canvas: the libax globals (g_canvas, g_cw, g_ch)
 * bound by the app (bind() / ax_canvas_dims()), exactly like
 * ax_px / ax_rect / ax_text.  All primitives clip themselves
 * to the canvas, so drawing near the edges is safe.
 *
 * Usage:
 *     #include "../lib/libax.h"
 *     #include "../lib/axquartz.h"
 * ============================================================
 */

#include "libax.h"

/* ============================================================
 * Colour math
 * ============================================================ */

static inline uint32_t aq_blend(uint32_t dst, uint32_t src)
{
    uint32_t a = (src >> 24) & 0xFF;
    if (a == 0)   return dst;
    if (a == 255) return src | 0xFF000000;

    uint32_t ia = 255 - a;
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8)  & 0xFF;
    uint32_t sb = (src)       & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8)  & 0xFF;
    uint32_t db = (dst)       & 0xFF;
    uint32_t rr = (sr * a + dr * ia) / 255;
    uint32_t rg = (sg * a + dg * ia) / 255;
    uint32_t rb = (sb * a + db * ia) / 255;
    return 0xFF000000 | (rr << 16) | (rg << 8) | rb;
}

/*
 * Interpolate ALL four channels (alpha included): translucent
 * gradients rely on the alpha byte surviving the lerp.
 * t = 0..256.
 */
static inline uint32_t aq_lerp(uint32_t a, uint32_t b, int t)
{
    if (t < 0)   t = 0;
    if (t > 256) t = 256;
    int it = 256 - t;
    uint32_t aa = (a >> 24) & 0xFF, ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t ba = (b >> 24) & 0xFF, br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint32_t ra = (aa * it + ba * t) >> 8;
    uint32_t rr = (ar * it + br * t) >> 8;
    uint32_t rg = (ag * it + bg * t) >> 8;
    uint32_t rb = (ab * it + bb * t) >> 8;
    return (ra << 24) | (rr << 16) | (rg << 8) | rb;
}

/* Lighten towards white (t = 0..256), keeps alpha. */
static inline uint32_t aq_tint(uint32_t c, int t)
{
    return aq_lerp(c, AX_RGB(255, 255, 255), t);
}

/* Darken towards black (t = 0..256), keeps alpha. */
static inline uint32_t aq_shade(uint32_t c, int t)
{
    return aq_lerp(c, AX_RGB(0, 0, 0), t);
}

/* Exact integer sqrt, bit-by-bit (division-free, no FPU). */
static inline int aq_isqrt(int v)
{
    if (v <= 0) return 0;
    int res = 0;
    int bit = 1 << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

/* ============================================================
 * Pixel + rect primitives (canvas-clipped)
 * ============================================================ */

static inline void aq_px(int x, int y, uint32_t color)
{
    if (x < 0 || y < 0 || x >= g_cw || y >= g_ch) return;
    g_canvas[y * g_cw + x] = color;
}

/* Alpha-blended pixel. */
static inline void aq_px_alpha(int x, int y, uint32_t argb)
{
    if (x < 0 || y < 0 || x >= g_cw || y >= g_ch) return;
    uint32_t *p = &g_canvas[y * g_cw + x];
    *p = aq_blend(*p, argb);
}

static inline uint32_t aq_get_px(int x, int y)
{
    if (x < 0 || y < 0 || x >= g_cw || y >= g_ch) return 0;
    return g_canvas[y * g_cw + x];
}

static inline void aq_clear(uint32_t color)
{
    for (int i = 0; i < g_cw * g_ch; i++) g_canvas[i] = color;
}

static inline void aq_fill(int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_cw) w = g_cw - x;
    if (y + h > g_ch) h = g_ch - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        uint32_t *row = &g_canvas[(y + j) * g_cw + x];
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

static inline void aq_fill_alpha(int x, int y, int w, int h, uint32_t argb)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_cw) w = g_cw - x;
    if (y + h > g_ch) h = g_ch - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        uint32_t *row = &g_canvas[(y + j) * g_cw + x];
        for (int i = 0; i < w; i++) row[i] = aq_blend(row[i], argb);
    }
}

static inline void aq_hline(int x, int y, int w, uint32_t color)
{
    aq_fill(x, y, w, 1, color);
}

static inline void aq_vline(int x, int y, int h, uint32_t color)
{
    aq_fill(x, y, 1, h, color);
}

static inline void aq_rect_outline(int x, int y, int w, int h, uint32_t color)
{
    aq_hline(x, y, w, color);
    aq_hline(x, y + h - 1, w, color);
    aq_vline(x, y, h, color);
    aq_vline(x + w - 1, y, h, color);
}

/* Bresenham line, opaque. */
static inline void aq_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    int err = (adx > ady ? adx : -ady) / 2;
    for (;;) {
        aq_px(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err;
        if (e2 > -adx) { err -= ady; x0 += sx; }
        if (e2 <  ady) { err += adx; y0 += sy; }
    }
}

/* ============================================================
 * Rounded rectangles
 * ============================================================ */

static inline int aq_in_rounded(int px, int py, int w, int h, int r)
{
    if (px >= r && px < w - r) return 1;
    if (py >= r && py < h - r) return 1;
    int cx, cy;
    if (px < r)      cx = r;       else cx = w - 1 - r;
    if (py < r)      cy = r;       else cy = h - 1 - r;
    int dx = px - cx;
    int dy = py - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

#define AQ_RCLAMP(r, w, h) \
    do { if ((r) * 2 > (w)) (r) = (w) / 2; if ((r) * 2 > (h)) (r) = (h) / 2; } while (0)

static inline void aq_rounded_fill(int x, int y, int w, int h, int radius,
                                   uint32_t color)
{
    AQ_RCLAMP(radius, w, h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (aq_in_rounded(i, j, w, h, radius))
                aq_px(x + i, y + j, color);
}

static inline void aq_rounded_fill_alpha(int x, int y, int w, int h, int radius,
                                         uint32_t argb)
{
    AQ_RCLAMP(radius, w, h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (aq_in_rounded(i, j, w, h, radius))
                aq_px_alpha(x + i, y + j, argb);
}

/*
 * 1px rounded outline, alpha-blended (pass translucent rim
 * colours safely).
 */
static inline void aq_rounded_outline(int x, int y, int w, int h, int radius,
                                      uint32_t color)
{
    AQ_RCLAMP(radius, w, h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int inside = aq_in_rounded(i, j, w, h, radius);
            int inner  = (i >= 1 && j >= 1 && i < w - 1 && j < h - 1)
                         ? aq_in_rounded(i - 1, j, w, h, radius) &&
                           aq_in_rounded(i + 1, j, w, h, radius) &&
                           aq_in_rounded(i, j - 1, w, h, radius) &&
                           aq_in_rounded(i, j + 1, w, h, radius)
                         : 0;
            if (inside && !inner)
                aq_px_alpha(x + i, y + j, color);
        }
}

/* ============================================================
 * Gradients
 * ============================================================ */

static inline void aq_vgradient(int x, int y, int w, int h,
                                uint32_t top, uint32_t bottom)
{
    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 256) / (h - 1) : 0;
        aq_hline(x, y + j, w, aq_lerp(top, bottom, t));
    }
}

static inline void aq_hgradient(int x, int y, int w, int h,
                                uint32_t left, uint32_t right)
{
    for (int i = 0; i < w; i++) {
        int t = (w > 1) ? (i * 256) / (w - 1) : 0;
        aq_vline(x + i, y, h, aq_lerp(left, right, t));
    }
}

/* Vertical gradient that BLENDS with the canvas (Lion glass). */
static inline void aq_vgradient_alpha(int x, int y, int w, int h,
                                      uint32_t top_argb, uint32_t bot_argb)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_cw) w = g_cw - x;
    if (y + h > g_ch) h = g_ch - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 256) / (h - 1) : 0;
        uint32_t c = aq_lerp(top_argb, bot_argb, t);
        uint32_t *row = &g_canvas[(y + j) * g_cw + x];
        for (int i = 0; i < w; i++) row[i] = aq_blend(row[i], c);
    }
}

static inline void aq_rounded_vgradient(int x, int y, int w, int h, int radius,
                                        uint32_t top, uint32_t bottom)
{
    AQ_RCLAMP(radius, w, h);
    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 256) / (h - 1) : 0;
        uint32_t c = aq_lerp(top, bottom, t);
        for (int i = 0; i < w; i++)
            if (aq_in_rounded(i, j, w, h, radius))
                aq_px(x + i, y + j, c);
    }
}

static inline void aq_rounded_vgradient_alpha(int x, int y, int w, int h,
                                              int radius,
                                              uint32_t top_argb,
                                              uint32_t bot_argb)
{
    AQ_RCLAMP(radius, w, h);
    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 256) / (h - 1) : 0;
        uint32_t c = aq_lerp(top_argb, bot_argb, t);
        for (int i = 0; i < w; i++)
            if (aq_in_rounded(i, j, w, h, radius))
                aq_px_alpha(x + i, y + j, c);
    }
}

/* ============================================================
 * Aqua circles, gloss, shadows, glyphs
 * ============================================================ */

/*
 * Aqua dot (traffic lights): exact circle with 1px AA skirt,
 * vertical body gradient + thin dark rim, no gloss dome.
 */
static inline void aq_gloss_circle(int cx, int cy, int r,
                                   uint32_t top, uint32_t bottom,
                                   uint32_t rim)
{
    int rad16 = r * 16;
    int rad2  = (r + 2) * (r + 2);

    for (int dy = -r - 1; dy <= r + 1; dy++) {
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            if (dx * dx + dy * dy > rad2) continue;

            int dist16 = aq_isqrt((dx * dx + dy * dy) << 8);

            int cov = (rad16 + 8) - dist16;
            if (cov <= 0) continue;
            cov *= 16;
            if (cov > 255) cov = 255;

            uint32_t col;
            if (dist16 >= rad16 - 20)
                col = rim;
            else {
                int t = (r > 0) ? ((dy + r) * 256) / (2 * r) : 0;
                col = aq_lerp(top, bottom, t);
            }

            aq_px_alpha(cx + dx, cy + dy,
                        ((uint32_t)cov << 24) | (col & 0x00FFFFFF));
        }
    }
}

/* Small anti-aliased flat disc (alpha in the top byte). */
static inline void aq_circle_aa(int cx, int cy, int r, uint32_t argb)
{
    uint32_t base = argb & 0x00FFFFFF;
    uint32_t a    = (argb >> 24) & 0xFF;
    if (!a || r <= 0) return;

    int rad16 = r * 16;
    int rad2  = (r + 2) * (r + 2);

    for (int dy = -r - 1; dy <= r + 1; dy++) {
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            if (dx * dx + dy * dy > rad2) continue;
            int dist16 = aq_isqrt((dx * dx + dy * dy) << 8);
            int cov = (rad16 + 8) - dist16;
            if (cov <= 0) continue;
            cov *= 16;
            if (cov > 255) cov = 255;
            cov = (cov * a) / 255;

            aq_px_alpha(cx + dx, cy + dy,
                        ((uint32_t)cov << 24) | base);
        }
    }
}

/* Layered translucent rounded-rect drop shadow (Lion window cue). */
static inline void aq_soft_shadow(int x, int y, int w, int h,
                                  int radius, int layers)
{
    static const uint8_t alphas[] = { 0x1E, 0x16, 0x10, 0x0A, 0x06 };
    if (layers > 5) layers = 5;
    for (int i = layers - 1; i >= 0; i--) {
        int grow = i * 2;
        uint32_t a = alphas[i];
        aq_rounded_fill_alpha(x - grow, y - grow + 3,
                              w + grow * 2, h + grow * 2,
                              radius + grow, ((uint32_t)a << 24));
    }
}

/* Top gloss dome for rectangular Aqua elements (buttons/icons). */
static inline void aq_gloss_top(int x, int y, int w, int h, int radius,
                                int depth)
{
    if (depth > h) depth = h;
    if (depth < 1) depth = 1;
    for (int j = 0; j < depth; j++) {
        int alpha = 0x55 - (j * 0x55) / depth;
        if (alpha <= 0) continue;
        uint32_t c = ((uint32_t)alpha << 24) | 0x00FFFFFF;
        int x0 = 0, x1 = w;
        for (int i = 0; i < w; i++)
            if (aq_in_rounded(i, j, w, h, radius)) { x0 = i; break; }
        for (int i = w - 1; i >= 0; i--)
            if (aq_in_rounded(i, j, w, h, radius)) { x1 = i + 1; break; }
        if (x1 > x0)
            aq_fill_alpha(x + x0, y + j, x1 - x0, 1, c);
    }
}

/*
 * Line glyphs (traffic-light marks and small icons).
 * kind: 0 = close (x), 1 = minimize (-), 2 = zoom (+).
 */
static inline void aq_glyph(int kind, int cx, int cy, uint32_t color)
{
    int d = 3;
    switch (kind) {
    case 0:
        aq_line(cx - d, cy - d, cx + d, cy + d, color);
        aq_line(cx - d + 1, cy - d, cx + d + 1, cy + d, color);
        aq_line(cx + d, cy - d, cx - d, cy + d, color);
        aq_line(cx + d, cy - d + 1, cx - d, cy + d + 1, color);
        break;
    case 1:
        aq_hline(cx - d, cy - 1, d * 2 + 1, color);
        aq_hline(cx - d, cy,     d * 2 + 1, color);
        break;
    case 2:
        aq_hline(cx - d, cy - 1, d * 2 + 1, color);
        aq_hline(cx - d, cy,     d * 2 + 1, color);
        aq_vline(cx - 1, cy - d, d * 2 + 1, color);
        aq_vline(cx,     cy - d, d * 2 + 1, color);
        break;
    default:
        break;
    }
}

/* ============================================================
 * Text (8x8 bitmap font from libax)
 * ============================================================ */

static inline void aq_text(const char *s, int x, int y, uint32_t color)
{
    ax_text(s, x, y, color);
}

/*
 * Vertical offset that centers the active text font (TrueType 13px,
 * fallback font8x8) inside a fixed-height row/toolbar: aq_text_y(24)
 * centers a toolbar label, aq_text_y(20) a menu row.
 */
static inline int aq_text_y(int row_h)
{
    return (row_h - (axttf_ready() ? AXTTF_UI_SIZE : 8) + 1) / 2;
}

/* Faux-bold: 1px double draw, the same trick the menubar uses. */
static inline void aq_text_bold(const char *s, int x, int y, uint32_t color)
{
    ax_text(s, x,     y, color);
    ax_text(s, x + 1, y, color);
}

/*
 * Scaled text.  С TrueType просто растеризуем больший кегль
 * (качество лучше, чем апскейл битмапа); без шрифта -- старый
 * путь с повторением строк font8x8.
 */
static inline void aq_text_scaled(const char *s, int x, int y,
                                  uint32_t color, int scale)
{
    if (scale <= 1) { ax_text(s, x, y, color); return; }

    if (axttf_ready()) {
        const int size = AXTTF_UI_SIZE * scale;
        const int asc  = axttf_ascent(size, 0);
        int pen = x, base = y + asc, ox = x;
        unsigned prev = 0;

        while (*s) {
            unsigned cp;
            const axttf_glyph *g;

            if (*s == '\n') {
                y += AXTTF_UI_LINE * scale;
                base = y + asc;
                pen = ox;
                s++;
                prev = 0;
                continue;
            }

            cp = ax_utf8_next(&s);

            g = axttf_get_glyph(cp, size);
            if (!g)
                continue;

            if (prev)
                pen += axttf_kern(prev, cp, size);

            ax_glyph_blit(g, pen + g->xoff, base + g->yoff, color);
            pen += g->adv;
            prev = cp;
        }

        return;
    }

    {
        int ox = x;

        while (*s) {
            if (*s == '\n') { y += 8 * scale; x = ox; s++; continue; }
            /* draw the glyph row-by-row, repeating each row */
            unsigned int cp = ax_utf8_next(&s);
            const unsigned char *g = ax_font_glyph(cp);
            for (int cy = 0; cy < 8; cy++)
                for (int rep = 0; rep < scale; rep++) {
                    int yy = y + cy * scale + rep;
                    for (int cx = 0; cx < 8; cx++)
                        if (g[cy] & (1 << cx))
                            aq_px(x + cx, yy, color);
                }
            x += 8;
        }
    }
}

static inline int aq_text_width(const char *s)
{
    return ax_text_width(s);
}

/* ============================================================
 * Blits
 * ============================================================ */

/* Copy a canvas-sized buffer into the canvas (full overlap). */
static inline void aq_blit(const uint32_t *src, int sw, int sh, int dx, int dy)
{
    for (int j = 0; j < sh; j++)
        for (int i = 0; i < sw; i++)
            aq_px(dx + i, dy + j, src[j * sw + i]);
}

/* ============================================================
 * Convenience composites used across the whole Aqua UI kit
 * ============================================================ */

/* 1px hairline separator (blended, works over gradients). */
static inline void aq_hairline(int x, int y, int w, uint32_t argb)
{
    aq_fill_alpha(x, y, w, 1, argb);
}

#endif /* AXQUARTZ_H */
