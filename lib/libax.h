#ifndef LIBAX_H
#define LIBAX_H

/*
 * ============================================================
 * libax.h -- клиентская библиотека GUI ArtyomX64
 * ============================================================
 *
 * МОДЕЛЬ NeXTSTEP / MacOS X: приложения-клиенты НЕ трогают экран
 * и не вызывают GUI-сисколлы.  Все функции ниже -- тонкие
 * обёртки над ОБМЕНОМ СООБЩЕНИЯМИ с оконным сервером "wserver"
 * (аналог WindowServer):
 *
 *   ax_surface()  -- MSG_REQ_SURFACE  -> REPLY_SURFACE (канвас)
 *   ax_poll()     -- события из СВОЕГО порта (MSG_EVENT)
 *   ax_commit()   -- MSG_REQ_COMMIT (канвас изменился)
 *   ax_resize()   -- MSG_REQ_RESIZE   -> REPLY_RESIZE (новый canvas)
 *   ax_win_*()    -- MSG_REQ_WIN_CLOSE / WIN_FOCUS
 *   ax_menu_set() -- MSG_REQ_MENU_SET (wire-структура)
 *   ax_screen()   -- MSG_REQ_SCREEN   -> REPLY_SCREEN
 *   ax_time()     -- MSG_REQ_TIME     -> REPLY_TIME
 *   ax_term_*()   -- tty-сервис ядра (TERM_*) + MSG_REQ_TERM_ATTACH
 *
 * Подключение: при первом вызове клиент находит порт "wserver"
 * (PORT_LOOKUP -- аналог Mach bootstrap lookup) и создаёт свой
 * анонимный порт (PORT_CREATE) -- в него сервер шлёт события,
 * туда же приходят ответы на запросы.
 *
 * Канвас окна -- РАЗДЕЛЯЕМАЯ ПАМЯТЬ (SHM_ALLOC на стороне
 * сервера): приложение рисует в него напрямую (ax_px/ax_text/...),
 * сервер композитит кадр.  Токен окна -- младшие 32 бита адреса
 * канваса (как и в ядре раньше).
 *
 * СИГНАТУРЫ ВСЕХ ФУНКЦИЙ СОХРАНЕНЫ: исходники приложений
 * (notes, files, temu, calc, prefs, xsh) не меняются.
 */

#include <stdint.h>
#include "axipc.h"
#include "axport.h"
#include "common.h"

/*
 * Глобальные размер/указатель канваса для ax_px/ax_text/ax_fill.
 * Заполняются автоматически в ax_surface() (определения -- внизу
 * файла, в каждый бинарник, как и раньше).
 */
extern int g_cw, g_ch;
extern uint32_t *g_canvas;

/*
 * ============================================================
 * Сисколл-примитивы (kernel/ipc.c)
 * ============================================================
 */

static inline int ax_port_create(const char *name)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)178), "D"((long)name) : "memory");
    return ret;
}

static inline int ax_port_lookup(const char *name)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)179), "D"((long)name) : "memory");
    return ret;
}

static inline int ax_port_send(int port, const ax_msg_t *m)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)180), "D"((long)port), "S"((long)m) : "memory");
    return ret;
}

static inline int ax_port_recv(int port, ax_msg_t *m)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)181), "D"((long)port), "S"((long)m) : "memory");
    return ret;
}

static inline int ax_getpid(void)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)182) : "memory");
    return ret;
}

static inline int ax_term_open(void)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)185) : "memory");
    return ret;
}

static inline int ax_term_read_id(int tty, uint8_t *buf, uint32_t maxlen)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)186), "D"((long)tty), "S"((long)buf),
          "d"((long)maxlen) : "memory");
    return ret;
}

static inline int ax_term_write_id(int tty, const uint8_t *buf, uint32_t len)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)187), "D"((long)tty), "S"((long)buf),
          "d"((long)len) : "memory");
    return ret;
}

