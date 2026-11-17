/*
 * ============================================================
 * axshell_input.c
 * ============================================================
 *
 * Mouse motion + click/keyboard dispatch.
 *
 * Reads raw mouse + keyboard state (drivers/mouse, drivers/keyboard),
 * routes clicks to the menu bar / dock / widgets / windows, and
 * pushes translated keyboard events into the focused window's
 * event queue.
 *
 * Does not own any state -- everything it touches is exported by
 * the other axshell_*.c files through axshell_internal.h.
 */

#include "axshell_internal.h"

#include "../drivers/keyboard.h"
#include "../fs/task.h"

/*
 * ============================================================
 * Mouse
 * ============================================================
 */

void update_mouse(void)
{
    handle_mouse();
    mouse.prev_x = mouse.x; mouse.prev_y = mouse.y;
    mouse.prev_left = mouse.left; mouse.prev_right = mouse.right;

    int new_x = m_cursor_x;
    int new_y = m_cursor_y;

    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if (new_x >= scr_w) new_x = scr_w - 1;
    if (new_y >= scr_h) new_y = scr_h - 1;

    mouse.x = new_x;
    mouse.y = new_y;
    mouse.left  = (mouse_buttons & 1) != 0;
    mouse.right = (mouse_buttons & 2) != 0;
    mouse.clicked  = mouse.left && !mouse.prev_left;
    mouse.released = !mouse.left && mouse.prev_left;
}

static bool in_resize_grip(ax_surface_win *w, int mx, int my)
{
    int gx = w->x + w->w - AX_RESIZE_GRIP;
    int gy = w->y + AX_TITLEBAR_H + w->h - AX_RESIZE_GRIP;
    return mx >= gx && my >= gy && mx < w->x + w->w && my < w->y + AX_TITLEBAR_H + w->h;
}

/*
 * ============================================================
 * Click + keyboard dispatch
 * ============================================================
 */

