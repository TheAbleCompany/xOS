#ifndef AXAQUA_H
#define AXAQUA_H

/*
 * ============================================================
 * AquaKit -- the unified application UI kit ("Cocoa analog")
 * ============================================================
 *
 * One design language for every ArtyomX64 application, styled
 * after Mac OS X 10.7 Lion Aqua and rendered with AxQuartz
 * (lib/axquartz.h) into the window canvas.
 *
 * The tokens below mirror the compositor palette from
 * kernel/axshell_internal.h (LION_*), so an app window drawn
 * with this kit is a seamless continuation of the chrome the
 * shell paints around it: same light brushed-metal toolbar,
 * same hairlines, same accent blue, same traffic-light Aqua.
 *
 * Layer cake:
 *
 *     libax.h      syscalls + canvas binding  (the "DPS layer")
 *     axquartz.h   2D primitives              (the "Quartz 2D")
 *     axaqua.h     widgets + design tokens    (the "Cocoa")
 *
 * Everything is header-only, freestanding C, integer-only.
 *
 * ============================================================
 */

#include "axquartz.h"

/* ============================================================
 * Design tokens (mirrors of the compositor LION_* palette)
 * ============================================================ */

/* --- window body --- */
#define AQ_WIN_BG_TOP     AX_ARGB(0xFF, 0xF2, 0xF2, 0xF6)
#define AQ_WIN_BG_BOT     AX_ARGB(0xFF, 0xE2, 0xE2, 0xEA)
#define AQ_CONTENT_BG     AX_RGB(255, 255, 255)   /* paper / list area */
#define AQ_CONTENT_EDGE   AX_ARGB(0x5A, 0x60, 0x60, 0x70)
#define AQ_CONTENT_EDGE_L AX_ARGB(0x80, 255, 255, 255)

/* --- text --- */
#define AQ_TEXT           AX_RGB(45, 45, 52)
#define AQ_TEXT_SECOND    AX_RGB(120, 120, 130)
#define AQ_TEXT_TERT      AX_RGB(160, 160, 170)
#define AQ_TEXT_ON_ACCENT AX_RGB(255, 255, 255)
#define AQ_TEXT_DISABLED  AX_RGB(170, 170, 178)

/* --- toolbar (brushed metal strip under the titlebar) --- */
#define AQ_TOOL_TOP       AX_RGB(0xF8, 0xF8, 0xFA)
#define AQ_TOOL_BOT       AX_RGB(0xD8, 0xD8, 0xE0)
#define AQ_TOOL_TEXT      AX_RGB(60, 60, 68)
#define AQ_TOOL_HAIR      AX_ARGB(0x50, 90, 90, 100)
#define AQ_TOOL_SHINE     AX_ARGB(0x70, 255, 255, 255)

/* --- accent (matches the shell default accent) --- */
#define AQ_ACCENT         AX_RGB(10, 132, 255)
#define AQ_ACCENT_TOP     AX_RGB(0x59, 0x9E, 0xFF)
#define AQ_ACCENT_BOT     AX_RGB(0x0A, 0x7A, 0xE8)
#define AQ_ACCENT_RIM     AX_RGB(0x05, 0x54, 0xA8)
#define AQ_ACCENT_DIM     AX_ARGB(0x60, 10, 132, 255)

/* --- Aqua push buttons (AQ_BTN_MID reserved for future themes) --- */
#define AQ_BTN_MID        AX_RGB(0xF4, 0xF4, 0xF6)
#define AQ_BTN_TOP        AX_RGB(255, 255, 255)
#define AQ_BTN_BOT        AX_RGB(0xD6, 0xD6, 0xDE)
#define AQ_BTN_RIM        AX_ARGB(0x9A, 0x62, 0x62, 0x70)
#define AQ_BTN_PRESS_TOP  AX_RGB(0xD8, 0xD8, 0xE0)
#define AQ_BTN_PRESS_BOT  AX_RGB(0xBC, 0xBC, 0xC8)
#define AQ_BTN_SHADOW     AX_ARGB(0x30, 0, 0, 0)
#define AQ_BTN_H          22      /* standard push button height */

