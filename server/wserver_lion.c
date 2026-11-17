/*
 * ============================================================
 * wserver_lion.c  --  Launchpad + Mission Control (Mac OS X Lion)
 * ============================================================
 *
 * Две флагманские фичи OS X 10.7 Lion, построенные на НОВЫХ
 * возможностях этой ОС (userspace WindowServer):
 *
 *   LAUNCHPAD (F4)    -- полноэкранная сетка всех установленных
 *                        приложений.  Клик по иконке = request_launch(),
 *                        то есть exec() через ядро; иконки рисуются теми
 *                        же процедурными Aqua-примитивами, что и док.
 *
 *   MISSION CONTROL (F3) -- полноэкранный Exposé: все живые окна
 *                        (включая свёрнутые) раскладываются сеткой
 *                        миниатюр; клик по миниатюре = surf_focus()
 *                        + genie-восстановление из дока.
 *
 *   FULLSCREEN (F11 / зелёная кнопка) -- полноэкранный режим окна
 *        живёт в wserver_surface.c (toggle_fullscreen); здесь только
 *        общая анимация-затемнение.
 *
 * Как это устроено технически:
 *   - при открытии оверлея КОПИЯ текущего кадра блюрится
 *     (gfx_blur_region) и кэшируется в SHM-буфере -- дальше каждый
 *     кадр только blit кэша + затемнение, без повторного блюра;
 *   - вся система-хром (меню-бар, док, виджеты) на время оверлея
 *     не рисуется (см. compose() в wserver.c), как в настоящем Lion;
 *   - клавиатура во время оверлея не доходит до приложений:
 *     Esc/F3/F4 перехватываются lion_system_key() раньше доставки
 *     в сфокусированное окно (wserver_input.c).
 *
 * Клиентского IPC здесь нет -- это чисто серверная часть.
 */

#include "wserver_internal.h"

#ifdef HOST_HARNESS
/*
 * Хост-харнесс: ядерных сисколлов нет -- обычный host malloc
 * (та же схема, что в wserver_wallpaper.c).
 */
static void *lion_shm_alloc(unsigned long size) { return malloc(size); }
static void  lion_shm_free(void *p)             { free(p); }
#else
static void *lion_shm_alloc(unsigned long size)
{
    volatile unsigned long ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)183), "D"(size)
        : "memory");
    return (void *)(uintptr_t)ret;
}

static void lion_shm_free(void *ptr)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)184), "D"((long)(uintptr_t)ptr)
        : "memory");
    (void)ret;
}
#endif

/*
 * ============================================================
 * Module state
 * ============================================================
 */

/* Launchpad */
static bool     lp_open = false;
static int      lp_anim = 0;           /* кадры fade-in              */
static uint32_t *lp_bg = 0;            /* кэш заблюренного кадра     */

/* Mission Control */
static bool     mc_open = false;
static int      mc_anim = 0;
static uint32_t *mc_bg = 0;

/* индексы миниатюр Mission Control: tile -> surface id */
static int mc_tile_surface[AX_MAX_WINDOWS];
static int mc_tile_count = 0;

#define LION_FADE_FRAMES 10

/*
 * ============================================================
 * Backdrop cache: один раз сильно блюрим текущий кадр и
 * запоминаем результат.
 *
 * "Дорогой" blur по-дешёвому (как делают настоящие композиторы):
 *   1. box-декеймация кадра в 1/6 по каждой оси (усреднение блока);
 *   2. gfx_blit_argb_scaled растягивает мелкий буфер обратно на
 *      весь экран (билинейно-ступенчато);
 *   3. пара проходов gfx_blur_region сглаживает блочность.
 * Результат эквивалентен гауссову blur с радиусом в десятки
 * пикселей, но за O(площади/36) вместо O(проходов*площади).
 * ============================================================
 */

#define LION_BLUR_DIV 6