static inline int ax_task_alive(int pid)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)167), "D"((long)pid) : "memory");
    return ret;
}

static inline void ax_yield(void)
{
    volatile int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"((long)151) : "memory");
    (void)ret;
}

/*
 * ============================================================
 * Состояние подключения клиента
 * ============================================================
 */

#define WSERVER_PORT_NAME "wserver"

static int g_ax_srv_port = -1;   /* порт сервера                 */
static int g_ax_cli_port = -1;   /* СВОЙ порт (события + ответы) */
static int g_ax_pid      = -2;   /* кэш getpid                   */

/*
 * Кольцо принятых сообщений: события приходят для ВСЕХ окон
 * процесса (multi-window, модель macOS), ответы могут прийти
 * вперемешку с событиями.  ax_poll забирает события СВОЕГО
 * окна; wait_reply забирает ответ нужного типа.
 */
#define AX_RING_MAX 24
static ax_msg_t g_ax_ring[AX_RING_MAX];
static int      g_ax_ring_n = 0;

static void ax_ring_push(const ax_msg_t *m)
{
    if (g_ax_ring_n < AX_RING_MAX)
        g_ax_ring[g_ax_ring_n++] = *m;
    /* переполнение: молча теряем (как и старая ev_queue[16]) */
}

static int ax_ring_find(int type, uint32_t win, ax_msg_t *out)
{
    for (int i = 0; i < g_ax_ring_n; i++) {
        if (g_ax_ring[i].type != type) continue;
        if (win != 0 && g_ax_ring[i].win != win) continue;
        if (out) *out = g_ax_ring[i];
        /* сдвигаем хвост */
        for (int j = i; j + 1 < g_ax_ring_n; j++)
            g_ax_ring[j] = g_ax_ring[j + 1];
        g_ax_ring_n--;
        return 1;
    }
    return 0;
}

/* Вытеснить всё из своего порта в кольцо. */
static void ax_drain(void)
{
    ax_msg_t m;
    while (g_ax_cli_port > 0 &&
           ax_port_recv(g_ax_cli_port, &m) == 1) {
        ax_ring_push(&m);
    }
}

static int ax_connected(void)
{
    return g_ax_srv_port > 0 && g_ax_cli_port > 0;
}

static int ax_connect(void)
{
    if (ax_connected()) return 0;

    if (g_ax_pid < 0)
        g_ax_pid = ax_getpid();

    /*
     * Сервер может ещё не стартовать (ранние вызовы) --
     * ждём в кооперативном цикле.
     */
    for (int guard = 0; guard < 100000; guard++) {
        g_ax_srv_port = ax_port_lookup(WSERVER_PORT_NAME);
        if (g_ax_srv_port > 0) break;
        ax_yield();
    }
    if (g_ax_srv_port <= 0) return -1;

    g_ax_cli_port = ax_port_create(0);   /* анонимный */
    if (g_ax_cli_port <= 0) {
        g_ax_srv_port = -1;
        return -1;
    }

    return 0;
}

/*
 * Ждать ответ заданного типа (события аккумулируются в кольце).
 * Граница ожидания -- чтобы умерший сервер не повесил клиента.
 */
static int ax_wait_reply(int type, uint32_t win, ax_msg_t *out)
{
    if (ax_ring_find(type, win, out))
        return 0;

    for (long guard = 0; guard < 2000000; guard++) {
        ax_msg_t m;

        if (ax_port_recv(g_ax_cli_port, &m) == 1) {
            if (m.type == type && (win == 0 || m.win == win)) {
                if (out) *out = m;
                return 0;
            }
            ax_ring_push(&m);
        }

        ax_yield();
    }

    return -1;
}

static uint32_t ax_token(const void *canvas)
{
    return (uint32_t)(uintptr_t)canvas;
}

/*
 * ============================================================
 * Публичный API (сигнатуры прежние!)
 * ============================================================
 */

