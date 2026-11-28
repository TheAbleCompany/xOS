/*
 * ============================================================
 * wserver_surface.c
 * ============================================================
 *
 * Window manager + клиентские запросы WindowServer.
 *
 * Бывший kernel/axshell_surface.c.  Отличия от ядровой версии:
 *
 *   - таблица окон, z-order, фокус и канвасы живут в ПРОЦЕССЕ
 *     wserver, а не в ядре;
 *   - очередь событий ev_queue[16] заменена ПОРТОМ клиента:
 *     surf_push_event() отправляет событие сообщением MSG_EVENT
 *     через ядро (Mach-семантика), клиент вынимает его через
 *     ax_poll();
 *   - запросы приходят сообщениями на порт "wserver":
 *     MSG_REQ_SURFACE / COMMIT / WIN_CLOSE / WIN_FOCUS / RESIZE /
 *     MENU_SET / SCREEN / TIME / TERM_ATTACH / GL_*;
 *   - канвасы выделяются SHM_ALLOC (видны клиенту напрямую);
 *   - tty-сервис -- сисколлы ядра TERM_* (pty как BSD tty).
 */

#include "wserver_internal.h"

#ifdef HOST_HARNESS
/*
 * Host harness build: there is no ArtyomX64 kernel underneath.
 * Канвасы и события живут в host-памяти: SHM -> malloc, порты
 * молчат (события никуда не летят), задачи считаются живыми,
 * чтобы окна не собирались reap'ом.  Real-сисколлы ниже в #else.
 */
#include <stdlib.h>

static int port_send_sys(int port, const ax_msg_t *m)
{ (void)port; (void)m; return -1; }

static int port_recv_sys(int port, ax_msg_t *m)
{ (void)port; (void)m; return -1; }

static void *shm_alloc_sys(unsigned long size) { return malloc(size); }

static void shm_free_sys(void *p) { free(p); }

static int task_alive_sys(int pid) { (void)pid; return 1; }

static int term_focus_sys(int tty) { (void)tty; return -1; }

static int term_release_sys(int tty) { (void)tty; return -1; }

int  exec_sys(const char *path, int argc, char *argv[])
{ (void)path; (void)argc; (void)argv; return -1; }

int proc_name_sys(int pid, char *buf, int len)
{
    /* Harness: имя задачи не получить -- отдаём пустую строку. */
    (void)pid;
    if (buf && len > 0) buf[0] = 0;
    return -1;
}

#endif /* HOST_HARNESS */

/*
 * ============================================================
 * Module state
 * ============================================================
 */

ax_surface_win surfaces[AX_MAX_WINDOWS];
int z_counter = 1;
int active_surface = -1;

#ifndef HOST_HARNESS
/*
 * ============================================================
 * User-side syscall shims (порты/SHM/tty/процессы) --
 * реальные kernel-сисколлы (стабы для хост-харнесса выше).
 * ============================================================
 */

static int port_send_sys(int port, const ax_msg_t *m)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)180), "D"((long)port), "S"((long)m)
        : "memory");
    return ret;
}

static int port_recv_sys(int port, ax_msg_t *m)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)181), "D"((long)port), "S"((long)m)
        : "memory");
    return ret;
}

static void *shm_alloc_sys(unsigned long size)
{
    volatile unsigned long ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)183), "D"(size)
        : "memory");
    return (void *)(uintptr_t)ret;
}

static void shm_free_sys(void *p)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)184), "D"((long)(uintptr_t)p)
        : "memory");
    (void)ret;
}

static int task_alive_sys(int pid)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)167), "D"((long)pid)
        : "memory");
    return ret;
}

static int term_focus_sys(int tty)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)188), "D"((long)tty)
        : "memory");
    return ret;
}

static int term_release_sys(int tty)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)195), "D"((long)tty)
        : "memory");
    return ret;
}

int  exec_sys(const char *path, int argc, char *argv[])
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)11), "D"((long)path), "S"((long)argc), "d"((long)argv)
        : "memory");
    return ret;
}

int proc_name_sys(int pid, char *buf, int len)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)192), "D"((long)pid), "S"((long)buf), "d"((long)len)
        : "memory");
    return ret;
}
#endif /* HOST_HARNESS */

/*
 * ============================================================
 * Z-order + focus
 * ============================================================
 */

ax_surface_win *surf_at(int x, int y)
{
    ax_surface_win *best = 0;
    int bestz = -1;
    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *w = &surfaces[i];
        if (!w->used || !w->visible || w->minimized) continue;
        if (x >= w->x && x < w->x + w->w &&
            y >= w->y && y < w->y + w->h + AX_TITLEBAR_H) {
            if (w->z > bestz) { bestz = w->z; best = w; }
        }
    }
    return best;
}