static uint32_t *lion_capture_blurred(void)
{
    int sw = scr_w / LION_BLUR_DIV;
    int sh = scr_h / LION_BLUR_DIV;
    if (sw < 2) sw = 2;
    if (sh < 2) sh = 2;

    uint32_t *buf = (uint32_t *)lion_shm_alloc(
        (unsigned long)scr_w * scr_h * sizeof(uint32_t));
    if (!buf) {
        serial_puts_ax("LION: backdrop alloc FAIL\n");
        return 0;
    }

    uint32_t *small = (uint32_t *)lion_shm_alloc(
        (unsigned long)sw * sh * sizeof(uint32_t));

    if (small) {
        /* 1. box-декеймация: среднее по блоку LION_BLUR_DIV x DIV */
        for (int dy = 0; dy < sh; dy++) {
            int y0 = dy * scr_h / sh;
            int y1 = (dy + 1) * scr_h / sh;
            for (int dx = 0; dx < sw; dx++) {
                int x0 = dx * scr_w / sw;
                int x1 = (dx + 1) * scr_w / sw;

                unsigned long r = 0, g = 0, b = 0;
                int n = 0;
                for (int y = y0; y < y1; y++) {
                    const uint32_t *row =
                        gfx_backbuffer() + (size_t)y * scr_w;
                    for (int x = x0; x < x1; x++) {
                        uint32_t c = row[x];
                        r += (c >> 16) & 0xFF;
                        g += (c >> 8) & 0xFF;
                        b += c & 0xFF;
                        n++;
                    }
                }
                if (!n) n = 1;
                small[dy * sw + dx] = 0xFF000000 |
                    ((uint32_t)(r / n) << 16) |
                    ((uint32_t)(g / n) << 8)  |
                    (uint32_t)(b / n);
            }
        }

        /* 2. растянуть мелкий буфер на весь экран */
        gfx_blit_argb_scaled(small, sw, sh, 0, 0, scr_w, scr_h);

        lion_shm_free(small);

        /* 3. сгладить блочность ступенчатого апскейла */
        gfx_blur_region(0, 0, scr_w, scr_h, 2);
    } else {
        /* мелкий буфер не выделился -- старый добрый многократный
         * blur по месту (медленнее, но работает) */
        gfx_blur_region(0, 0, scr_w, scr_h, 6);
    }

    memcpy(buf, gfx_backbuffer(),
           (size_t)scr_w * scr_h * sizeof(uint32_t));

    return buf;
}

static void lion_drop_backdrop(uint32_t **bg)
{
    if (*bg) {
        lion_shm_free(*bg);
        *bg = 0;
    }
}

/*
 * ============================================================
 * Launchpad -- таблица приложений
 * ============================================================
 *
 * Список синхронизирован с initrd (apps/Makefile, цель initrd):
 * имена совпадают с task name, который exec() видит в initrd.
 * elf == 0 -- системный элемент (About), запускает панель "About".
 */

#define LP_MAX_APPS 16

static struct {
    const char *elf;
    const char *label;
    uint32_t    color;
    int         icon;      /* LION_ICON_* из wserver_ui.c */
} lp_apps[LP_MAX_APPS] = {
    { "temu",      "Terminal", RGB(52,60,72)    , LION_ICON_TERMINAL },
    { "files",     "Files",    RGB(70,140,240)  , LION_ICON_FOLDER   },
    { "notes",     "Notes",    RGB(255,214,90)  , LION_ICON_NOTES    },
    { "calc",      "Calc",     RGB(90,98,110)   , LION_ICON_CALC     },
    { "cube",      "GL Cube",  RGB(52,199,89)   , LION_ICON_GL       },
    { "prefs",     "Settings", RGB(150,150,160) , LION_ICON_PREFS    },
    { "raycaster", "Raycaster",RGB(190,80,50)   , LION_ICON_RAYCAST  },
    { "doom",      "DOOM",     RGB(150,32,28)   , LION_ICON_DOOM     },
    { "quake",     "Quake",    RGB(110,78,48)   , LION_ICON_QUAKE    },
    { "bench",     "Bench",    RGB(40,120,180)  , LION_ICON_BENCH    },
    { "guitest",   "GUI Test", RGB(170,90,180)  , LION_ICON_GUITEST  },
    { 0,           "About",    RGB(191,90,242)  , LION_ICON_ABOUT    },
};
#define LP_APP_COUNT 12

/* геометрия сетки Launchpad (4 колонки) */
#define LP_COLS     4
#define LP_ICON     76
#define LP_CELL_W   148
#define LP_CELL_H   122

bool lion_launchpad_open(void) { return lp_open; }
bool lion_mission_open(void)   { return mc_open; }
bool lion_overlay_open(void)   { return lp_open || mc_open; }

void lion_toggle_launchpad(void)
{
    if (lp_open) {
        lp_open = false;
        lion_drop_backdrop(&lp_bg);
        return;
    }

    /* Lion: Mission Control и Launchpad не живут одновременно. */
    if (mc_open) lion_toggle_mission();

    lp_open = true;
    lp_anim = 0;
    /* Фон захватывается на первом кадре отрисовки оверлея --
     * к тому моменту compose() уже нарисовал десктоп целиком. */
}