/* --- text fields --- */
#define AQ_FIELD_BG       AX_RGB(255, 255, 255)
#define AQ_FIELD_RIM      AX_ARGB(0x8C, 0x74, 0x74, 0x80)
#define AQ_FIELD_INNER    AX_ARGB(0x28, 0, 0, 0)
#define AQ_FIELD_H        20

/* --- status bar --- */
#define AQ_STATUS_H       20
#define AQ_STATUS_TOP     AX_ARGB(0xFF, 0xE8, 0xE8, 0xEE)
#define AQ_STATUS_BOT     AX_ARGB(0xFF, 0xD8, 0xD8, 0xE0)

/* --- sidebar (Lion source list) --- */
#define AQ_SIDEBAR_BG_TOP AX_RGB(0xE8, 0xEA, 0xF0)
#define AQ_SIDEBAR_BG_BOT AX_RGB(0xD4, 0xD8, 0xE2)
#define AQ_SIDEBAR_TEXT   AX_RGB(40, 40, 48)
#define AQ_SIDEBAR_DIM    AX_RGB(120, 122, 132)
#define AQ_SEL_TOP        AQ_ACCENT_TOP
#define AQ_SEL_BOT        AQ_ACCENT_BOT
#define AQ_SEL_RIM        AQ_ACCENT_RIM

/* --- scrollbars (Lion overlay style) --- */
#define AQ_SCROLL_THUMB   AX_ARGB(0xB4, 0x8E, 0x8E, 0x98)
#define AQ_SCROLL_THUMB_H AX_ARGB(0xB4, 0x6E, 0x6E, 0x78)
#define AQ_SCROLL_TRACK   AX_ARGB(0x30, 0, 0, 0)
#define AQ_SCROLL_W       12

/* --- terminal palette (unified chrome for dark content) --- */
#define AQ_TERM_BG        AX_ARGB(0xFF, 0x10, 0x14, 0x1A)
#define AQ_TERM_TEXT      AX_RGB(200, 220, 200)
#define AQ_TERM_PROMPT    AX_RGB(0x64, 0xC8, 0xFF)
#define AQ_TERM_CURSOR    AX_RGB(255, 180, 80)

/* ============================================================
 * Window scaffolding
 * ============================================================ */

/*
 * aq_window_bg -- the standard Lion window interior: light
 * vertical gradient.  Call first in every render pass.
 */
static inline void aq_window_bg(void)
{
    aq_vgradient(0, 0, g_cw, g_ch, AQ_WIN_BG_TOP, AQ_WIN_BG_BOT);
}

/*
 * aq_toolbar -- brushed-metal strip under the titlebar with the
 * same hairline the shell draws under its own titlebars.
 */
static inline void aq_toolbar(int x, int y, int w, int h)
{
    aq_vgradient(x, y, w, h, AQ_TOOL_TOP, AQ_TOOL_BOT);
    aq_fill_alpha(x, y, w, 1, AQ_TOOL_SHINE);
    aq_fill_alpha(x, y + h - 1, w, 1, AQ_TOOL_HAIR);
}

/* Divider between the toolbar and the content area. */
static inline void aq_content_edge(int x, int y, int w, int h)
{
    aq_fill_alpha(x, y, w, 1, AQ_CONTENT_EDGE);
    aq_fill_alpha(x, y + 1, w, 1, AQ_CONTENT_EDGE_L);
    aq_fill(x, y + 2, w, h - 2, AQ_CONTENT_BG);
}

/*
 * aq_statusbar -- light metal footer with a top hairline.
 */