void surf_focus(ax_surface_win *w)
{
    for (int i = 0; i < AX_MAX_WINDOWS; i++) surfaces[i].focused = false;
    w->focused = true;
    w->z = z_counter++;
    active_surface = w->id;

    /*
     * Keyboard PTY follows the focus: if the focused window is a
     * terminal (owns a tty), direct keyboard feeding goes there.
     * GUI windows have no tty at all, so we just keep the last
     * terminal's tty -- terminal apps pump keys via AX_EV_KEY
     * events anyway.
     */
    if (w->tty != AX_TTY_NONE)
        term_focus_sys(w->tty);
}

ax_surface_win *surf_by_token(uint32_t token)
{
    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *s = &surfaces[i];
        if (s->used && (uint32_t)(uintptr_t)s->canvas == token)
            return s;
    }
    return 0;
}

/*
 * ============================================================
 * Event delivery -- ЧЕРЕЗ ПОРТ КЛИЕНТА (Mach-семантика)
 * ============================================================
 */

void surf_push_event(ax_surface_win *w, ax_event ev)
{
    if (!w || !w->used || w->client_port <= 0) {
        serial_puts_ax("WSERVER: push_event to dead window\n");
        return;
    }

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_EVENT;
    m.win  = (uint32_t)(uintptr_t)w->canvas;   /* токен окна   */
    m.a    = ev.type;
    m.b    = ev.key;
    m.x    = (uint32_t)ev.mx;
    m.y    = (uint32_t)ev.my;
    m.len  = 12;
    memcpy(m.data, &ev.buttons, 4);
    memcpy(m.data + 4, &ev.w, 4);
    memcpy(m.data + 8, &ev.h, 4);

    if (port_send_sys(w->client_port, &m) != 0) {
        serial_puts_ax("WSERVER: event queue full for ");
        serial_puts_ax(w->title);
        serial_putc_ax('\n');
    }
}

/*
 * ============================================================
 * Allocation
 * ============================================================
 */

int surf_alloc(const char *title, int w, int h, int client_port)
{
    serial_puts_ax("WSERVER SURF_ALLOC: ");
    serial_puts_ax(title ? title : "?");
    serial_putc_ax('\n');

    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *s = &surfaces[i];
        if (s->used) continue;

        memset((u8int*)s, 0, sizeof(ax_surface_win));
        s->dirty = true;
        s->id = i;
        s->used = true;
        s->visible = true;
        s->closing = false;
        s->client_port = client_port;
        s->tty = AX_TTY_NONE;
        s->canvas_w = w;
        s->canvas_h = h;
        s->w = w;
        s->h = h;
        s->x = scr_w/2 - w/2 + (z_counter % 4) * 24;
        s->y = scr_h/2 - h/2 + (z_counter % 4) * 18;

        if (s->y < AX_MENUBAR_H + 4) s->y = AX_MENUBAR_H + 4;
        if (s->y + s->h + AX_TITLEBAR_H > scr_h - AX_DOCK_H - 4)
            s->y = scr_h - AX_DOCK_H - 4 - s->h - AX_TITLEBAR_H;
        if (s->y < AX_MENUBAR_H + 4) s->y = AX_MENUBAR_H + 4;

        /* Разделяемый канвас: клиент рисует в него напрямую. */
        s->canvas = (uint32_t *)shm_alloc_sys((unsigned long)w * h * 4);
        if (!s->canvas) {
            serial_puts_ax("WSERVER SURF_ALLOC_SHM_FAIL\n");
            s->used = false;
            return -1;
        }

        serial_puts_ax("WSERVER CANVAS_ALLOC\n");

        for (int p = 0; p < w * h; p++) {
            s->canvas[p] = ARGB(0xF0, 28, 28, 34);
        }

        int k = 0;
        while (title && title[k] && k < 47) {
            s->title[k] = title[k];
            k++;
        }
        s->title[k] = 0;

        /*
         * App display name for the global menu bar: taken from the
         * window title ("Notes", "XSH", "Files", ...).
         */
        k = 0;
        while (title && title[k] && k < (int)sizeof(s->app_name) - 1) {
            s->app_name[k] = title[k];
            k++;
        }
        s->app_name[k] = 0;

        s->anim = 0;
        s->anim_kind = AX_ANIM_OPEN;   /* Lion zoom-open "pop" */
        s->anim_t = 0;

        serial_puts_ax("WSERVER SURF_ALLOC_OK id=");
        char t[8];
        int n = 0;
        int x = i;
        if (x == 0) t[n++] = '0';
        while (x) { t[n++] = '0' + (x % 10); x /= 10; }
        while (n) serial_putc_ax(t[--n]);
        serial_putc_ax('\n');

        surf_focus(s);
        return i;
    }
    return -1;
}

/*
 * ============================================================
 * Close / maximize
 * ============================================================
 */

void reap_closed_windows(void)
{
    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *w = &surfaces[i];

        /*
         * macOS model: a window is orphaned as soon as its owner
         * task is gone -- crash or plain exit.
         */
        if (w->used && !task_alive_sys(w->owner_pid)) {
            serial_puts_ax("WSERVER REAP_WIN: ");
            serial_puts_ax(w->title);
            serial_putc_ax('\n');

            /* Меню процесса умирают вместе с процессом. */
            menu_drop(w->owner_pid);

            if (w->canvas) { shm_free_sys(w->canvas); w->canvas = 0; }
            if (w->tty != AX_TTY_NONE) { term_release_sys(w->tty); w->tty = AX_TTY_NONE; }
            w->used = false;
            w->visible = false;
            w->closing = false;
            if (active_surface == w->id) active_surface = -1;
        }
    }
}