/*
 * ax_surface -- создать окно "title" w x h.  Возвращает
 * указатель на канвас (разделяемая память) или 0.
 * Заодно привязывает глобальные g_canvas/g_cw/g_ch для
 * 2D-примитивов ax_px/ax_text/ax_fill/ax_rect.
 */
static inline uint32_t *ax_surface(const char *title, int w, int h)
{
    g_cw = 0;
    g_ch = 0;
    g_canvas = 0;

    if (ax_connect() != 0)
        return 0;

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_SURFACE;
    m.from = (uint32_t)g_ax_cli_port;
    m.a    = (uint32_t)w;
    m.b    = (uint32_t)h;
    m.x    = (uint32_t)g_ax_pid;

    int k = 0;
    while (title && title[k] && k < 46) {
        m.data[k] = (uint8_t)title[k];
        k++;
    }
    m.data[k] = 0;
    m.len = (uint32_t)k + 1;

    if (ax_port_send(g_ax_srv_port, &m) != 0)
        return 0;

    ax_msg_t r;
    if (ax_wait_reply(MSG_REPLY_SURFACE, 0, &r) != 0)
        return 0;

    uint64_t lo = r.a;
    uint64_t hi = r.b;
    uint64_t ptr = lo | (hi << 32);
    if (!ptr)
        return 0;

    g_canvas = (uint32_t *)(uintptr_t)ptr;
    g_cw = (int)r.x;
    g_ch = (int)r.y;

    return g_canvas;
}

/*
 * ax_poll -- забрать следующее событие ОКНА canvas.
 * Возвращает 1 (есть событие), 0 (пусто), -1 (нет связи).
 *
 * События других окон процесса остаются в кольце -- модель
 * macOS: один процесс владеет многими окнами.
 */
static inline int ax_poll(uint32_t *canvas, ax_event *ev)
{
    if (!ax_connected() || !ev)
        return -1;

    uint32_t tok = ax_token(canvas);

    ax_drain();

    ax_msg_t m;
    if (!ax_ring_find(MSG_EVENT, tok, &m))
        return 0;

    ev->type    = m.a;
    ev->key     = m.b;
    ev->mx      = (int32_t)m.x;
    ev->my      = (int32_t)m.y;
    if (m.len >= 12) {
        memcpy((u8int *)&ev->buttons, (u8int *)m.data,      4);
        memcpy((u8int *)&ev->w,       (u8int *)m.data + 4,  4);
        memcpy((u8int *)&ev->h,       (u8int *)m.data + 8,  4);
    } else {
        ev->buttons = 0;
        ev->w = 0;
        ev->h = 0;
    }

    return 1;
}

/* ax_commit -- "канвас изменился" (сервер пометит окно грязным). */
static inline int ax_commit(uint32_t *canvas)
{
    if (!ax_connected()) return -1;

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_COMMIT;
    m.win  = ax_token(canvas);
    m.from = (uint32_t)g_ax_cli_port;

    return ax_port_send(g_ax_srv_port, &m);
}

/*
 * macOS-model window API.  One PROCESS may own many windows:
 * call ax_surface() again to open another one, and keep the
 * returned canvas handles in your own window list.
 *
 *   ax_win_close(canvas)  -- destroy ONE window.  The process
 *                            keeps running while it still has
 *                            other windows.  Exit the process
 *                            yourself after closing the last
 *                            one.
 *
 *   ax_win_focus(canvas)  -- bring one of your windows to the
 *                            front (typical right after
 *                            ax_surface()).
 */
static inline int ax_win_close(uint32_t *canvas)
{
    if (!ax_connected()) return -1;

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_WIN_CLOSE;
    m.win  = ax_token(canvas);
    m.from = (uint32_t)g_ax_cli_port;
    m.x    = (uint32_t)g_ax_pid;

    return ax_port_send(g_ax_srv_port, &m);
}