static inline void aq_statusbar(int y, int h)
{
    aq_vgradient(0, y, g_cw, h, AQ_STATUS_TOP, AQ_STATUS_BOT);
    aq_fill_alpha(0, y, g_cw, 1, AQ_TOOL_HAIR);
    aq_fill_alpha(0, y + 1, g_cw, 1, AX_ARGB(0x60, 255, 255, 255));
}

/* ============================================================
 * Aqua push buttons
 * ============================================================ */

typedef enum {
    AQ_BTN_NORMAL  = 0,   /* grey Aqua push button           */
    AQ_BTN_DEFAULT = 1,   /* blue pulsing "return" button    */
    AQ_BTN_DISCLOS = 2    /* flat gradientless (toolbars)    */
} aq_btn_kind;

typedef enum {
    AQ_BTN_UP      = 0,
    AQ_BTN_HOVER   = 1,
    AQ_BTN_DOWN    = 2,
    AQ_BTN_DISABLED= 3
} aq_btn_state;

/*
 * aq_button -- rounded Aqua push button: vertical gradient
 * body, top gloss dome, 1px rim, soft shadow, centered label.
 */
static inline void aq_button(int x, int y, int w, int h,
                             const char *label,
                             aq_btn_kind kind, aq_btn_state state)
{
    int r = h / 2 > 6 ? 6 : h / 2;

    if (kind == AQ_BTN_DEFAULT) {
        uint32_t top = AQ_BTN_UP, bot = AQ_BTN_UP, rim = AQ_BTN_RIM;

        if (state == AQ_BTN_DOWN)        { top = AQ_BTN_PRESS_TOP; bot = AQ_BTN_PRESS_BOT; }
        else if (state == AQ_BTN_DISABLED){ top = AQ_BTN_TOP; bot = AQ_BTN_BOT; }
        else                             { top = AQ_ACCENT_TOP;    bot = AQ_ACCENT_BOT;    }
        rim = (state == AQ_BTN_DISABLED) ? AQ_BTN_RIM : AQ_ACCENT_RIM;

        aq_soft_shadow(x, y, w, h, r, 2);
        aq_rounded_vgradient(x, y, w, h, r, top, bot);
        aq_gloss_top(x, y, w, h, r, h / 2);
        aq_rounded_outline(x, y, w, h, r, rim);

        uint32_t tc = (state == AQ_BTN_DISABLED)
                    ? AQ_TEXT_DISABLED
                    : AQ_TEXT_ON_ACCENT;
        if (label)
            aq_text(label, x + w / 2 - aq_text_width(label) / 2,
                    y + h / 2 - 4, tc);
        return;
    }

    /* grey push button */
    uint32_t top = AQ_BTN_TOP, bot = AQ_BTN_BOT;
    uint32_t rim = AQ_BTN_RIM;
    uint32_t tc  = AQ_TEXT;

    if (state == AQ_BTN_DOWN) {
        top = AQ_BTN_PRESS_TOP; bot = AQ_BTN_PRESS_BOT;
    } else if (state == AQ_BTN_DISABLED) {
        tc = AQ_TEXT_DISABLED;
    }

    aq_soft_shadow(x, y, w, h, r, 2);
    aq_rounded_vgradient(x, y, w, h, r, top, bot);
    if (state != AQ_BTN_DOWN)
        aq_gloss_top(x, y, w, h, r, h / 2);
    aq_rounded_outline(x, y, w, h, r, rim);

    /* Lion hover cue: faint accent ring around the button */
    if (state == AQ_BTN_HOVER)
        aq_rounded_outline(x - 1, y - 1, w + 2, h + 2, r + 1,
                           AX_ARGB(0x66, 10, 132, 255));

    if (label)
        aq_text(label, x + w / 2 - aq_text_width(label) / 2,
                y + h / 2 - 4, tc);
}

/* Hit-test helper (same rect convention as aq_button). */
static inline int aq_button_hit(int x, int y, int w, int h, int px, int py)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ============================================================
 * Text fields
 * ============================================================ */