/*
 * Destroy ONE window immediately (the app asked for it through
 * MSG_REQ_WIN_CLOSE).  The owner process keeps running.
 */
void surf_destroy(ax_surface_win *w)
{
    if (!w || !w->used) return;

    serial_puts_ax("WSERVER SURF_DESTROY: ");
    serial_puts_ax(w->title);
    serial_putc_ax('\n');

    if (w->canvas) { shm_free_sys(w->canvas); w->canvas = 0; }
    if (w->tty != AX_TTY_NONE) { term_release_sys(w->tty); w->tty = AX_TTY_NONE; }
    w->used = false;
    w->visible = false;
    w->closing = false;
    w->minimized = false;

    if (active_surface == w->id) {
        /* Focus the next top-most visible window. */
        int new_active = -1, highest_z = -1;
        for (int i = 0; i < AX_MAX_WINDOWS; i++) {
            if (surfaces[i].used && surfaces[i].visible &&
                !surfaces[i].minimized && surfaces[i].z > highest_z) {
                highest_z = surfaces[i].z;
                new_active = i;
            }
        }
        if (new_active >= 0)
            surf_focus(&surfaces[new_active]);
        else
            active_surface = -1;
    }
}

void toggle_maximize(ax_surface_win *w)
{
    if (!w || !w->used) return;

    /*
     * NOTE: this changes only the WINDOW geometry (w->x/y/w/h).
     * The CANVAS is NOT reallocated here -- the app sees
     * AX_EV_RESIZE and calls ax_resize() itself.
     */

    if (!w->maximized) {
        w->restore_x = w->x;
        w->restore_y = w->y;
        w->restore_w = w->w;
        w->restore_h = w->h;

        w->x = 0;
        w->y = AX_MENUBAR_H;
        w->w = scr_w;
        w->h = scr_h - AX_MENUBAR_H - AX_DOCK_H;
        w->maximized = true;
    } else {
        w->x = w->restore_x;
        w->y = w->restore_y;
        w->w = w->restore_w;
        w->h = w->restore_h;
        w->maximized = false;
    }

    ax_event ev = {0};
    ev.type = AX_EV_RESIZE;
    ev.w = w->w;
    ev.h = w->h;
    surf_push_event(w, ev);
}

/*
 * ============================================================
 * Lion fullscreen (OS X 10.7): окно занимает ВЕСЬ экран, меню-бар
 * и док исчезают (compose() их не рисует, пока есть полноэкранное
 * окно; меню-бар возвращается, когда курсор у верхнего края).
 *
 * В настоящем Lion зелёный светофор -- это и есть fullscreen:
 * toggle_maximize (zoom) остаётся доступным по двойному клику
 * на тайтлбар (wserver_input.c).
 * ============================================================
 */

void toggle_fullscreen(ax_surface_win *w)
{
    if (!w || !w->used) return;

    if (!w->fullscreen) {
        w->fs_x = w->x;
        w->fs_y = w->y;
        w->fs_w = w->w;
        w->fs_h = w->h;

        w->x = 0;
        w->y = 0;
        w->w = scr_w;
        w->h = scr_h - AX_TITLEBAR_H;   /* + тайтлбар = ровно экран */
        w->fullscreen = true;
        w->maximized = false;

        /* Lion: полноэкранное окно выезжает наверх с "pop" */
        surf_focus(w);
        w->anim_kind = AX_ANIM_OPEN;
        w->anim_t = 0;
    } else {
        w->x = w->fs_x;
        w->y = w->fs_y;
        w->w = w->fs_w;
        w->h = w->fs_h;
        w->fullscreen = false;

        w->anim_kind = AX_ANIM_OPEN;
        w->anim_t = 0;
    }

    /* Канвас пере-раскладывает само приложение (AX_EV_RESIZE). */
    ax_event ev = {0};
    ev.type = AX_EV_RESIZE;
    ev.w = w->w;
    ev.h = w->h;
    surf_push_event(w, ev);
}

/* Видимое полноэкранное окно (решает, прятать ли хром в compose). */
ax_surface_win *surf_fullscreen_active(void)
{
    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *w = &surfaces[i];
        if (w->used && w->visible && !w->minimized && w->fullscreen)
            return w;
    }
    return 0;
}

/*
 * ============================================================
 * Drawing -- Mac OS X Lion window chrome
 * ============================================================
 */

static bool mouse_in_rect(int x, int y, int w, int h)
{
    return mouse.x >= x && mouse.x < x + w && mouse.y >= y && mouse.y < y + h;
}

/* integer ease-in-out, 0..256 in and out */
static int anim_ease(int t, int frames)
{
    if (frames <= 0) return 256;
    int p = t * 256 / frames;
    if (p < 0) p = 0;
    if (p > 256) p = 256;
    if (p < 128)
        return (p * p) >> 7;
    int q = 256 - p;
    return 256 - ((q * q) >> 7);
}