static inline int ax_win_focus(uint32_t *canvas)
{
    if (!ax_connected()) return -1;

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_WIN_FOCUS;
    m.win  = ax_token(canvas);
    m.from = (uint32_t)g_ax_cli_port;
    m.x    = (uint32_t)g_ax_pid;

    return ax_port_send(g_ax_srv_port, &m);
}

/*
 * ax_resize -- поменять разрешение канваса ОДНОГО своего окна.
 * Возвращает НОВЫЙ указатель канваса (старый недействителен!)
 * или 0 при отказе.  После ответа сервер пришлёт AX_EV_RESIZE.
 */
static inline uint32_t *ax_resize(uint32_t *canvas, int w, int h)
{
    if (!ax_connected()) return 0;

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_RESIZE;
    m.win  = ax_token(canvas);
    m.from = (uint32_t)g_ax_cli_port;
    m.x    = (uint32_t)g_ax_pid;
    m.a    = (uint32_t)w;
    m.b    = (uint32_t)h;

    if (ax_port_send(g_ax_srv_port, &m) != 0)
        return 0;

    ax_msg_t r;
    if (ax_wait_reply(MSG_REPLY_RESIZE, ax_token(canvas), &r) != 0)
        return 0;

    uint64_t ptr = (uint64_t)r.a | ((uint64_t)r.b << 32);
    if (!ptr)
        return 0;

    uint32_t *newc = (uint32_t *)(uintptr_t)ptr;

    if (g_canvas == canvas) {
        g_canvas = newc;
        g_cw = (int)r.x;
        g_ch = (int)r.y;
    }

    return newc;
}

/*
 * ax_menu_set -- register THIS process' global menu bar.
 *
 * macOS model: the application owns its menus.  While one of
 * the process' windows is focused, the server renders exactly
 * these menus next to <AppName>, and every click on an item
 * arrives at the focused window as
 *
 *     AX_EV_MENU,  ev.key = menu index,  ev.w = item index
 *
 * All strings are COPIED into the wire structure (и сервер
 * скопирует их ещё раз к себе), so the app may free/reuse its
 * buffers afterwards.  The [logo] and <AppName> menus (About /
 * Hide / Quit) are provided by the server.
 *
 * Returns 0 on success, -1 on bad arguments or a full table.
 */
static inline int ax_menu_set(const ax_menubar_def_t *def)
{
    if (!ax_connected() || !def)
        return -1;

    ax_menubar_wire_t wire;
    memset((u8int *)&wire, 0, sizeof(wire));

    int k = 0;
    if (def->app_name) {
        while (def->app_name[k] && k < (int)sizeof(wire.app_name) - 1) {
            wire.app_name[k] = def->app_name[k];
            k++;
        }
    }
    wire.app_name[k] = 0;

    uint32_t nmenus = def->menu_count;
    if (nmenus > AX_APP_MENUS_MAX) nmenus = AX_APP_MENUS_MAX;
    wire.menu_count = nmenus;

    for (uint32_t i = 0; i < nmenus; i++) {
        const ax_menu_def_t *src = &def->menus[i];

        k = 0;
        if (src->title) {
            while (src->title[k] && k < AX_MENU_TITLE_MAX - 1) {
                wire.menus[i].title[k] = src->title[k];
                k++;
            }
        }
        wire.menus[i].title[k] = 0;

        uint32_t nitems = src->item_count;
        if (nitems > AX_MENU_ITEMS_MAX) nitems = AX_MENU_ITEMS_MAX;
        wire.menus[i].item_count = nitems;

        for (uint32_t j = 0; j < nitems; j++) {
            const char *it = src->items ? src->items[j] : 0;
            k = 0;
            if (it) {
                while (it[k] && k < AX_MENU_ITEM_MAX - 1) {
                    wire.menus[i].items[j][k] = it[k];
                    k++;
                }
            }
            wire.menus[i].items[j][k] = 0;
        }
    }

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_MENU_SET;
    m.from = (uint32_t)g_ax_cli_port;
    m.x    = (uint32_t)g_ax_pid;
    m.len  = sizeof(wire);
    memcpy((u8int *)m.data, (u8int *)&wire, sizeof(wire));

    if (ax_port_send(g_ax_srv_port, &m) != 0)
        return -1;

    ax_msg_t r;
    if (ax_wait_reply(MSG_REPLY_OK, 0, &r) != 0)
        return -1;

    return (int32_t)r.a;
}