void process_input(void)
{
    /* About dialog: click anywhere inside closes it */
    if (about_open && mouse.clicked) {
        if (mouse.x >= about_x && mouse.x < about_x + 360 &&
            mouse.y >= about_y && mouse.y < about_y + 220) { about_open = false; return; }
    }

    /* Right-click opens the context menu */
    if (mouse.right && !mouse.prev_right) {
        ctx_menu_open = true; ctx_x = mouse.x; ctx_y = mouse.y;
        for (int i = 0; i < menu_count; i++) menus[i].open = false;
        open_menu = -1;
        return;
    }
    if (ctx_menu_open) { if (mouse.clicked) context_menu_click(mouse.x, mouse.y); return; }

    if (mouse.clicked) {
        /* Menu bar clicks */
        if (mouse.y < AX_MENUBAR_H) {
            serial_puts_ax("MENUBAR_CLICK: x=");
            debug_putnum((uint64_t)mouse.x);
            serial_puts_ax(" y=");
            debug_putnum((uint64_t)mouse.y);
            serial_putc_ax('\n');
            int logo_w = 22;
            if (mouse.x >= 8 && mouse.x < 8 + logo_w) {
                bool was_open = menus[0].open;
                for (int j = 0; j < menu_count; j++) menus[j].open = false;
                menus[0].open = !was_open;
                open_menu = menus[0].open ? 0 : -1;
                return;
            }
            /*
             * i starts at 1: menus[0] is the logo menu and its
             * hit area was handled above.  Clicking the empty
             * gap between menus just closes whatever is open.
             */
            for (int i = 1; i < menu_count; i++) {
                bool was_open = menus[i].open;
                menus[i].open = false;
                if (mouse.x >= menus[i].x_pos && mouse.x < menus[i].x_pos + menus[i].w) {
                    if (was_open) {
                        open_menu = -1;
                    } else {
                        for (int j = 0; j < menu_count; j++) menus[j].open = false;
                        menus[i].open = true;
                        open_menu = i;
                    }
                    return;
                }
            }
            open_menu = -1;
            return;
        }
        if (open_menu >= 0) {
            menu_click(mouse.x, mouse.y);
            return;
        }

        /* Dock clicks */
        if (mouse.y > scr_h - AX_DOCK_H - 8) { dock_click(mouse.x, mouse.y); return; }

        /* Widget drag pickup */
        for (int wi = 0; wi < WIDGET_COUNT; wi++) {
            if (point_in_widget(widgets[wi], mouse.x, mouse.y)) {
                dragging_widget = wi;
                widgets[wi]->dragging = true;
                widgets[wi]->drag_dx = mouse.x - widgets[wi]->x;
                widgets[wi]->drag_dy = mouse.y - widgets[wi]->y;
                return;
            }
        }

        /* Window clicks */
        ax_surface_win *w = surf_at(mouse.x, mouse.y);
        if (w) {
            surf_focus(w);
            int lx = mouse.x - w->x;
            int ly = mouse.y - w->y;
            if (in_resize_grip(w, mouse.x, mouse.y)) {
                w->resizing = true;
            } else if (ly < AX_TITLEBAR_H) {
                /* Close button (red) */
                if (lx >= 10 && lx <= 22) {
                    ax_event ev = {0}; ev.type = AX_EV_CLOSE;
                    surf_push_event(w, ev);
                    w->visible = false;
                    w->closing = true;

                    if (active_surface == w->id) {
                        active_surface = -1;
                        for (int i = 0; i < AX_MAX_WINDOWS; i++) {
                            if (surfaces[i].used && surfaces[i].visible) {
                                surf_focus(&surfaces[i]);
                                break;
                            }
                        }
                    }
                    return;
                }
                /* Minimize button (yellow) */
                if (lx >= 30 && lx <= 42) { w->minimized = true; return; }
                /* Maximize button (green) */
                if (lx >= 50 && lx <= 62) {
                    toggle_maximize(w);
                    return;
                }
                /* Otherwise start dragging */
                w->dragging = true;
                w->drag_dx = mouse.x - w->x;
                w->drag_dy = mouse.y - w->y;
            } else {
                ax_event ev = {0};
                ev.type = AX_EV_MOUSE;
                ev.mx = lx;
                ev.my = ly - AX_TITLEBAR_H;
                ev.buttons = mouse_buttons;
                surf_push_event(w, ev);
            }
        }
    }

    /* Drag + resize follow-through while button is held */
    if (mouse.left) {
        if (dragging_widget >= 0) {
            ax_widget_state *ws = widgets[dragging_widget];
            if (ws->dragging) {
                ws->x = mouse.x - ws->drag_dx;
                ws->y = mouse.y - ws->drag_dy;
                if (ws->x < 0) ws->x = 0;
                if (ws->y < AX_MENUBAR_H) ws->y = AX_MENUBAR_H;
                if (ws->x > scr_w - 60) ws->x = scr_w - 60;
                if (ws->y > scr_h - AX_DOCK_H - 60) ws->y = scr_h - AX_DOCK_H - 60;
            }
        }
        for (int i = 0; i < AX_MAX_WINDOWS; i++) {
            ax_surface_win *w = &surfaces[i];
            if (!w->used) continue;
            if (w->dragging) {
                w->x = mouse.x - w->drag_dx;
                w->y = mouse.y - w->drag_dy;
                if (w->y < AX_MENUBAR_H) w->y = AX_MENUBAR_H;
                if (w->x < 0) w->x = 0;
                if (w->x + w->w > scr_w) w->x = scr_w - w->w;
                if (w->y + w->h + AX_TITLEBAR_H > scr_h - AX_DOCK_H - 2)
                    w->y = scr_h - AX_DOCK_H - 2 - w->h - AX_TITLEBAR_H;
                if (w->y < AX_MENUBAR_H) w->y = AX_MENUBAR_H;
            }
            if (w->resizing) {
                w->w = mouse.x - w->x;
                w->h = mouse.y - w->y - AX_TITLEBAR_H;
                if (w->w < AX_WIN_MIN_W) w->w = AX_WIN_MIN_W;
                if (w->h < AX_WIN_MIN_H) w->h = AX_WIN_MIN_H;
                if (w->x + w->w > scr_w) w->w = scr_w - w->x;
                if (w->y + w->h + AX_TITLEBAR_H > scr_h - AX_DOCK_H - 2)
                    w->h = scr_h - AX_DOCK_H - 2 - w->y - AX_TITLEBAR_H;
                ax_event ev = {0};
                ev.type = AX_EV_RESIZE; ev.w = w->w; ev.h = w->h;
                surf_push_event(w, ev);
            }
        }
    }

    if (mouse.released) {
        dragging_widget = -1;
        for (int i = 0; i < WIDGET_COUNT; i++) widgets[i]->dragging = false;
        for (int i = 0; i < AX_MAX_WINDOWS; i++) {
            surfaces[i].dragging = false;
            surfaces[i].resizing = false;
        }
    }

    /*
     * Keyboard events: drain the keyboard event queue and forward
     * to the focused window.
     */
    for (;;)
    {
        int keycode = keyboard_pop_event();

        if (keycode < 0)
            break;

        if (active_surface < 0 ||
            active_surface >= AX_MAX_WINDOWS)
        {
            continue;
        }

        ax_surface_win *w = &surfaces[active_surface];

        if (!w->used || !w->focused)
        {
            continue;
        }

        ax_event ev = {0};
        ev.type = AX_EV_KEY;

        /* Ctrl + S */
        if (keycode == 0xF1)
        {
            ev.key = AX_KEY_CTRL_S;
            surf_push_event(w, ev);
            continue;
        }

        /* Ctrl + O */
        if (keycode == 0xF4)
        {
            ev.key = AX_KEY_CTRL_O;
            surf_push_event(w, ev);
            continue;
        }

        /* Ctrl + N */
        if (keycode == 0xF5)
        {
            ev.key = AX_KEY_CTRL_N;
            surf_push_event(w, ev);
            continue;
        }

        /* Enter */
        if (keycode == 0xF2)
        {
            ev.key = AX_KEY_ENTER;
            surf_push_event(w, ev);
            continue;
        }

        /* Backspace */
        if (keycode == 0xF3)
        {
            ev.key = AX_KEY_BACKSPACE;
            surf_push_event(w, ev);
            continue;
        }

        /*
         * Plain scancode -> ASCII, honouring Shift and CapsLock.
         *
         *   Shift            -> keyboard_map_shift
         *   CapsLock         -> keyboard_map_caps
         *   Shift + CapsLock -> letter stays lowercase (PC standard),
         *                       other symbols come from shift table
         */
        if (keycode >= 0 && keycode < 128)
        {
            unsigned char base = keyboard_map[keycode];

            char ascii;
            if (ax_shift_pressed)
            {
                if (is_caps_pressed &&
                    base >= 'a' && base <= 'z')
                    ascii = (char)base;   /* Shift+Caps+letter = lowercase */
                else
                    ascii = (char)keyboard_map_shift[keycode];
            }
            else if (is_caps_pressed)
                ascii = (char)keyboard_map_caps[keycode];
            else
                ascii = (char)base;

            if (ascii)
            {
                ev.key = (uint32_t)(unsigned char)ascii;
                surf_push_event(w, ev);
            }
        }
    }

    /* Mouse-motion events to the window under the cursor */
    if (mouse.clicked || mouse.released ||
        mouse.x != mouse.prev_x || mouse.y != mouse.prev_y)
    {
        ax_surface_win *w = surf_at(mouse.x, mouse.y);
        if (w) {
            ax_event ev = {0};
            ev.type = AX_EV_MOUSE;
            ev.mx = mouse.x - w->x;
            ev.my = mouse.y - w->y - AX_TITLEBAR_H;
            ev.buttons = mouse_buttons;
            surf_push_event(w, ev);

            serial_puts_ax("EVENT: MOUSE to ");
            serial_puts_ax(w->title);
            serial_puts_ax(" at ");
            debug_putnum(ev.mx);
            serial_putc_ax(',');
            debug_putnum(ev.my);
            serial_putc_ax('\n');
        } else {
            /* No window under cursor: clear stale focus if needed */
            if (active_surface >= 0) {
                bool found = false;
                for (int i = 0; i < AX_MAX_WINDOWS; i++) {
                    if (surfaces[i].used && surfaces[i].focused) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    active_surface = -1;
                }
            }
        }
    }
}