static int anim_lerp(int a, int b, int e256)
{
    return a + ((b - a) * e256) / 256;
}

/*
 * The dock target of a window (for the minimize/restore genie):
 * the shelf slot of the owning app, or the middle of the dock.
 */
static void win_dock_target(const ax_surface_win *w, int *tx, int *ty)
{
    int cx = -1;
    char tname[32];
    if (proc_name_sys(w->owner_pid, tname, sizeof(tname)) == 0 && tname[0])
        cx = dock_icon_cx_by_elf(tname);

    *tx = (cx >= 0) ? cx : scr_w / 2;
    *ty = scr_h - AX_DOCK_H / 2 - 8;
}

static int isqrt_loc(int v)
{
    int res = 0, bit = 1 << 30;
    if (v <= 0) return 0;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return res;
}

/*
 * White gloss dome on the upper part of a traffic light -- the
 * :after overlay of the Lion CSS3 reconstruction (white, .4 at
 * 16%, transparent at 43% of the button height).
 */
static void gloss_dome(int cx, int cy, int r)
{
    int stop = (r * 43) / 100;
    if (stop < 1) return;
    for (int dy = 0; dy < stop; dy++) {
        int yy = cy - r + 1 + dy;
        int ddy = yy - cy;
        int half = isqrt_loc(r * r - ddy * ddy) - 2;
        if (half <= 0) continue;
        int a = 0xA0 - (0xA0 * dy) / stop;
        gfx_fill_rect_alpha(cx - half, yy, half * 2, 1,
                            ((uint32_t)a << 24) | 0x00FFFFFF);
    }
}

/*
 * Lion titlebar: brushed-metal light gradient, centered dark
 * title, Aqua traffic lights.  `tbh` is the (possibly animated)
 * titlebar height; when the window is unfocused the buttons go
 * grey, and hovering the titlebar colours them and reveals the
 * close/min/zoom glyphs exactly like 10.7.
 */
static void draw_titlebar_ex(ax_surface_win *w, int x, int y, int wid, int tbh,
                             int radius)
{
    bool hover_title = mouse_in_rect(w->x, w->y, w->w, AX_TITLEBAR_H) && !w->anim_kind;
    bool colored     = w->focused || hover_title;

    uint32_t t_top = w->focused ? LION_TITLE_F_TOP : LION_TITLE_U_TOP;
    uint32_t t_bot = w->focused ? LION_TITLE_F_BOT : LION_TITLE_U_BOT;

    /* titlebar body */
    gfx_vgradient_top_rounded(x, y, wid, tbh, radius, t_top, t_bot);

    /* top inner highlight begins after the corner curve; the bar has
     * no bottom rule -- the window toolbar continues the same surface */
    gfx_fill_rect_alpha(x + radius, y + 1, wid - radius * 2, 1,
                        ARGB(0x70,255,255,255));

    /* --- traffic lights --- */
    int cy = y + tbh / 2;

    uint32_t r_top = colored ? LION_RED_TOP : LION_GRAY_TOP;
    uint32_t r_bot = colored ? LION_RED_BOT : LION_GRAY_BOT;
    uint32_t r_rim = colored ? LION_RED_RIM : LION_GRAY_RIM;
    uint32_t y_top = colored ? LION_YEL_TOP : LION_GRAY_TOP;
    uint32_t y_bot = colored ? LION_YEL_BOT : LION_GRAY_BOT;
    uint32_t y_rim = colored ? LION_YEL_RIM : LION_GRAY_RIM;
    uint32_t g_top = colored ? LION_GRN_TOP : LION_GRAY_TOP;
    uint32_t g_bot = colored ? LION_GRN_BOT : LION_GRAY_BOT;
    uint32_t g_rim = colored ? LION_GRN_RIM : LION_GRAY_RIM;

    bool over_c1 = mouse_in_rect(x + AX_BTN_C1 - AX_BTN_R - 2, cy - AX_BTN_R - 2,
                                 (AX_BTN_R + 2) * 2, (AX_BTN_R + 2) * 2);
    bool over_c2 = mouse_in_rect(x + AX_BTN_C2 - AX_BTN_R - 2, cy - AX_BTN_R - 2,
                                 (AX_BTN_R + 2) * 2, (AX_BTN_R + 2) * 2);
    bool over_c3 = mouse_in_rect(x + AX_BTN_C3 - AX_BTN_R - 2, cy - AX_BTN_R - 2,
                                 (AX_BTN_R + 2) * 2, (AX_BTN_R + 2) * 2);

    /* darken the button the cursor is over (Lion hover cue) */
    if (over_c1) { r_top = gfx_lerp_color(r_top, RGB(0,0,0), 40); r_bot = gfx_lerp_color(r_bot, RGB(0,0,0), 40); }
    if (over_c2) { y_top = gfx_lerp_color(y_top, RGB(0,0,0), 40); y_bot = gfx_lerp_color(y_bot, RGB(0,0,0), 40); }
    if (over_c3) { g_top = gfx_lerp_color(g_top, RGB(0,0,0), 40); g_bot = gfx_lerp_color(g_bot, RGB(0,0,0), 40); }

    gfx_gloss_circle(x + AX_BTN_C1, cy, AX_BTN_R, r_top, r_bot, r_rim);
    gfx_gloss_circle(x + AX_BTN_C2, cy, AX_BTN_R, y_top, y_bot, y_rim);
    gfx_gloss_circle(x + AX_BTN_C3, cy, AX_BTN_R, g_top, g_bot, g_rim);
    /* white gloss dome on every button (the demo's :after) */
    gloss_dome(x + AX_BTN_C1, cy, AX_BTN_R);
    gloss_dome(x + AX_BTN_C2, cy, AX_BTN_R);
    gloss_dome(x + AX_BTN_C3, cy, AX_BTN_R);
    /* 1px white edge below the buttons (0 1px 0 rgba(255,255,255,.5)) */
    gfx_fill_rect_alpha(x + AX_BTN_C1 - AX_BTN_R, cy + AX_BTN_R + 1,
                        AX_BTN_R * 2, 1, ARGB(0x50,255,255,255));

    /* glyphs appear only while the cursor is over the button */
    if (over_c1) gfx_glyph(0, x + AX_BTN_C1, cy, LION_BTN_GLYPH);
    if (over_c2) gfx_glyph(1, x + AX_BTN_C2, cy, LION_BTN_GLYPH);
    if (over_c3) gfx_glyph(2, x + AX_BTN_C3, cy, LION_BTN_GLYPH);

    /* --- centered title --- */
    uint32_t tcol = w->focused ? LION_TITLE_TEXT : LION_TITLE_TEXT_U;
    int tw = gfx_text_width(w->title);
    int txp = x + wid / 2 - tw / 2;
    if (txp < x + AX_BTN_C3 + 14) txp = x + AX_BTN_C3 + 14;  /* keep off the buttons */
    int typ = y + (tbh - gfx_font_h() + 1) / 2;
    /* subtle emboss: #E7E7E7 shadow under the #3C3C3C title */
    gfx_text(w->title, txp, typ + 1, ARGB(0xC0,231,231,231));
    gfx_text(w->title, txp, typ, tcol);
}

