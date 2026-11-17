/*
 * ============================================================
 * wserver.c
 * ============================================================
 *
 * WSERVER -- оконный сервер ArtyomX64, аналог WindowServer
 * MacOS X / NeXTSTEP (Display PostScript).
 *
 * Это ОБЫЧНЫЙ userspace ELF (wserver.bin в initrd, program_type
 * gui).  Ядро запускает его при загрузке тем же exec(), каким
 * запускаются приложения.  После старта сервер:
 *
 *   1. создаёт именованный порт "wserver" -- точку входа для
 *      всех клиентов (аналог Mach bootstrap-порта);
 *   2. запрашивает у ядра параметры дисплея (FB_INFO) и
 *      выделяет backbuffer через SHM_ALLOC;
 *   3. крутит главный цикл:
 *
 *        drain(порт "wserver")   -- клиентские запросы
 *        update_mouse()          -- INPUT_MOUSE (190)
 *        process_input()         -- маршрутизация ввода
 *        compose()               -- композитинг кадра
 *        yield()
 *
 * Канвасы окон -- разделяемая память (SHM_ALLOC): клиент рисует
 * в свой canvas сам, сервер только компонует кадр и отправляет
 * события через порты.  Никакой GUI-логики в ядре больше нет.
 *
 * История: файлы server/wserver*.c -- это бывшие kernel/axshell*.c,
 * вынесенные из ядра и переведённые с сисколлов на сообщения.
 */

#include "wserver_internal.h"

/*
 * ЕДИНСТВЕННАЕ единица трансляции с libc-обвязками (unistd.h):
 * open/read/close/exec/yield и т.д. определены здесь и видны
 * остальным файлам сервера по extern-прототипам из internal.h.
 */
#include "../lib/unistd.h"

#ifdef HOST_HARNESS
#include <stdlib.h>
#endif

#define malloc wserver_shm_malloc
#define free   wserver_shm_free

/*
 * ============================================================
 * User-side syscalls used by the server directly
 * ============================================================
 */

/*
 * Алиасы libc-обвязок для остальных файлов сервера: реальные
 * функции приходят из ../lib/unistd.h (включён выше), а в
 * wserver_internal.h объявлены имена u_*.
 */
int u_read(int fd, void *buf, unsigned long count)
{
    return read(fd, buf, count);
}

int u_write(int fd, const void *buf, unsigned long count)
{
    return write(fd, buf, count);
}

int u_open(void *path, int flags, int mode)
{
    return open(path, flags, mode);
}

int u_close(int fd)
{
    return close(fd);
}

#ifdef HOST_HARNESS
/* Host harness: обычный host malloc вместо SHM-сисколлов. */
static void *wserver_shm_malloc(unsigned long size) { return malloc(size); }
static void  wserver_shm_free(void *ptr)            { free(ptr); }
#else
static void *wserver_shm_malloc(unsigned long size)
{
    volatile unsigned long ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)183), "D"(size)
        : "memory");
    return (void *)(uintptr_t)ret;
}

static void wserver_shm_free(void *ptr)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)184), "D"((long)(uintptr_t)ptr)
        : "memory");
    (void)ret;
}
#endif /* HOST_HARNESS */

static int port_create_sys(const char *name)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)178), "D"((long)name)
        : "memory");
    return ret;
}

/*
 * ============================================================
 * Serial helpers (used by every wserver_*.c for debug output)
 * ============================================================
 */

#ifdef HOST_HARNESS
#include <stdio.h>
void serial_putc_ax(char c) { fputc(c, stderr); }
void serial_puts_ax(const char *s) { if (s) fputs(s, stderr); }
#else
void serial_putc_ax(char c) { outb(0x3F8, (u8int)c); }
void serial_puts_ax(const char *s) { while (*s) serial_putc_ax(*s++); }
#endif /* HOST_HARNESS */

/*
 * ============================================================
 * Shared state
 * ============================================================
 */

int scr_w, scr_h;
ax_mouse_state mouse;

const char *WSERVER_PORT_NAME = "wserver";
int srv_port = -1;

/*
 * ============================================================
 * compose -- one frame
 * ============================================================
 */

int ax_frame = 0;