void lion_toggle_mission(void)
{
    if (mc_open) {
        mc_open = false;
        lion_drop_backdrop(&mc_bg);
        return;
    }

    if (lp_open) lion_toggle_launchpad();

    mc_open = true;
    mc_anim = 0;
}

void lion_overlays_close(void)
{
    if (lp_open) lion_toggle_launchpad();
    if (mc_open) lion_toggle_mission();
}

/*
 * Перехват "системных" клавиш Lion.  Вызывается из
 * process_input() ДО доставки события приложению; возвращает
 * true, если клавиша съедена оверлеем/системой.
 *
 *   F3  (0x3D) -- Mission Control (дефолт Lion)
 *   F4  (0x3E) -- Launchpad         (дефолт Lion)
 *   F11 (0x57) -- fullscreen активного окна
 *   Esc (0x01) -- закрыть оверлей
 */
bool lion_system_key(int sc)
{
    switch (sc) {
        case 0x3D: lion_toggle_mission();   return true;
        case 0x3E: lion_toggle_launchpad(); return true;

        case 0x57: {
            ax_surface_win *w = surf_active_window();
            if (w) toggle_fullscreen(w);
            return true;
        }

        case 0x01:
            if (lion_overlay_open()) {
                lion_overlays_close();
                return true;
            }
            return false;
    }
    return false;
}

/*
 * ============================================================
 * Launchpad -- отрисовка
 * ============================================================
 */

static int lp_hover = -1;

static void draw_launchpad(void)
{
    /*
     * Первый кадр: захватываем и блюрим фон.  Дальше каждый кадр
     * blit кэша + варьируем только затемнение (fade-in).
     */
    if (!lp_bg) lp_bg = lion_capture_blurred();

    if (lp_bg) {
        gfx_blit_argb(lp_bg, scr_w, scr_h, 0, 0);
    } else {
        gfx_clear_rgb(RGB(20,22,30));
    }

    int alpha = 0xB4;                       /* финальное затемнение */
    if (lp_anim < LION_FADE_FRAMES)
        alpha = (lp_anim * 0xB4) / LION_FADE_FRAMES;
    gfx_fill_rect_alpha(0, 0, scr_w, scr_h,
                        ((uint32_t)alpha << 24));

    lp_anim++;

    /* --- сетка иконок --- */
    int rows = (LP_APP_COUNT + LP_COLS - 1) / LP_COLS;
    int grid_w = LP_COLS * LP_CELL_W;
    int grid_h = rows * LP_CELL_H;
    int gx0 = scr_w / 2 - grid_w / 2;
    int gy0 = AX_MENUBAR_H + (scr_h - AX_MENUBAR_H - grid_h) / 2 - 10;
    if (gy0 < AX_MENUBAR_H + 8) gy0 = AX_MENUBAR_H + 8;

    lp_hover = -1;

    for (int i = 0; i < LP_APP_COUNT; i++) {
        int col = i % LP_COLS;
        int row = i / LP_COLS;
        int cx  = gx0 + col * LP_CELL_W + LP_CELL_W / 2;
        int cy  = gy0 + row * LP_CELL_H + LP_CELL_H / 2;

        int ix = cx - LP_ICON / 2;
        int iy = cy - LP_ICON / 2 - 6;

        /* hover: мягкая белая подложка под иконкой (Lion cue) */
        bool hovered = mouse.x >= cx - LP_CELL_W / 2 + 6 &&
                       mouse.x <  cx + LP_CELL_W / 2 - 6 &&
                       mouse.y >= cy - LP_CELL_H / 2 + 2 &&
                       mouse.y <  cy + LP_CELL_H / 2 - 2;
        if (hovered) lp_hover = i;

        if (hovered) {
            gfx_soft_shadow(ix - 4, iy - 4, LP_ICON + 8, LP_ICON + 8, 8, 2);
            gfx_rounded_rect_alpha(ix - 6, iy - 6, LP_ICON + 12, LP_ICON + 12,
                                   12, ARGB(0x30,255,255,255));
        }

        lion_icon_tile(lp_apps[i].icon, ix, iy, LP_ICON, lp_apps[i].color);

        /* подпись с мягкой тенью, как в Lion */
        int lw = gfx_text_width(lp_apps[i].label);
        int ly = iy + LP_ICON + 6;
        gfx_text(lp_apps[i].label, cx - lw / 2 + 1, ly + 1, ARGB(0x90,0,0,0));
        gfx_text(lp_apps[i].label, cx - lw / 2, ly, RGB(250,250,252));
    }

    /* --- заголовок сверху (Lion держит там поиск; у нас -- имя) --- */
    {
        const char *t = "Launchpad";
        int tw = gfx_text_width(t);
        gfx_text(t, scr_w / 2 - tw / 2, AX_MENUBAR_H + 6, ARGB(0xA0,255,255,255));
    }
}