/*
 * draw_surface with Lion window animations:
 *
 *   AX_ANIM_OPEN  -- zoom-open: the whole window scales from 88%
 *                    up to 100% around its centre (the classic
 *                    Lion "pop" when an app opens a window).
 *   AX_ANIM_MINI  -- the window shrinks toward its dock slot.
 *   AX_ANIM_RESTR -- the reverse trip out of the dock.
 *
 * All three work by computing an animated frame rect (fx, fy,
 * fw, fh) and rendering the whole chrome into it; hit-testing
 * keeps using the real rect, which is fine -- an animation lasts
 * only AX_ANIM_FRAMES frames (~0.2 s).
 */
void draw_surface(ax_surface_win *w)
{
    if (!w->visible || w->minimized) return;

    int full_w = w->w;
    int full_h = w->h + AX_TITLEBAR_H;
    int fx = w->x, fy = w->y, fw = full_w, fh = full_h;
    int tbh = AX_TITLEBAR_H;
    /* В полноэкранном режиме окно занимает весь экран -- углы квадратные,
     * как в полноэкранном macOS, иначе в углах просвечивают обои. */
    int wr = w->fullscreen ? 0 : AX_WIN_RADIUS;
    bool animating = false;

    if (w->anim_kind == AX_ANIM_OPEN) {
        int e = anim_ease(w->anim_t, AX_ANIM_FRAMES);
        int s = 225 + (31 * e) / 256;          /* 0.88 -> 1.0 */
        fw = full_w * s / 256;
        fh = full_h * s / 256;
        fx = w->x + (full_w - fw) / 2;
        fy = w->y + (full_h - fh) / 2;
        tbh = AX_TITLEBAR_H * fh / (full_h > 0 ? full_h : 1);
        if (tbh < 10) tbh = 10;
        animating = true;
        w->anim_t++;
        if (w->anim_t >= AX_ANIM_FRAMES) w->anim_kind = AX_ANIM_NONE;
    }
    else if (w->anim_kind == AX_ANIM_MINI || w->anim_kind == AX_ANIM_RESTR) {
        int tx, ty;
        win_dock_target(w, &tx, &ty);
        int tw2 = 44;
        int th2 = full_h * 44 / (full_w > 0 ? full_w : 1);
        if (th2 < 30) th2 = 30;
        if (th2 > 70) th2 = 70;

        int t = w->anim_t;
        int e = anim_ease(t, AX_ANIM_FRAMES);

        int a_x = w->x,       a_y = w->y,       a_w = full_w,      a_h = full_h;
        int b_x = tx - tw2/2, b_y = ty - th2/2, b_w = tw2,         b_h = th2;

        if (w->anim_kind == AX_ANIM_RESTR) {
            /* restore: dock -> full (e runs 0..256) */
            fx = anim_lerp(b_x, a_x, e);
            fy = anim_lerp(b_y, a_y, e);
            fw = anim_lerp(b_w, a_w, e);
            fh = anim_lerp(b_h, a_h, e);
        } else {
            /* minimize: full -> dock */
            fx = anim_lerp(a_x, b_x, e);
            fy = anim_lerp(a_y, b_y, e);
            fw = anim_lerp(a_w, b_w, e);
            fh = anim_lerp(a_h, b_h, e);
        }

        tbh = AX_TITLEBAR_H * fh / (full_h > 0 ? full_h : 1);
        if (tbh < 8) tbh = 8;
        animating = true;
        w->anim_t++;

        if (w->anim_t >= AX_ANIM_FRAMES) {
            if (w->anim_kind == AX_ANIM_MINI) {
                w->minimized = true;
            }
            w->anim_kind = AX_ANIM_NONE;
            w->anim_t = 0;
        }
    }
    else {
        w->anim = 0;   /* legacy counter idle */
    }

    /* --- soft Lion drop shadow (skipped while tiny) ---
     * two stacked passes approximate the demo's deep
     * 0 22px 70px 4px rgba(0,0,0,0.56) window shadow */
    if (!animating || fw > 120) {
        gfx_soft_shadow(fx, fy + 4,  fw, fh, 14, 5);
        gfx_soft_shadow(fx, fy + 12, fw, fh, 22, 3);
    }

    /* --- window body --- */

    /* canvas blit first; the titlebar overlays the top region.
     * Тело окна идёт вровень с заголовком (раньше был отступ 2px слева,
     * справа и снизу -- он давал ступеньку под заголовком), а нижние
     * углы срезаются тем же радиусом, что и заголовок, -- окно читается
     * как одна скруглённая форма, как в macOS. */
    if (w->canvas) {
        int bx = fx;
        int by = fy + tbh;
        int bw = fw;
        int bh = fh - tbh;
        if (bw > 0 && bh > 0) {
            gfx_blit_argb_round_bottom(w->canvas, w->canvas_w, w->canvas_h,
                                       bx, by, bw, bh, wr);
        }
        w->dirty = false;
    } else {
        serial_puts_ax("NO_CANVAS\n");
    }

    draw_titlebar_ex(w, fx, fy, fw, tbh, wr);

    /* 1px dark outline LAST: иначе заголовок накрывает рамку и
     * вылезает за скруглённые края окна */
    gfx_rounded_outline(fx, fy, fw, fh, wr, LION_FRAME_EDGE);

}