/*
 * aq_field -- inset white Lion text field: white body, inner
 * shadow, 1px rim; optional cursor bar at `cursor` chars.
 * focused == 1 draws the blue focus ring.
 */
static inline void aq_field(int x, int y, int w, int h,
                            const char *text, int cursor, int focused)
{
    int r = 4;
    aq_rounded_fill(x, y, w, h, r, AQ_FIELD_BG);
    aq_rounded_outline(x, y, w, h, r, AQ_FIELD_RIM);
    /* inner top shadow */
    aq_fill_alpha(x + 2, y + 1, w - 4, 1, AQ_FIELD_INNER);

    if (focused) {
        aq_rounded_outline(x - 1, y - 1, w + 2, h + 2, r + 1,
                           AX_ARGB(0x70, 10, 132, 255));
    }

    if (text)
        aq_text(text, x + 5, y + h / 2 - 4, AQ_TEXT);

    if (cursor >= 0) {
        int cx = x + 5 + cursor * 8;
        if (cx < x + w - 6)
            aq_fill(cx, y + 3, 2, h - 6, AQ_ACCENT);
    }
}

/* ============================================================
 * Sidebar / source list (Lion Finder look)
 * ============================================================ */

static inline void aq_sidebar(int x, int y, int w, int h)
{
    aq_vgradient(x, y, w, h, AQ_SIDEBAR_BG_TOP, AQ_SIDEBAR_BG_BOT);
    aq_fill_alpha(x + w - 1, y, 1, h, AQ_CONTENT_EDGE);
}

/*
 * aq_sidebar_item -- one source-list row.  Selected rows get
 * the rounded accent gradient + white text, like Lion Finder.
 */
static inline void aq_sidebar_item(int x, int y, int w, int h,
                                   const char *label, int selected,
                                   uint32_t icon_color)
{
    if (selected) {
        aq_rounded_vgradient(x + 3, y + 1, w - 6, h - 2, 4,
                             AQ_SEL_TOP, AQ_SEL_BOT);
        aq_rounded_outline(x + 3, y + 1, w - 6, h - 2, 4,
                           AQ_SEL_RIM);
        if (label)
            aq_text(label, x + 24, y + h / 2 - 4, AQ_TEXT_ON_ACCENT);
    } else {
        if (label)
            aq_text(label, x + 24, y + h / 2 - 4, AQ_SIDEBAR_TEXT);
    }

    /* small glyph disc */
    aq_circle_aa(x + 13, y + h / 2, 4, icon_color);
}

/* Section caption inside the sidebar. */
static inline void aq_sidebar_caption(int x, int y, const char *label)
{
    if (label)
        aq_text(label, x + 24, y, AQ_SIDEBAR_DIM);
}

/* ============================================================
 * Scrollbar (Lion overlay style)
 * ============================================================ */

/*
 * aq_scrollbar -- translucent track + rounded thumb.  Draw it
 * LAST so it overlays the content.  `show_track` renders the
 * faint track area (use 0 for pure overlay look).
 */
static inline void aq_scrollbar(int x, int y, int w, int h,
                                int thumb_y, int thumb_h,
                                int show_track, int dragging)
{
    if (show_track)
        aq_rounded_fill_alpha(x, y, w, h, w / 2, AQ_SCROLL_TRACK);

    aq_rounded_fill_alpha(x + 1, thumb_y, w - 2, thumb_h, (w - 2) / 2,
                          dragging ? AQ_SCROLL_THUMB_H : AQ_SCROLL_THUMB);
    /* thumb top shine */
    aq_fill_alpha(x + 2, thumb_y + 1, w - 4, 1,
                  AX_ARGB(0x50, 255, 255, 255));
}