/*
 * ============================================================
 * Mission Control -- отрисовка (Exposé-сетка окон)
 * ============================================================
 */

#define MC_TILE_STRIP 18       /* мини-тайтлбар миниатюры            */
#define MC_MARGIN     40
#define MC_GAP        16

static int mc_hover = -1;

static void draw_mission(void)
{
    if (!mc_bg) mc_bg = lion_capture_blurred();

    if (mc_bg) {
        gfx_blit_argb(mc_bg, scr_w, scr_h, 0, 0);
    } else {
        gfx_clear_rgb(RGB(16,18,26));
    }

    int alpha = 0xC2;
    if (mc_anim < LION_FADE_FRAMES)
        alpha = (mc_anim * 0xC2) / LION_FADE_FRAMES;
    gfx_fill_rect_alpha(0, 0, scr_w, scr_h, ((uint32_t)alpha << 24));

    mc_anim++;

    /* --- собрать живые окна (включая свёрнутые) --- */
    mc_tile_count = 0;
    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *w = &surfaces[i];
        if (w->used && w->canvas) {
            mc_tile_surface[mc_tile_count] = i;
            mc_tile_count++;
        }
    }

    if (mc_tile_count == 0) {
        const char *t = "Mission Control";
        int tw = gfx_text_width(t);
        gfx_text(t, scr_w / 2 - tw / 2, scr_h / 2, ARGB(0xB0,255,255,255));
        return;
    }

    /* --- сетка: 1-4 окна -> 2 колонки, 7-16 -> 4 колонки --- */
    int cols = (mc_tile_count <= 1) ? 1 :
               (mc_tile_count <= 4) ? 2 :
               (mc_tile_count <= 9) ? 3 : 4;
    int rows = (mc_tile_count + cols - 1) / cols;

    int avail_w = scr_w - MC_MARGIN * 2 - (cols - 1) * MC_GAP;
    int avail_h = scr_h - AX_MENUBAR_H - MC_MARGIN - 24 -
                  (rows - 1) * MC_GAP;

    int tile_w = avail_w / cols;
    int tile_h = avail_h / rows;
    if (tile_w > 380) tile_w = 380;
    if (tile_h > 240) tile_h = 240;

    int grid_w = cols * tile_w + (cols - 1) * MC_GAP;
    int grid_h = rows * tile_h + (rows - 1) * MC_GAP;
    int gx0 = scr_w / 2 - grid_w / 2;
    int gy0 = AX_MENUBAR_H + (scr_h - AX_MENUBAR_H - 24 - grid_h) / 2 + 6;
    if (gy0 < AX_MENUBAR_H + 8) gy0 = AX_MENUBAR_H + 8;

    mc_hover = -1;

    for (int t = 0; t < mc_tile_count; t++) {
        ax_surface_win *w = &surfaces[mc_tile_surface[t]];

        int col = t % cols;
        int row = t / cols;
        int tx  = gx0 + col * (tile_w + MC_GAP);
        int ty  = gy0 + row * (tile_h + MC_GAP);
        int th_h = tile_h - 14;      /* 14px снизу -- подпись приложения */

        bool hovered = mouse.x >= tx && mouse.x < tx + tile_w &&
                       mouse.y >= ty && mouse.y < ty + th_h + 12;
        if (hovered) mc_hover = t;

        /* --- карточка миниатюры --- */
        gfx_soft_shadow(tx, ty, tile_w, th_h, 10, 3);
        gfx_rounded_vgradient_alpha(tx, ty, tile_w, th_h, 8,
                                    hovered ? ARGB(0xFF,252,252,255)
                                            : ARGB(0xE8,240,240,246),
                                    hovered ? ARGB(0xFF,232,232,240)
                                            : ARGB(0xE0,222,222,230));
        gfx_rounded_outline(tx, ty, tile_w, th_h, 8,
                            hovered ? ARGB(0x90,255,255,255)
                                    : ARGB(0x60,40,40,50));

        /* мини-тайтлбар с именем окна */
        gfx_fill_rect_alpha(tx + 1, ty + 1, tile_w - 2, MC_TILE_STRIP,
                            ARGB(0xD8,236,236,240));
        gfx_fill_rect_alpha(tx + 1, ty + MC_TILE_STRIP, tile_w - 2, 1,
                            ARGB(0x60,101,101,101));
        gfx_text(w->title, tx + 8,
                 ty + (MC_TILE_STRIP - gfx_font_h() + 1) / 2,
                 w->minimized ? LION_TITLE_TEXT_U : LION_TITLE_TEXT);

        /* свёрнутое окно -- жёлтая точка в тайтлбаре (Lion cue) */
        if (w->minimized) {
            gfx_circle_aa(tx + tile_w - 10, ty + MC_TILE_STRIP / 2 + 1, 3,
                          RGB(245,195,95));
        }

        /* --- миниатюра канваса, aspect-fit внутри карточки --- */
        int bx = tx + 4;
        int by = ty + MC_TILE_STRIP + 2;
        int bw = tile_w - 8;
        int bh = th_h - MC_TILE_STRIP - 6;

        if (w->canvas && bw > 4 && bh > 4) {
            int cw = w->canvas_w > 0 ? w->canvas_w : 1;
            int ch = w->canvas_h > 0 ? w->canvas_h : 1;

            int dw = bw, dh = bh;
            if (cw * bh > ch * bw) dh = ch * bw / cw;   /* шире, чем бокс */
            else                    dw = cw * bh / ch;
            if (dw > bw) { dw = bw; dh = ch * bw / cw; }
            if (dh > bh) { dh = bh; dw = cw * bh / ch; }

            gfx_fill_rect(bx, by, bw, bh, ARGB(0xFF,24,26,32));
            gfx_blit_argb_scaled(w->canvas, cw, ch,
                                 bx + (bw - dw) / 2, by + (bh - dh) / 2,
                                 dw, dh);
        }

        /* подпись приложения под карточкой */
        {
            char label[24];
            label[0] = 0;
            /* owner name берём PROC_NAME'ом -- как док */
            if (proc_name_sys(w->owner_pid, label, (int)sizeof(label) - 1) != 0)
                label[0] = 0;
            label[sizeof(label) - 1] = 0;

            const char *who = label[0] ? label : (const char *)w->app_name;
            int lw = gfx_text_width(who);
            gfx_text(who, tx + tile_w / 2 - lw / 2 + 1,
                     ty + th_h + 3, ARGB(0x80,0,0,0));
            gfx_text(who, tx + tile_w / 2 - lw / 2,
                     ty + th_h + 2, RGB(235,235,245));
        }
    }
}