static inline int ax_screen(ax_screen_t *s)
{
    if (!ax_connected() || !s) return -1;

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_SCREEN;
    m.from = (uint32_t)g_ax_cli_port;

    if (ax_port_send(g_ax_srv_port, &m) != 0)
        return -1;

    ax_msg_t r;
    if (ax_wait_reply(MSG_REPLY_SCREEN, 0, &r) != 0)
        return -1;

    s->width  = r.a;
    s->height = r.b;
    return 0;
}

static inline void ax_time(ax_time_t *t)
{
    if (!t) return;

    t->year = t->month = t->day = 0;
    t->hour = t->minute = t->second = 0;

    if (!ax_connected())
        return;

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_TIME;
    m.from = (uint32_t)g_ax_cli_port;

    if (ax_port_send(g_ax_srv_port, &m) != 0)
        return;

    ax_msg_t r;
    if (ax_wait_reply(MSG_REPLY_TIME, 0, &r) != 0)
        return;

    if (r.len >= sizeof(ax_time_t))
        memcpy((u8int *)t, (u8int *)r.data, sizeof(ax_time_t));
}

/*
 * ============================================================
 * Терминальные окна (tty-сервис ядра)
 * ============================================================
 *
 * Модель как в macOS: tty -- часть "BSD"-слоя ядра, WindowServer
 * НЕ владеет терминалами.  Приложение открывает pty сисколлом
 * TERM_OPEN (ядро подключает ctty вызывающего к slave-стороне --
 * дети через exec() наследуют stdio), а серверу сообщает
 * привязку "окно -> tty" (MSG_REQ_TERM_ATTACH), чтобы при фокусе
 * окна клавиатура шла в нужный pty.
 *
 * Соответствие канвас -> tty кэшируется (до 4 терминальных окон
 * на процесс -- как и раньше, пул PTY ограничен).
 */

#define AX_TTY_MAP_MAX 4

typedef struct {
    uint32_t win;
    int      tty;
} ax_tty_ent;

static ax_tty_ent g_ax_tty_map[AX_TTY_MAP_MAX];

static int ax_tty_for(uint32_t win)
{
    for (int i = 0; i < AX_TTY_MAP_MAX; i++)
        if (g_ax_tty_map[i].win == win)
            return g_ax_tty_map[i].tty;

    /* Ленивое открытие (как раньше win_ensure_pty): */
    int tty = ax_term_open();
    if (tty < 0)
        return -1;

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_TERM_ATTACH;
    m.win  = win;
    m.from = (uint32_t)g_ax_cli_port;
    m.a    = (uint32_t)tty;
    m.x    = (uint32_t)g_ax_pid;

    for (int i = 0; i < AX_TTY_MAP_MAX; i++) {
        if (g_ax_tty_map[i].win == 0) {
            g_ax_tty_map[i].win = win;
            g_ax_tty_map[i].tty = tty;
            break;
        }
    }

    ax_port_send(g_ax_srv_port, &m);
    return tty;
}

/*
 * ax_term_claim -- "I am a terminal emulator, attach my tty to
 * this window".  Дети, запущенные через exec(), наследуют
 * controlling terminal (slave-сторону pty).
 */
static inline int ax_term_claim(uint32_t *canvas)
{
    if (!ax_connected())
        return -1;

    return ax_tty_for(ax_token(canvas));
}

/* Дренирование master-стороны pty окна (вывод детей + echo). */
static inline int ax_term_read(uint32_t *canvas, uint8_t *buf, uint32_t maxlen)
{
    if (!ax_connected())
        return -1;

    int tty = ax_tty_for(ax_token(canvas));
    if (tty < 0)
        return 0;

    return ax_term_read_id(tty, buf, maxlen);
}

