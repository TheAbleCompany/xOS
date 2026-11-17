/*
 * shim_ax.h -- host-side shim of the ArtyomX64 libax API for testing.
 *
 * The game code (hovertank.c) is compiled UNCHANGED on Linux with
 * -DARTYOM_HOST_SHIM; this header replaces ../lib/libax.h.
 *
 * Behaviour:
 *   * canvas  = malloc'd framebuffer
 *   * events  = driven by a script file ($HOV_SCRIPT):
 *         wait <n>       -- advance n commits
 *         key <ch>       -- inject AX_EV_KEY
 *         menu <m> <i>   -- inject AX_EV_MENU
 *         dump <tag>     -- write <tag>.ppm right after the next commit
 *         quit           -- exit(0)
 *   * clock   = fake RTC: +1/20 s per commit (FRAME_FPS), so the
 *               30 s timer runs deterministically (600 commits)
 *   * commits = each ax_commit increments the counter; with
 *               $HOV_DUMP_ALL=1 every frame is written out
 *
 * Frame dumps are PPM (P6); convert with PIL for viewing.
 */

#ifndef SHIM_AX_H
#define SHIM_AX_H

#include <stdint.h>

#define AX_ARGB(a,r,g,b) (((uint32_t)(a)<<24)|((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))
#define AX_RGB(r,g,b)    AX_ARGB(0xFF,r,g,b)

#define AX_EV_NONE   0
#define AX_EV_KEY    1
#define AX_EV_MOUSE  2
#define AX_EV_CLOSE  3
#define AX_EV_RESIZE 4
#define AX_EV_NEWWIN 5
#define AX_EV_QUIT   6
#define AX_EV_MENU   7

#define AX_KEY_CTRL_S    0x101
#define AX_KEY_CTRL_O    0x102
#define AX_KEY_CTRL_N    0x103
#define AX_KEY_BACKSPACE 8
#define AX_KEY_ENTER     13
#define AX_KEY_CTRL      0x100

typedef struct {
    uint32_t type;
    uint32_t key;
    int32_t  mx, my;
    uint32_t buttons;
    uint32_t w, h;
} ax_event;

typedef struct {
    int32_t year, month, day;
    int32_t hour, minute, second;
} ax_time_t;

typedef struct {
    uint32_t width;
    uint32_t height;
} ax_screen_t;

typedef struct {
    const char *title;
    const char **items;
    uint32_t item_count;
} ax_menu_def_t;

typedef struct {
    const char    *app_name;
    const ax_menu_def_t *menus;
    uint32_t       menu_count;
} ax_menubar_def_t;

/* libax.h globals (mirrored) */
extern int g_cw, g_ch;
extern uint32_t *g_canvas;

uint32_t *ax_surface(const char *title, int w, int h);
int   ax_poll(uint32_t *canvas, ax_event *ev);
void  ax_commit(uint32_t *canvas);
int   ax_menu_set(const ax_menubar_def_t *def);
int   ax_win_close(uint32_t *canvas);
int   ax_win_focus(uint32_t *canvas);
void  ax_time(ax_time_t *t);
void  ax_screen(ax_screen_t *s);
void  yield(void);

static inline void ax_canvas_dims(int w, int h) { g_cw = w; g_ch = h; }

static inline void ax_px(int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= g_cw || y >= g_ch) return;
    g_canvas[y * g_cw + x] = c;
}

static inline void ax_fill(uint32_t c)
{
    for (int i = 0; i < g_cw * g_ch; i++) g_canvas[i] = c;
}

static inline void ax_rect(int x, int y, int w, int h, uint32_t c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            ax_px(x + i, y + j, c);
}

/* 8x8 font: reuse the OS font header for pixel-identical text */
#include "../lib/font8x8_basic.h"

static inline void ax_char(char ch, int x, int y, uint32_t c)
{
    const unsigned char *g = (const unsigned char*)font8x8_basic[(unsigned char)ch];
    for (int cy = 0; cy < 8; cy++)
        for (int cx = 0; cx < 8; cx++)
            if (g[cy] & (1 << cx)) ax_px(x + cx, y + cy, c);
}

static inline void ax_text(const char *s, int x, int y, uint32_t c)
{
    int ox = x;
    while (*s) {
        if (*s == '\n') { y += 9; x = ox; s++; continue; }
        ax_char(*s++, x, y, c);
        x += 8;
    }
}

#endif /* SHIM_AX_H */
