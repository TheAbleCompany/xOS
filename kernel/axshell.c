/*
 * ============================================================
 * axshell.c
 * ============================================================
 *
 * The shell's entry point + main loop.
 *
 * After the axshell split (mirroring syscalls.c -> open.c /
 * rw.c / exec.c), this file is intentionally small: it owns
 * only the screen-size globals, the shared mouse state, the
 * serial helpers, and the compose() / axshell_main()
 * orchestration.  Everything else lives in:
 *
 *   axshell_surface.c   window manager + AX syscalls
 *   axshell_wallpaper.c wallpaper / photos / BMP loader
 *   axshell_ui.c        widgets / menubar / dock / menus / about
 *   axshell_input.c     mouse + keyboard dispatch
 *   axshell_launch.c    app launch trampoline
 *
 * Shared types + externs: see axshell_internal.h.
 */

#include "axshell_internal.h"

#include "../drivers/mouse.h"
#include "../drivers/keyboard.h"
#include "../fs/task.h"
#include "../mm/kheap.h"

/*
 * ============================================================
 * Serial helpers (used by every axshell_*.c for debug output)
 * ============================================================
 */

void serial_putc_ax(char c) { outb(0x3F8, (u8int)c); }
void serial_puts_ax(const char *s) { while (*s) serial_putc_ax(*s++); }

/*
 * ============================================================
 * Shared state
 * ============================================================
 */

int scr_w, scr_h;
ax_mouse_state mouse;

/*
 * ============================================================
 * compose -- one frame
 * ============================================================
 */

void compose(void)
{
    reap_closed_windows();
    draw_wallpaper();
    wallpaper_tick();
    draw_widgets();

    /* Draw every visible window */
    bool has_windows = false;
    int first_visible = -1;

    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *w = &surfaces[i];
        if (w->used && w->visible && !w->minimized) {
            has_windows = true;
            if (first_visible == -1) first_visible = i;
            draw_surface(w);
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

    draw_about();
    draw_menubar();
    draw_dock();
    draw_context_menu();
    draw_cursor(mouse.x, mouse.y);

    gfx_present();

    /* Launch the pending app, if any */
    if (launch_request) {
        serial_puts_ax("LAUNCH_REQUEST: ");
        serial_puts_ax(pending_launch);
        serial_putc_ax('\n');

        app_trampoline();
        launch_request = 0;
    }

    yield();
}

/*
 * ============================================================
 * Entry point
 * ============================================================
 */

void axshell_main(void)
{
    /* Init serial so debug logs are visible even before gfx */
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x80);
    outb(0x3F8, 0x03);
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x03);
    outb(0x3FA, 0xC7);
    outb(0x3FC, 0x0B);

    serial_puts_ax("AXSHELL_START\n");

    /* Query actual framebuffer size before any scaling */
    gfx_init();
    scr_w = gfx_width();
    scr_h = gfx_height();

    serial_puts_ax("SCR:");
    {
        int x = scr_w; char t[16]; int n=0; if(!x) t[n++]='0'; while(x){ t[n++]='0'+(x%10); x/=10; } while(n) serial_putc_ax(t[--n]);
        serial_putc_ax('x');
        x = scr_h; n=0; if(!x) t[n++]='0'; while(x){ t[n++]='0'+(x%10); x/=10; } while(n) serial_putc_ax(t[--n]);
        serial_putc_ax('\n');
    }

    memset((u8int*)surfaces, 0, sizeof(surfaces));
    memset((u8int*)photos,   0, sizeof(photos));

    m_cursor_x = scr_w / 2;
    m_cursor_y = scr_h / 2;

    init_menus();
    scan_photos();

    launch_about();

    while (1) {
        update_mouse();
        process_input();
        compose();
        yield();
    }
}