/* GUI-окно кладёт нажатие в pty (line discipline -- в ядре). */
static inline int ax_term_write(uint32_t *canvas, const uint8_t *buf, uint32_t len)
{
    if (!ax_connected())
        return -1;

    int tty = ax_tty_for(ax_token(canvas));
    if (tty < 0)
        return -1;

    return ax_term_write_id(tty, buf, len);
}

/*
 * ============================================================
 * 2D-примитивы (рисуют в канвас напрямую -- разделяемая память)
 * ============================================================
 */

#define AX_ARGB(a,r,g,b) (((uint32_t)(a)<<24)|((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))
#define AX_RGB(r,g,b)    AX_ARGB(0xFF,r,g,b)

/* Определения (в каждый бинарник, см. extern выше). */
int g_cw, g_ch;
uint32_t *g_canvas;

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

/*
 * ============================================================
 * Текст: TrueType (lib/axttf.h, stb_truetype) с откатом на
 * bitmap font8x8, если шрифта в initrd нет.
 *
 *   ax_text        -- пропорциональный UI-шрифт (13px, AA),
 *   ax_char/ax_char_cp -- моно-сетка 8px (совместимость с
 *                     font8x8-раскладками: терминалы, редактор),
 *   ax_text_width  -- ширина строки (соответствует ax_text).
 * ============================================================
 */

#include "axttf.h"

static inline uint32_t ax_color_a(uint32_t c, int a)
{
    return ((uint32_t)a << 24) | (c & 0x00FFFFFFu);
}

/* src-over блендинг пикселя канваса (AX_ARGB: R в битах 16..23) */
static inline uint32_t ax_blend(uint32_t dst, uint32_t src)
{
    uint32_t a = (src >> 24) & 0xFF;
    uint32_t ia, rr, rg, rb;

    if (a == 0)   return dst;
    if (a == 255) return src | 0xFF000000u;

    ia = 255 - a;
    rr = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
    rg = (((src >> 8)  & 0xFF) * a + ((dst >> 8)  & 0xFF) * ia) / 255;
    rb = ((src & 0xFF) * a + (dst & 0xFF) * ia) / 255;
    return 0xFF000000u | (rr << 16) | (rg << 8) | rb;
}

static inline void ax_px_a(int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= g_cw || y >= g_ch)
        return;

    {
        uint32_t *p = &g_canvas[y * g_cw + x];

        *p = ax_blend(*p, c);
    }
}

/* Отрисовка альфа-битмапа глифа движка в канвас */
static inline void ax_glyph_blit(const axttf_glyph *g, int px, int py,
                                 uint32_t c)
{
    int yy, xx;

    for (yy = 0; yy < g->h; yy++) {
        const unsigned char *row = g->aa + yy * g->w;

        for (xx = 0; xx < g->w; xx++)
            if (row[xx])
                ax_px_a(px + xx, py + yy, ax_color_a(c, row[xx]));
    }
}

#include "font8x8_basic.h"
#include "font8x8_cyr.h"

/*
 * Decode one UTF-8 code point.
 *
 * Names on real disks are UTF-8 (the FAT driver converts LFN from
 * UTF-16LE), so the text renderer must not treat bytes >= 0x80 as
 * single characters: that both drew the wrong glyph and read past
 * the end of font8x8_basic (which only has 128 entries).
 */