/*
 * Genie-to-dock: called by the yellow button and Hide <App>.
 * The window stays visible while the animation runs and is
 * marked minimized when it disappears into the dock.
 */
void win_minimize_anim(ax_surface_win *w)
{
    if (!w || !w->used) return;
    if (w->minimized || w->anim_kind == AX_ANIM_MINI) return;

    w->anim_kind = AX_ANIM_MINI;
    w->anim_t    = 0;
}

/*
 * ============================================================
 * macOS model helpers -- the global menu talks to the ACTIVE
 * APP, not just to one window.
 * ============================================================
 */

ax_surface_win *surf_active_window(void)
{
    if (active_surface < 0 || active_surface >= AX_MAX_WINDOWS)
        return 0;

    ax_surface_win *s = &surfaces[active_surface];

    if (!s->used || !s->focused)
        return 0;

    return s;
}

/*
 * Ask the active app to open a new window (File > New Window):
 * AX_EV_NEWWIN goes to its focused window.
 */
void app_request_newwin(void)
{
    ax_surface_win *s = surf_active_window();
    if (!s) return;

    ax_event ev = {0};
    ev.type = AX_EV_NEWWIN;
    surf_push_event(s, ev);
}

/*
 * Quit the whole app (App menu > Quit): deliver AX_EV_QUIT to
 * every window of the owner process.
 */
void app_request_quit(int owner_pid)
{
    int delivered = 0;

    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *s = &surfaces[i];
        if (s->used && s->owner_pid == owner_pid) {
            ax_event ev = {0};
            ev.type = AX_EV_QUIT;
            surf_push_event(s, ev);
            delivered++;
        }
    }

    serial_puts_ax("WSERVER APP_QUIT: windows=");
    debug_putnum((uint64_t)delivered);
    serial_putc_ax('\n');
}

/*
 * ============================================================
 * Request handlers (клиент -> сервер)
 * ============================================================
 */

static void reply_u64w(const ax_msg_t *req, int type, uint32_t lo, uint32_t hi,
                       uint32_t x, uint32_t y)
{
    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = type;
    m.win  = req->win;
    m.a = lo; m.b = hi; m.x = x; m.y = y;
    port_send_sys(req->from, &m);
}

static void reply_ok(const ax_msg_t *req, int type, int32_t rc)
{
    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = type;
    m.win  = req->win;
    m.a    = (uint32_t)rc;
    port_send_sys(req->from, &m);
}