/*
 * ============================================================
 * Клик по оверлею (process_input маршрутизирует сюда)
 * ============================================================
 */

void lion_overlay_click(int mx, int my)
{
    /* попадания считаются по mc_hover/lp_hover, посчитанным
     * отрисовкой текущего кадра */
    (void)mx;
    (void)my;

    if (lp_open) {
        /* клик мимо сетки закрывает Launchpad (как в Lion -- Esc
         * или клик по пустому месту) */
        if (lp_hover >= 0) {
            int i = lp_hover;

            if (lp_apps[i].elf)
                request_launch(lp_apps[i].elf);
            else
                launch_about();

            lion_toggle_launchpad();
            return;
        }
        lion_toggle_launchpad();
        return;
    }

    if (mc_open) {
        if (mc_hover >= 0) {
            ax_surface_win *w = &surfaces[mc_tile_surface[mc_hover]];

            bool was_minimized = w->minimized;
            w->minimized = false;
            w->visible = true;
            if (was_minimized) {
                w->anim_kind = AX_ANIM_RESTR;   /* genie из дока */
                w->anim_t = 0;
            }
            surf_focus(w);

            lion_toggle_mission();
            return;
        }
        lion_toggle_mission();
        return;
    }
}

/*
 * ============================================================
 * Отрисовка оверлеев -- вызывается из compose() ПОСЛЕ всех окон
 * и хрома, ДО курсора.
 * ============================================================
 */

void lion_overlays_draw(void)
{
    if (mc_open) draw_mission();
    else if (lp_open) draw_launchpad();
}