static inline unsigned int ax_utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned int cp = s[0];

    if (cp < 0x80u) {
        *ps = (const char *)(s + 1);
        return cp;
    }
    if ((cp & 0xE0u) == 0xC0u && s[1]) {
        cp = ((cp & 0x1Fu) << 6) | (s[1] & 0x3Fu);
        *ps = (const char *)(s + 2);
        return cp < 0x80u ? 0xFFFDu : cp;
    }
    if ((cp & 0xF0u) == 0xE0u && s[1] && s[2]) {
        cp = ((cp & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
        *ps = (const char *)(s + 3);
        return cp < 0x800u ? 0xFFFDu : cp;
    }
    if ((cp & 0xF8u) == 0xF0u && s[1] && s[2] && s[3]) {
        cp = ((cp & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) |
             ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
        *ps = (const char *)(s + 4);
        return (cp < 0x10000u || cp > 0x10FFFFu) ? 0xFFFDu : cp;
    }

    /* Broken sequence: consume one byte, show a placeholder. */
    *ps = (const char *)(s + 1);
    return 0xFFFDu;
}

/* Glyph for a code point; '?' for anything the font cannot show. */
static inline const unsigned char *ax_font_glyph(unsigned int cp)
{
    if (cp < 0x80u) return font8x8_basic[cp];
    if (cp >= 0x0410u && cp <= 0x044Fu) return font8x8_cyr[cp - 0x0410u];
    if (cp == 0x0401u) return font8x8_cyr[64];
    if (cp == 0x0451u) return font8x8_cyr[65];
    return font8x8_basic[(unsigned char)'?'];
}

/*
 * ax_char_cp: моно-сетка (ячейка AXTTF_MONO_CELL x строка) --
 * глиф TrueType моно-шрифта с принудительным advance 8px.
 * Без шрифта -- классический font8x8.
 */
static inline void ax_char_cp(unsigned int cp, int x, int y, uint32_t c)
{
    if (axttf_mono_ready()) {
        const axttf_glyph *g = axttf_get_glyph_mono(cp);

        if (g) {
            ax_glyph_blit(g, x + g->xoff,
                          y + axttf_ascent(AXTTF_MONO_SIZE, 1) + g->yoff,
                          c);
            return;
        }
    }

    {
        const unsigned char *g = ax_font_glyph(cp);

        for (int cy = 0; cy < 8; cy++)
            for (int cx = 0; cx < 8; cx++)
                if (g[cy] & (1 << cx)) ax_px(x + cx, y + cy, c);
    }
}

static inline void ax_char(char ch, int x, int y, uint32_t c)
{
    ax_char_cp((unsigned char)ch, x, y, c);
}

/*
 * ax_text: пропорциональный TrueType с антиалиасингом и
 * кернингом.  y -- верх строки (глифы висят ниже, строка
 * ~AXTTF_UI_LINE px высотой).  Без шрифта -- font8x8.
 */
static inline void ax_text(const char *s, int x, int y, uint32_t c)
{
    if (axttf_ready()) {
        const int size = AXTTF_UI_SIZE;
        const int asc  = axttf_ascent(size, 0);
        int pen = x, base = y + asc, ox = x;
        unsigned prev = 0;

        while (*s) {
            unsigned cp;
            const axttf_glyph *g;

            if (*s == '\n') {
                y += AXTTF_UI_LINE;
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

            ax_glyph_blit(g, pen + g->xoff, base + g->yoff, c);
            pen += g->adv;
            prev = cp;
        }

        return;
    }

    {
        int ox = x;

        while (*s) {
            if (*s == '\n') { y += 9; x = ox; s++; continue; }
            unsigned int cp = ax_utf8_next(&s);
            ax_char_cp(cp, x, y, c);
            x += 8;
        }
    }
}

/* Width in pixels of a UTF-8 string. */
static inline int ax_text_width(const char *s)
{
    if (axttf_ready())
        return axttf_text_width(s, AXTTF_UI_SIZE);

    {
        int n = 0;

        while (*s) {
            if (*s == '\n') { s++; continue; }
            (void)ax_utf8_next(&s);
            n++;
        }
        return n * 8;
    }
}

/* Ширина строки в моно-сетке (для сеточных раскладок). */
static inline int ax_text_width_mono(const char *s)
{
    if (axttf_mono_ready())
        return axttf_text_width_mono(s);

    return ax_text_width(s);
}

#endif /* LIBAX_H */