static void h_surface(const ax_msg_t *m)
{
    char title[48];
    int w = (int)m->a, h = (int)m->b;
    int pid = (int)m->x;

    int k = 0;
    while (k < 47 && k < (int)m->len && m->data[k]) {
        title[k] = (char)m->data[k];
        k++;
    }
    title[k] = 0;

    if (w < AX_WIN_MIN_W) w = AX_WIN_MIN_W;
    if (h < AX_WIN_MIN_H) h = AX_WIN_MIN_H;
    if (w > scr_w) w = scr_w;
    if (h > scr_h - AX_MENUBAR_H - AX_DOCK_H) h = scr_h - AX_MENUBAR_H - AX_DOCK_H;

    int id = surf_alloc(title, w, h, m->from);
    if (id < 0) {
        reply_u64w(m, MSG_REPLY_SURFACE, 0, 0, 0, 0);
        return;
    }

    /*
     * macOS model: a window has NO tty by default.  Terminals
     * attach one lazily via MSG_REQ_TERM_ATTACH (ax_term_claim /
     * first ax_term_read).
     */
    surfaces[id].tty = AX_TTY_NONE;

    /* Владелец -- pid, который прислал клиент. */
    surfaces[id].owner_pid = pid;

    uint64_t ptr = (uint64_t)(uintptr_t)surfaces[id].canvas;
    reply_u64w(m, MSG_REPLY_SURFACE,
               (uint32_t)(ptr & 0xFFFFFFFFull), (uint32_t)(ptr >> 32),
               (uint32_t)surfaces[id].canvas_w, (uint32_t)surfaces[id].canvas_h);
}

static void h_commit(const ax_msg_t *m)
{
    ax_surface_win *s = surf_by_token(m->win);
    if (s) s->dirty = true;
}

static void h_win_close(const ax_msg_t *m)
{
    ax_surface_win *s = surf_by_token(m->win);
    if (!s) { return; }

    /* Only the owner may destroy its window. */
    if ((int)m->x != s->owner_pid) {
        serial_puts_ax("WSERVER WIN_CLOSE: not owner\n");
        return;
    }

    surf_destroy(s);
}

static void h_win_focus(const ax_msg_t *m)
{
    ax_surface_win *s = surf_by_token(m->win);
    if (!s) return;
    if ((int)m->x != s->owner_pid) return;

    s->minimized = false;
    s->visible = true;
    surf_focus(s);
}

static void h_resize(const ax_msg_t *m)
{
    ax_surface_win *s = surf_by_token(m->win);

    if (!s) {
        reply_u64w(m, MSG_REPLY_RESIZE, 0, 0, 0, 0);
        return;
    }

    if ((int)m->x != s->owner_pid) {
        reply_u64w(m, MSG_REPLY_RESIZE, 0, 0, 0, 0);
        return;
    }

    int w = (int)m->a, h = (int)m->b;

    /* Clamp to sane limits (same rules as surf_alloc).
     * Полноэкранное окно (Lion) имеет право на канвас во весь
     * экран: сверху только его собственный тайтлбар. */
    int max_h = s->fullscreen ? (scr_h - AX_TITLEBAR_H)
                              : (scr_h - AX_MENUBAR_H - AX_DOCK_H);
    if (w < AX_WIN_MIN_W) w = AX_WIN_MIN_W;
    if (h < AX_WIN_MIN_H) h = AX_WIN_MIN_H;
    if (w > scr_w)    w = scr_w;
    if (h > max_h)    h = max_h;

    /* Allocate the NEW canvas BEFORE freeing the old one. */
    uint32_t *new_canvas = (uint32_t *)shm_alloc_sys((unsigned long)w * h * 4);
    if (!new_canvas) {
        serial_puts_ax("WSERVER WIN_RESIZE: shm failed -- keep old canvas\n");
        reply_u64w(m, MSG_REPLY_RESIZE, 0, 0, 0, 0);
        return;
    }

    for (int p = 0; p < w * h; p++)
        new_canvas[p] = ARGB(0xF0, 28, 28, 34);

    if (s->canvas) shm_free_sys(s->canvas);
    s->canvas   = new_canvas;
    s->canvas_w = w;
    s->canvas_h = h;
    s->w        = w;
    s->h        = h;
    s->dirty    = true;

    /* Tell the app it needs to re-layout (ev.w/ev.h = new dims). */
    ax_event ev = {0};
    ev.type = AX_EV_RESIZE;
    ev.w    = (uint32_t)w;
    ev.h    = (uint32_t)h;
    surf_push_event(s, ev);

    serial_puts_ax("WSERVER WIN_RESIZE: ok\n");

    uint64_t ptr = (uint64_t)(uintptr_t)s->canvas;
    reply_u64w(m, MSG_REPLY_RESIZE,
               (uint32_t)(ptr & 0xFFFFFFFFull), (uint32_t)(ptr >> 32),
               (uint32_t)w, (uint32_t)h);
}

