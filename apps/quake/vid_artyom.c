/*
 * vid_artyom.c -- программный видео-драйвер Quake для ArtyomX64.
 *
 * Кадр рендерится TyrQuake'ом в 8-битный буфер 320x200 (vid.buffer),
 * затем здесь переводится по палитре в ARGB и растягивается 2x в
 * канвас окна WindowServer'а (640x400).  Канвас -- разделяемая
 * память, поэтому копирование прямо в него; ax_commit() сообщает
 * серверу, что окно изменилось.
 *
 * Палитра хранится как LUT; VID_ShiftPalette() (вспышки урона,
 * подводный оттенок) перестраивает её.
 */

#include <stdint.h>

#include "common.h"
#include "console.h"
#include "d_local.h"
#include "host.h"
#include "keys.h"
#include "quakedef.h"
#include "screen.h"
#include "sys.h"
#include "vid.h"

#include "artyom.h"
#include "axglue.h"

#define BASEWIDTH   320
#define BASEHEIGHT  200
#define WIN_SCALE   2
#define WIN_WIDTH   (BASEWIDTH * WIN_SCALE)
#define WIN_HEIGHT  (BASEHEIGHT * WIN_SCALE)

viddef_t vid;

unsigned short d_8to16table[256];
unsigned d_8to24table[256];

/* Статические буферы, как в vid_null.c, но с warpbuffer'ом. */
static byte  vid_buffer[BASEWIDTH * BASEHEIGHT];
static short zbuffer[BASEWIDTH * BASEHEIGHT];
static byte  surfcache[SURFCACHE_SIZE_AT_320X200];
static byte  warpbuffer[BASEWIDTH * BASEHEIGHT];

static byte     current_palette[768];
static uint32_t pal_lut[256];

static void build_palette_lut(void)
{
    for (int i = 0; i < 256; i++) {
        unsigned r = current_palette[i * 3 + 0];
        unsigned g = current_palette[i * 3 + 1];
        unsigned b = current_palette[i * 3 + 2];
        uint32_t rgb = (r << 16) | (g << 8) | b;

        pal_lut[i] = 0xFF000000u | rgb;
        d_8to24table[i] = rgb;
        d_8to16table[i] = (unsigned short)(((r >> 3) << 11) |
                                           ((g >> 2) << 5) |
                                           (b >> 3));
    }
}

void VID_SetPalette(const byte *palette)
{
    if (!palette)
        return;

    for (int i = 0; i < 768; i++)
        current_palette[i] = palette[i];

    build_palette_lut();
}

void VID_ShiftPalette(const byte *palette)
{
    VID_SetPalette(palette);
}

void VID_InitColormap(const byte *palette)
{
    (void)palette;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
}

void VID_GetDesktopRect(vrect_t *rect)
{
    rect->x = 0;
    rect->y = 0;
    rect->width = 1280;
    rect->height = 800;
}

qboolean VID_CheckAdequateMem(int width, int height)
{
    (void)width;
    (void)height;
    return true;
}

void VID_LockBuffer(void) { }
void VID_UnlockBuffer(void) { }
void VID_ProcessEvents(void) { AX_PumpEvents(); }

void VID_AddCommands(void) { }
void VID_RegisterVariables(void) { }

void D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height)
{
    (void)x; (void)y; (void)pbitmap; (void)width; (void)height;
}

void D_EndDirectRect(int x, int y, int width, int height)
{
    (void)x; (void)y; (void)width; (void)height;
}

qboolean window_visible(void)
{
    return true;
}

/*
 * Пересоздать внутренние буферы.  Разрешение фиксировано 320x200;
 * mode оставлен для совместимости с vid_restart.
 */
qboolean VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
    (void)mode;

    vid.width = vid.conwidth = BASEWIDTH;
    vid.height = vid.conheight = BASEHEIGHT;
    vid.rowbytes = vid.conrowbytes = BASEWIDTH;
    vid.aspect = 1.0f;
    vid.numpages = 1;
    vid.output.scale = WIN_SCALE;
    vid.output.width = WIN_WIDTH;
    vid.output.height = WIN_HEIGHT;

    vid.buffer = vid.conbuffer = vid_buffer;
    vid.direct = NULL;

    VID_InitColormap(palette);

    d_pzbuffer = zbuffer;
    r_warpbuffer = warpbuffer;
    D_InitCaches(surfcache, sizeof(surfcache));
    R_AllocSurfEdges(false);

    VID_SetPalette(palette);

    vid.recalc_refdef = 1;
    SCR_CheckResize();
    Con_CheckResize();

    return true;
}

void VID_Init(const byte *palette)
{
    AX_InitDebug();

    vid_windowed_mode.width = WIN_WIDTH;
    vid_windowed_mode.height = WIN_HEIGHT;
    vid_windowed_mode.bpp = 32;
    vid_windowed_mode.refresh = 0;
    vid_windowed_mode.min_scale = WIN_SCALE;
    vid_windowed_mode.resolution.scale = WIN_SCALE;
    vid_windowed_mode.resolution.width = BASEWIDTH;
    vid_windowed_mode.resolution.height = BASEHEIGHT;

    /* wserver может стартовать чуть позже нашего процесса. */
    for (int i = 0; i < 1000 && !ax_win; i++) {
        ax_win = axg_surface("Quake", WIN_WIDTH, WIN_HEIGHT,
                             &ax_win_w, &ax_win_h);
        if (!ax_win)
            axg_yield();
    }

    if (!ax_win) {
        Sys_Error("VID: could not create WindowServer surface");
    }

    AX_DebugPuts("QUAKE: surface ");
    AX_DebugDec(ax_win_w);
    AX_DebugPuts("x");
    AX_DebugDec(ax_win_h);
    AX_DebugPuts("\n");

    axg_win_focus(ax_win);

    VID_SetMode(&vid_windowed_mode, palette);

    vid_menudrawfn = VID_MenuDraw;
    vid_menukeyfn = VID_MenuKey;
}

void VID_Shutdown(void)
{
    /* Окно закроет сервер при завершении процесса. */
}

/*
 * Полный кадр: 320x200 -> ARGB 640x400.
 * Рисуем каждую строку дважды и каждый пиксель дважды.
 */
static void blit_frame(void)
{
    int w = ax_win_w;
    int h = ax_win_h;

    if (!ax_win || w <= 0 || h <= 0)
        return;

    for (int y = 0; y < BASEHEIGHT; y++) {
        const byte *src = vid.buffer + y * BASEWIDTH;
        uint32_t *d0 = ax_win + (uint32_t)(y * WIN_SCALE) * (uint32_t)w;
        uint32_t *d1 = d0 + w;

        if ((y * WIN_SCALE + 1) * w >= (uint32_t)(h * w))
            break;

        for (int x = 0; x < BASEWIDTH; x++) {
            uint32_t c = pal_lut[src[x]];
            int dx = x * WIN_SCALE;

            if (dx + 1 >= w)
                break;

            d0[dx] = c;
            d0[dx + 1] = c;
            d1[dx] = c;
            d1[dx + 1] = c;
        }
    }
}

void VID_Update(vrect_t *rects)
{
    static int dbg_updates;

    (void)rects;

    if (!ax_win)
        return;

    if (dbg_updates < 4) {
        AX_DebugPuts("QUAKE: vid_update ");
        AX_DebugDec(dbg_updates);
        AX_DebugPuts("\n");
        dbg_updates++;
    }

    blit_frame();
    axg_commit(ax_win);

    /* Кооперативная ОС: даём wserver'у отрисовать кадр и принять ввод. */
    axg_yield();
}