/* Up/down chevron arrows for classic scroll corners. */
static inline void aq_scroll_arrow(int cx, int cy, int up, uint32_t color)
{
    if (up) {
        aq_line(cx - 3, cy + 1, cx, cy - 2, color);
        aq_line(cx, cy - 2, cx + 3, cy + 1, color);
    } else {
        aq_line(cx - 3, cy - 1, cx, cy + 2, color);
        aq_line(cx, cy + 2, cx + 3, cy - 1, color);
    }
}

/* ============================================================
 * Selection + panels
 * ============================================================ */

/* Rounded accent selection for list/grid rows. */
static inline void aq_select_rect(int x, int y, int w, int h)
{
    aq_rounded_vgradient(x, y, w, h, 5, AQ_SEL_TOP, AQ_SEL_BOT);
    aq_rounded_outline(x, y, w, h, 5, AQ_SEL_RIM);
}

/* Hover cue for grid items (light wash). */
static inline void aq_hover_rect(int x, int y, int w, int h)
{
    aq_rounded_fill_alpha(x, y, w, h, 5, AX_ARGB(0x28, 0, 0, 0));
}

/* White card panel with hairline border (prefs groups, sheets). */
static inline void aq_panel(int x, int y, int w, int h)
{
    aq_soft_shadow(x, y, w, h, 5, 2);
    aq_rounded_fill(x, y, w, h, 5, AQ_CONTENT_BG);
    aq_rounded_outline(x, y, w, h, 5, AQ_CONTENT_EDGE);
}

/* ============================================================
 * Small Aqua controls
 * ============================================================ */

static inline void aq_checkbox(int x, int y, int checked, int focused)
{
    aq_rounded_fill(x, y, 13, 13, 3, AQ_CONTENT_BG);
    aq_rounded_outline(x, y, 13, 13, 3, AQ_FIELD_RIM);
    if (checked)
        aq_rounded_fill(x + 2, y + 2, 9, 9, 2, AQ_ACCENT);
    if (focused)
        aq_rounded_outline(x - 1, y - 1, 15, 15, 4,
                           AX_ARGB(0x70, 10, 132, 255));
}

/* Progress bar, t = 0..256. */
static inline void aq_progress(int x, int y, int w, int t)
{
    int h = 12;
    aq_rounded_fill(x, y, w, h, h / 2, AX_RGB(0xC8, 0xC8, 0xD0));
    aq_rounded_outline(x, y, w, h, h / 2, AQ_FIELD_RIM);
    int fill = (w - 2) * t / 256;
    if (fill > 0) {
        if (fill < h - 2) fill = h - 2;
        aq_rounded_vgradient(x + 1, y + 1, fill, h - 2, (h - 2) / 2,
                             AQ_ACCENT_TOP, AQ_ACCENT_BOT);
    }
}

/* Horizontal slider, t = 0..256. */
static inline void aq_slider(int x, int y, int w, int t)
{
    aq_fill_alpha(x, y + 4, w, 3, AX_ARGB(0x8C, 0, 0, 0));
    int kx = x + (w - 12) * t / 256;
    aq_soft_shadow(kx, y, 12, 12, 5, 2);
    aq_rounded_vgradient(kx, y, 12, 12, 5, AQ_BTN_TOP, AQ_BTN_BOT);
    aq_gloss_top(kx, y, 12, 12, 5, 6);
    aq_rounded_outline(kx, y, 12, 12, 5, AQ_BTN_RIM);
}

/* Segmented control (Lion), seg = index of the active segment. */
static inline void aq_segments(int x, int y, int w, int h,
                               const char **labels, int count, int active)
{
    if (count <= 0) return;
    int sw = w / count;
    for (int i = 0; i < count; i++) {
        int sx = x + i * sw;
        int swid = (i == count - 1) ? (x + w - sx) : sw;
        if (i == active) {
            aq_rounded_vgradient(sx, y, swid, h, 4,
                                 AQ_BTN_PRESS_TOP, AQ_BTN_PRESS_BOT);
        } else {
            aq_rounded_vgradient(sx, y, swid, h, 4,
                                 AQ_BTN_TOP, AQ_BTN_BOT);
        }
        aq_rect_outline(sx, y, swid, h, AX_ARGB(0x60, 0x74, 0x74, 0x80));
        if (labels && labels[i])
            aq_text(labels[i],
                    sx + swid / 2 - aq_text_width(labels[i]) / 2,
                    y + h / 2 - 4, AQ_TEXT);
    }
}

