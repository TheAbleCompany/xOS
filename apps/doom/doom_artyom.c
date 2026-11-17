/*
 * doom_artyom.c -- платформенный слой doomgeneric для ArtyomX64.
 *
 * Реализует шесть функций DG_* и точку входа _start.
 *
 * Графика: окно 640x400 создаётся через ax_surface() (WindowServer),
 * кадр из DG_ScreenBuffer копируется в канвас (ARGB, alpha=0xFF:
 * композитор сервера учитывает альфу).
 *
 * Ввод: сервер шлёт AX_EV_RAWKEY на каждое нажатие и отпускание
 * (сканкод set 1 + признак extended), здесь он превращается в
 * keycode DOOM и кладётся в кольцо.  DG_GetKey отдаёт события
 * i_input.c, у которого TranslateKey() -- тождественная функция.
 *
 * Время: сисколл 196 (timer_ticks_ms, PIT 1000 Гц).  Планировщик
 * кооперативный, поэтому DG_DrawFrame / DG_SleepMs / каждый
 * 64-й DG_GetTicksMs делают ax_yield().
 */

#include <stdint.h>

#include "doomgeneric.h"
#include "doomkeys.h"

/*
 * libax.h тянет lib/common.h, который объявляет memcpy/memset с
 * сигнатурами ядра.  Поэтому <string.h> сюда включать нельзя --
 * наши собственные строковые хелперы ниже.
 */
#include "libax.h"

#define WIN_W DOOMGENERIC_RESX
#define WIN_H DOOMGENERIC_RESY

/* ------------------------------------------------------------------ */
/* Отладка в COM1 (видно в QEMU -serial file:...)                     */
/* ------------------------------------------------------------------ */

#define DOOM_DEBUG 1