void compose(void)
{
    ax_frame++;

    reap_closed_windows();
    draw_wallpaper();
    wallpaper_tick();

    /*
     * Lion fullscreen: пока есть видимое полноэкранное окно,
     * меню-бар, док и виджеты НЕ рисуются -- приложение занимает
     * весь экран.  Меню-бар возвращается, когда курсор касается
     * верхнего края (тот же механизм, что в OS X 10.7).
     */
    ax_surface_win *fw = surf_fullscreen_active();
    bool reveal_bar = (fw != 0 && mouse.y < 3);

    if (!fw)
        draw_widgets();

    /*
     * Draw every visible window in Z-ORDER (bottom -> top).
     * Раньше окна рисовались в порядке записи в таблице, поэтому
     * сфокусированное (и полноэкранное) окно могло оказаться
     * ПОД более старым.  Теперь порядок рендера = порядок стека.
     */
    bool has_windows = false;
    int first_visible = -1;
    {
        bool drawn[AX_MAX_WINDOWS];
        memset(drawn, 0, sizeof(drawn));

        for (;;) {
            int best = -1, bestz = 0x7FFFFFFF;
            for (int i = 0; i < AX_MAX_WINDOWS; i++) {
                ax_surface_win *w = &surfaces[i];
                if (w->used && w->visible && !w->minimized &&
                    !drawn[i] && w->z < bestz) {
                    bestz = w->z;
                    best = i;
                }
            }
            if (best < 0) break;
            drawn[best] = true;
            if (!has_windows) first_visible = best;
            has_windows = true;
            draw_surface(&surfaces[best]);
        }
    }

    if (!has_windows) {
        active_surface = -1;
        /* Reset cursor backdrop so we don't restore stale pixels */
        cursor_old_x = -1;
        cursor_old_y = -1;
    } else if (active_surface == -1 && first_visible != -1) {
        /* Active window went away -- focus the first visible one */
        surf_focus(&surfaces[first_visible]);
    }

    /* Системные оверлеи Lion (Launchpad / Mission Control)
     * рисуются поверх всего хрома, до контекстного меню. */
    bool overlay = lion_overlay_open();

    if (!overlay) {
        draw_about();
        if (!fw || reveal_bar) {
            draw_menubar();
            if (!fw) draw_dock();
        }
    }
    lion_overlays_draw();
    draw_context_menu();
    draw_cursor(mouse.x, mouse.y);

    gfx_present();

    /* Launch the pending app, if any */
    if (launch_request) {
        serial_puts_ax("WSERVER LAUNCH_REQUEST: ");
        serial_puts_ax(pending_launch);
        serial_putc_ax('\n');

        app_trampoline();
        launch_request = 0;
    }
}

/*
 * ============================================================
 * Entry point (ELF _start)
 * ============================================================
 */

void wserver_main(void)
{
    /* Init serial so debug logs are visible even before gfx */
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x80);
    outb(0x3F8, 0x03);
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x03);
    outb(0x3FA, 0xC7);
    outb(0x3FC, 0x0B);

    serial_puts_ax("WSERVER_START (userspace GUI server)\n");

    /* Query actual framebuffer size via FB_INFO before any scaling */
    gfx_init();
    scr_w = gfx_width();
    scr_h = gfx_height();

    serial_puts_ax("WSERVER SCR:");
    {
        int x = scr_w; char t[16]; int n=0; if(!x) t[n++]='0'; while(x){ t[n++]='0'+(x%10); x/=10; } while(n) serial_putc_ax(t[--n]);
        serial_putc_ax('x');
        x = scr_h; n=0; if(!x) t[n++]='0'; while(x){ t[n++]='0'+(x%10); x/=10; } while(n) serial_putc_ax(t[--n]);
        serial_putc_ax('\n');
    }

    memset((u8int*)surfaces, 0, sizeof(surfaces));
    memset((u8int*)photos,   0, sizeof(photos));

    gfx_font_selftest();   /* TrueType диагностика в COM1 */

    /*
     * Курсор: позицию ведёт драйвер мыши ядра (INPUT_MOUSE
     * возвращает уже клампированные координаты); начальное
     * значение -- центр экрана до первого движения.
     */
    mouse.x = scr_w / 2;
    mouse.y = scr_h / 2;
    mouse.prev_x = mouse.x;
    mouse.prev_y = mouse.y;

    init_menus();
    scan_photos();

    /* TEMP-TEST: launch_about(); -- приветственное окно закрывает Files */

    /*
     * Register the bootstrap port.  Клиенты находят сервер
     * через PORT_LOOKUP("wserver") -- как Mach bootstrap.
     */
    srv_port = port_create_sys(WSERVER_PORT_NAME);
    serial_puts_ax("WSERVER port 'wserver' id=");
    {
        int p = srv_port; char t[8]; int n=0; if(!p) t[n++]='0';
        while(p){ t[n++]='0'+(p%10); p/=10; } while(n) serial_putc_ax(t[--n]);
        serial_putc_ax('\n');
    }

    while (1) {
        /* 1. Клиентские запросы (surface/commit/resize/gl/...) */
        wserver_handle_requests();

        /* 2. Ввод (мышь через INPUT_MOUSE, клавиши через INPUT_KEY) */
        update_mouse();
        process_input();

        /* 3. Кадр */
        compose();

        yield();
    }
}

/*
 * mini-libc apps start at _start; сервер -- не исключение.
 * (Хост-харнесс собирается с обычным crt0: там точка входа --
 * main() из lion_harness.c, поэтому _start не нужен.)
 */
#ifndef HOST_HARNESS
void _start(void)
{
    wserver_main();

    /* Сервер не завершается; на всякий случай -- тихий выход. */
    volatile int ret;
    __asm__ volatile("int $0x80" :: "a"((long)1), "D"(0) : "memory");
    (void)ret;
    for (;;);
}
#endif /* !HOST_HARNESS */