/* ============================================================
 * Procedural Aqua icons (match the dock / Finder)
 * ============================================================ */

static inline void aq_icon_terminal(int x, int y, int s)
{
    aq_rounded_vgradient(x, y, s, s, 3, AX_RGB(70, 76, 90), AX_RGB(30, 34, 42));
    aq_rounded_outline(x, y, s, s, 3, AX_ARGB(0x60, 255, 255, 255));
    aq_fill(x + 2, y + 2, s - 4, 3, AX_RGB(210, 210, 220));
    int u = s / 8;
    aq_line(x + 3 * u, y + 6 * u, x + 4 * u, y + 7 * u, AX_RGB(120, 255, 120));
    aq_line(x + 4 * u, y + 7 * u, x + 3 * u, y + 8 * u, AX_RGB(120, 255, 120));
    aq_fill(x + 5 * u, y + 8 * u - 1, 2 * u, 2, AX_RGB(120, 255, 120));
}

static inline void aq_icon_folder(int x, int y, int s)
{
    int tab_h = s / 4;
    aq_rounded_fill(x, y, s / 2, tab_h, 2, AX_RGB(52, 110, 200));
    aq_rounded_vgradient(x, y + tab_h - 2, s, s - tab_h + 2, 3,
                         AX_RGB(96, 166, 250), AX_RGB(40, 92, 190));
    aq_rounded_outline(x, y + tab_h - 2, s, s - tab_h + 2, 3,
                       AX_ARGB(0x50, 255, 255, 255));
    aq_fill_alpha(x + 2, y + tab_h, s - 4, 2, AX_ARGB(0x50, 255, 255, 255));
}

static inline void aq_icon_notes(int x, int y, int s)
{
    aq_rounded_vgradient(x, y, s, s, 3, AX_RGB(255, 240, 180), AX_RGB(245, 220, 120));
    aq_rounded_outline(x, y, s, s, 3, AX_ARGB(0x50, 120, 100, 40));
    aq_fill(x + 2, y + 2, s - 4, s / 5, AX_RGB(232, 180, 60));
    int u = s / 8;
    for (int l = 0; l < 3; l++)
        aq_fill(x + 2 * u, y + 4 * u + l * 2 * u, s - 4 * u, 1,
                AX_RGB(140, 120, 70));
}

static inline void aq_icon_calc(int x, int y, int s)
{
    aq_rounded_vgradient(x, y, s, s, 3, AX_RGB(120, 126, 138), AX_RGB(60, 64, 74));
    aq_rounded_outline(x, y, s, s, 3, AX_ARGB(0x50, 255, 255, 255));
    int u = s / 8;
    aq_fill(x + 2 * u, y + u, s - 4 * u, 2 * u, AX_RGB(140, 220, 120));
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++)
            aq_fill(x + 2 * u + c * 2 * u, y + 4 * u + r * 2 * u,
                    u + 1, u + 1, AX_RGB(220, 222, 230));
}