static inline void dbg_outb(unsigned short port, unsigned char val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char dbg_inb(unsigned short port)
{
    unsigned char r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static void dbg_init(void)
{
    dbg_outb(0x3F9, 0x00);
    dbg_outb(0x3FB, 0x80);
    dbg_outb(0x3F8, 0x03);
    dbg_outb(0x3F9, 0x00);
    dbg_outb(0x3FB, 0x03);
    dbg_outb(0x3FA, 0xC7);
    dbg_outb(0x3FC, 0x0B);
}

static void dbg_puts(const char *s)
{
#if DOOM_DEBUG
    while (*s) {
        while ((dbg_inb(0x3FD) & 0x20) == 0)
            ;
        dbg_outb(0x3F8, (unsigned char)*s++);
    }
#else
    (void)s;
#endif
}

static void dbg_hex(uint32_t v)
{
#if DOOM_DEBUG
    char buf[11];
    int i;

    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        unsigned d = (v >> ((7 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    buf[10] = 0;
    dbg_puts(buf);
#else
    (void)v;
#endif
}

/* ------------------------------------------------------------------ */
/* Время (сисколл 196 == timer_ticks_ms)                              */
/* ------------------------------------------------------------------ */

static inline uint32_t ax_now_ms(void)
{
    volatile uint32_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"((uint32_t)196) : "memory");
    return r;
}

/* ------------------------------------------------------------------ */
/* Очередь клавиш                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char key;      /* keycode DOOM                    */
    unsigned char pressed;  /* 1 = нажатие, 0 = отпускание     */
} key_ev_t;

#define KEYQ_SIZE 128

static key_ev_t keyq[KEYQ_SIZE];
static int keyq_head;
static int keyq_tail;

static void key_push(unsigned char key, int pressed)
{
    int next = (keyq_head + 1) % KEYQ_SIZE;

    if (next == keyq_tail)
        return;                     /* переполнение: теряем */

    keyq[keyq_head].key = key;
    keyq[keyq_head].pressed = (unsigned char)(pressed ? 1 : 0);
    keyq_head = next;
}

/* ------------------------------------------------------------------ */
/* Сканкод set 1 -> keycode DOOM                                      */
/* ------------------------------------------------------------------ */

static unsigned char map_scancode(unsigned sc, int ext)
{
    static const char digits[]   = "1234567890";
    static const char row_qwer[] = "qwertyuiop";
    static const char row_asdf[] = "asdfghjkl";
    static const char row_zxcv[] = "zxcvbnm";

    if (ext) {
        switch (sc) {
        case 0x1c: return KEY_ENTER;        /* keypad Enter      */
        case 0x1d: return KEY_FIRE;         /* правый Ctrl       */
        case 0x35: return KEYP_DIVIDE;      /* keypad '/'        */
        case 0x38: return KEY_RALT;         /* правый Alt        */
        case 0x47: return KEY_HOME;
        case 0x48: return KEY_UPARROW;
        case 0x49: return KEY_PGUP;
        case 0x4b: return KEY_LEFTARROW;
        case 0x4d: return KEY_RIGHTARROW;
        case 0x4f: return KEY_END;
        case 0x50: return KEY_DOWNARROW;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INS;
        case 0x53: return KEY_DEL;
        default:   return 0;
        }
    }

    /* Буквенно-цифровые ряды. */
    if (sc >= 0x02 && sc <= 0x0b) return (unsigned char)digits[sc - 0x02];
    if (sc >= 0x10 && sc <= 0x19) return (unsigned char)row_qwer[sc - 0x10];
    if (sc >= 0x1e && sc <= 0x26) return (unsigned char)row_asdf[sc - 0x1e];
    if (sc >= 0x2c && sc <= 0x32) return (unsigned char)row_zxcv[sc - 0x2c];

    switch (sc) {
    case 0x01: return KEY_ESCAPE;
    case 0x0c: return KEY_MINUS;
    case 0x0d: return KEY_EQUALS;
    case 0x0e: return KEY_BACKSPACE;
    case 0x0f: return KEY_TAB;
    case 0x1a: return '[';
    case 0x1b: return ']';
    case 0x1c: return KEY_ENTER;
    case 0x1d: return KEY_FIRE;         /* левый Ctrl        */
    case 0x27: return ';';
    case 0x28: return '\'';
    case 0x29: return '`';
    case 0x2a: return KEY_RSHIFT;       /* левый Shift       */
    case 0x2b: return '\\';
    case 0x33: return ',';
    case 0x34: return '.';
    case 0x35: return '/';
    case 0x36: return KEY_RSHIFT;       /* правый Shift      */
    case 0x37: return KEYP_MULTIPLY;    /* keypad '*'        */
    case 0x38: return KEY_LALT;         /* левый Alt         */
    case 0x39: return KEY_USE;          /* пробел            */
    case 0x3a: return KEY_CAPSLOCK;
    case 0x45: return KEY_NUMLOCK;
    case 0x46: return KEY_SCRLCK;

    /* F1..F10 */
    case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44:
        return (unsigned char)(0x80 + sc);

    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;

    /* Keypad (NumLock on). */
    case 0x47: return KEYP_7;
    case 0x48: return KEYP_8;
    case 0x49: return KEYP_9;
    case 0x4a: return KEYP_MINUS;
    case 0x4b: return KEYP_4;
    case 0x4e: return KEYP_PLUS;
    case 0x4d: return KEYP_6;
    case 0x4f: return KEYP_1;
    case 0x50: return KEYP_2;
    case 0x51: return KEYP_3;
    case 0x52: return '0';
    case 0x53: return '.';

    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Окно                                                               */
/* ------------------------------------------------------------------ */

static uint32_t *g_win;
static int g_yield_guard;

static void pump_events(void)
{
    int i;

    if (!g_win)
        return;

    for (i = 0; i < 64; i++) {
        ax_event ev;
        int r = ax_poll(g_win, &ev);

        if (r <= 0)
            break;

        switch (ev.type) {
        case AX_EV_RAWKEY: {
            unsigned char key = map_scancode(ev.key & 0xFFu,
                                             (ev.buttons & 1u) != 0);
            if (key)
                key_push(key, ev.w != 0);
            break;
        }

        case AX_EV_CLOSE:
        case AX_EV_QUIT:
            exit(0);
            break;

        case AX_EV_RESIZE:
        default:
            /* Разрешение DOOM фиксировано -- игнорируем. */
            break;
        }
    }
}

void DG_Init(void)
{
    int i;

    g_win = 0;
    keyq_head = keyq_tail = 0;

    for (i = 0; i < 100 && !g_win; i++) {
        g_win = ax_surface("DOOM", WIN_W, WIN_H);
        if (!g_win)
            ax_yield();
    }

    if (!g_win) {
        dbg_puts("DOOM: no surface\n");
        exit(1);
    }

    dbg_puts("DOOM: DG_Init surface ok\n");

    ax_win_focus(g_win);
}

void DG_DrawFrame(void)
{
    int w, h, x, y;

    if (!g_win)
        return;

    w = g_cw < WIN_W ? g_cw : WIN_W;
    h = g_ch < WIN_H ? g_ch : WIN_H;

    for (y = 0; y < h; y++) {
        uint32_t *dst = g_canvas + (uint32_t)y * (uint32_t)g_cw;
        const uint32_t *src = DG_ScreenBuffer + (uint32_t)y * WIN_W;

        for (x = 0; x < w; x++)
            dst[x] = src[x] | 0xFF000000u;
    }

    ax_commit(g_win);

    pump_events();

    if ((++g_yield_guard & 1) == 0)
        ax_yield();
}

void DG_SleepMs(uint32_t ms)
{
    uint32_t start = ax_now_ms();

    for (;;) {
        pump_events();
        ax_yield();

        if ((uint32_t)(ax_now_ms() - start) >= ms)
            break;
    }
}

uint32_t DG_GetTicksMs(void)
{
    uint32_t t = ax_now_ms();

    /* Кооперативная ОС: отдаём процесс, иначе всё встанет. */
    if ((++g_yield_guard & 63) == 0) {
        pump_events();
        ax_yield();
    }

    return t;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    pump_events();

    if (keyq_tail == keyq_head)
        return 0;

    *pressed = keyq[keyq_tail].pressed;
    *key = keyq[keyq_tail].key;
    keyq_tail = (keyq_tail + 1) % KEYQ_SIZE;

    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;                /* заголовок задан при ax_surface */
}

/* Используется i_system.c (I_Error) для диагностики в COM1. */
void DG_Log(const char *msg)
{
    if (!msg)
        return;

    dbg_puts("DOOM_ERROR: ");
    dbg_puts(msg);
    dbg_puts("\n");
}

/* ------------------------------------------------------------------ */
/* Точка входа                                                        */
/* ------------------------------------------------------------------ */

static char arg0[] = "doom";
static char arg1[] = "-iwad";
static char arg2[] = "doom1.wad";
static char arg3[] = "-nosound";

static char *doom_argv[] = { arg0, arg1, arg2, arg3, 0 };

void _start(void)
{
    dbg_init();

    doomgeneric_Create(4, doom_argv);

    /*
     * doomgeneric_Tick() выполняет ОДИН тик и рисует ОДИН кадр --
     * цикл живёт здесь (так же, как в doomgeneric_xlib.c).
     */
    for (;;)
        doomgeneric_Tick();

    exit(0);
}