static void h_menu_set(const ax_msg_t *m)
{
    if (m->len < sizeof(ax_menubar_wire_t)) {
        reply_ok(m, MSG_REPLY_OK, -1);
        return;
    }

    ax_menubar_wire_t wire;
    memcpy(&wire, m->data, sizeof(wire));

    int rc = menu_set_wire((int)m->x, &wire);
    reply_ok(m, MSG_REPLY_OK, rc);
}

static void h_screen(const ax_msg_t *m)
{
    reply_u64w(m, MSG_REPLY_SCREEN, (uint32_t)scr_w, (uint32_t)scr_h, 0, 0);
}

static void h_time(const ax_msg_t *m)
{
    ax_time_t t;
    ax_time_now(&t);

    ax_msg_t r;
    memset((u8int *)&r, 0, sizeof(r));
    r.type = MSG_REPLY_TIME;
    r.win  = m->win;
    r.len  = sizeof(t);
    memcpy(r.data, &t, sizeof(t));
    port_send_sys(m->from, &r);
}

static void h_term_attach(const ax_msg_t *m)
{
    ax_surface_win *s = surf_by_token(m->win);

    if (!s || (int)m->x != s->owner_pid) {
        reply_ok(m, MSG_REPLY_OK, -1);
        return;
    }

    s->tty = (int)m->a;

    /* Терминал получил окно -- клавиатура следует за фокусом. */
    if (s->tty != AX_TTY_NONE && s->focused)
        term_focus_sys(s->tty);

    reply_ok(m, MSG_REPLY_OK, 0);
}

/*
 * ============================================================
 * AxGL: программный растеризатор живёт ТУТ (в wserver), GPU-сабмит
 * уходит ядру через GPU_SUBMIT_RT (канвас = просто буфер).
 * ============================================================
 */

static void h_gl_begin(const ax_msg_t *m)
{
    /* Размеры канваса берём у ОКНА (клиент их не дублирует). */
    ax_surface_win *s = surf_by_token(m->win);
    if (!s || !s->canvas)
        return;

    axgl_begin(m->win, s->canvas_w, s->canvas_h);
}

static void h_gl_tri(const ax_msg_t *m)
{
    ax_surface_win *s = surf_by_token(m->win);
    if (!s || !s->canvas || m->len < 3 * 28) return;

    axgl_triangles(m->win, m->data, m->len / 28);
}

static void h_gl_end(const ax_msg_t *m)
{
    ax_surface_win *s = surf_by_token(m->win);
    if (!s || !s->canvas) {
        reply_ok(m, MSG_REPLY_GL, (uint32_t)-1);
        return;
    }

    s->dirty = true;
    reply_ok(m, MSG_REPLY_GL, 0);
}

static void h_gl_backend(const ax_msg_t *m)
{
    char name[24];
    axgl_backend_name(name, sizeof(name));

    ax_msg_t r;
    memset((u8int *)&r, 0, sizeof(r));
    r.type = MSG_REPLY_GLSTR;
    r.win  = m->win;
    r.len  = 0;
    int k = 0;
    while (name[k] && k < 23) { r.data[k] = (uint8_t)name[k]; k++; }
    r.data[k] = 0;
    r.len = (uint32_t)k + 1;
    port_send_sys(m->from, &r);
}

static void h_gl_setmode(const ax_msg_t *m)
{
    int rc = axgl_set_mode((int)m->a);
    reply_ok(m, MSG_REPLY_GL, (uint32_t)rc);
}

/*
 * ============================================================
 * Dispatch + main-loop drain
 * ============================================================
 */

void wserver_dispatch(const ax_msg_t *m)
{
    switch (m->type) {
        case MSG_REQ_SURFACE:     h_surface(m);     break;
        case MSG_REQ_COMMIT:      h_commit(m);      break;
        case MSG_REQ_WIN_CLOSE:   h_win_close(m);   break;
        case MSG_REQ_WIN_FOCUS:   h_win_focus(m);   break;
        case MSG_REQ_RESIZE:      h_resize(m);      break;
        case MSG_REQ_MENU_SET:    h_menu_set(m);    break;
        case MSG_REQ_SCREEN:      h_screen(m);      break;
        case MSG_REQ_TIME:        h_time(m);        break;
        case MSG_REQ_TERM_ATTACH: h_term_attach(m); break;
        case MSG_REQ_GL_BEGIN:    h_gl_begin(m);    break;
        case MSG_REQ_GL_TRI:      h_gl_tri(m);      break;
        case MSG_REQ_GL_END:      h_gl_end(m);      break;
        case MSG_REQ_GL_BACKEND:  h_gl_backend(m);  break;
        case MSG_REQ_GL_SETMODE:  h_gl_setmode(m);  break;
        default:
            serial_puts_ax("WSERVER: unknown msg type ");
            debug_putnum(m->type);
            serial_putc_ax('\n');
            break;
    }
}

void wserver_handle_requests(void)
{
    ax_msg_t m;

    /*
     * PORT_RECV возвращает одну копию сообщения; сервер --
     * единственный получатель порта "wserver".
     */
    while (port_recv_sys(srv_port, &m) == 1) {
        wserver_dispatch(&m);
    }
}