static inline void aq_icon_gl(int x, int y, int s)
{
    aq_rounded_vgradient(x, y, s, s, 3, AX_RGB(90, 210, 110), AX_RGB(30, 140, 60));
    aq_rounded_outline(x, y, s, s, 3, AX_ARGB(0x50, 255, 255, 255));
    int m = s / 2;
    aq_line(x + m, y + 3, x + s - 4, y + m, AX_RGB(240, 255, 240));
    aq_line(x + s - 4, y + m, x + m, y + s - 4, AX_RGB(240, 255, 240));
    aq_line(x + m, y + s - 4, x + 4, y + m, AX_RGB(240, 255, 240));
    aq_line(x + 4, y + m, x + m, y + 3, AX_RGB(240, 255, 240));
    aq_line(x + m, y + 3, x + m, y + s - 4, AX_ARGB(0xB0, 240, 255, 240));
    aq_line(x + 4, y + m, x + s - 4, y + m, AX_ARGB(0xB0, 240, 255, 240));
}

static inline void aq_icon_prefs(int x, int y, int s)
{
    aq_rounded_vgradient(x, y, s, s, 3, AX_RGB(200, 202, 210), AX_RGB(140, 142, 152));
    aq_rounded_outline(x, y, s, s, 3, AX_ARGB(0x50, 255, 255, 255));
    int u = s / 8;
    for (int l = 0; l < 3; l++) {
        int ly = y + 2 * u + l * 2 * u;
        aq_fill(x + u, ly, s - 2 * u, 2, AX_RGB(250, 250, 252));
        int kx = x + (l == 0 ? 4 : l == 1 ? 2 : 5) * u;
        aq_fill(kx, ly - 1, 2 * u, 4, AX_RGB(70, 74, 84));
    }
}

static inline void aq_icon_about(int x, int y, int s)
{
    aq_rounded_vgradient(x, y, s, s, 3, AX_RGB(210, 140, 250), AX_RGB(130, 50, 190));
    aq_rounded_outline(x, y, s, s, 3, AX_ARGB(0x50, 255, 255, 255));
    int u = s / 8;
    aq_fill(x + 3 * u, y + u, 2 * u, 2 * u, AX_RGB(255, 255, 255));
    aq_fill(x + 3 * u, y + 4 * u, 2 * u, 3 * u, AX_RGB(255, 255, 255));
}

/* White document icon (Finder grid). */
static inline void aq_icon_doc(int x, int y, int s)
{
    int fold = s / 5;
    aq_rounded_fill(x, y, s, s, 2, AX_RGB(250, 250, 252));
    aq_rect_outline(x, y, s, s, AX_ARGB(0x8C, 0x8A, 0x8A, 0x96));
    /* dog-ear */
    aq_line(x + s - fold, y, x + s - 1, y + fold, AX_ARGB(0x8C, 0x8A, 0x8A, 0x96));
    aq_line(x + s - fold, y, x + s - fold, y + fold, AX_ARGB(0x8C, 0x8A, 0x8A, 0x96));
    aq_line(x + s - fold, y + fold, x + s - 1, y + fold, AX_ARGB(0x8C, 0x8A, 0x8A, 0x96));
    /* text lines */
    int u = s / 8;
    for (int l = 0; l < 3; l++)
        aq_fill(x + u + 1, y + 3 * u + l * 2 * u, s - 2 * u - 3, 1,
                AX_ARGB(0x80, 0x9A, 0x9A, 0xA8));
}

/* Executable icon (Finder grid): dark tile with ">_". */
static inline void aq_icon_exec(int x, int y, int s)
{
    aq_rounded_vgradient(x, y, s, s, 2, AX_RGB(70, 76, 90), AX_RGB(30, 34, 42));
    aq_rounded_outline(x, y, s, s, 2, AX_ARGB(0x60, 255, 255, 255));
    int u = s / 8;
    aq_line(x + 2 * u, y + 3 * u, x + 3 * u, y + 4 * u, AX_RGB(120, 255, 120));
    aq_line(x + 3 * u, y + 4 * u, x + 2 * u, y + 5 * u, AX_RGB(120, 255, 120));
    aq_fill(x + 4 * u, y + 5 * u - 1, 2 * u, 2, AX_RGB(120, 255, 120));
}

#endif /* AXAQUA_H */
