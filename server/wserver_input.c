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

#include "wserver_internal.h"

/*
 * Ввод приходит сисколлами INPUT_MOUSE (190) / INPUT_KEY (191) --
 * ядро отдаёт СЫРОЕ состояние драйверов, политику (клик, фокус,
 * маршрутизация) решает сервер.  Таблицы scancode->ASCII
 * (drivers/keyboard_map.h) включены здесь: перевод символов --
 * забота WindowServer'а, как и весь ввод-вывод GUI.
 */
#include "../drivers/keyboard_map.h"

/*
 * ============================================================
 * Mouse
 * ============================================================
 */

#ifdef HOST_HARNESS
/*
 * Host harness: ввода нет -- mouse/key шимы возвращают "пусто",
 * кадр целиком определяется состоянием mouse/global из main().
 */
static int input_mouse_sys(ax_mouse_info_t *out)
{ (void)out; return -1; }

static int input_key_sys(ax_key_info_t *out)
{ (void)out; return 0; }
#else
/*
 * INPUT_MOUSE(190): ядро опрашивает свой драйвер PS/2 и
 * возвращает готовые координаты (уже клампированные) + кнопки.
 */
static int input_mouse_sys(ax_mouse_info_t *out)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)190), "D"((long)out)
        : "memory");
    return ret;
}

/* INPUT_KEY(191): одно событие клавиатуры или 0 (пусто). */
static int input_key_sys(ax_key_info_t *out)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)191), "D"((long)out)
        : "memory");
    return ret;
}
#endif /* HOST_HARNESS */

void update_mouse(void)
{
    ax_mouse_info_t mi;

    mouse.prev_x = mouse.x; mouse.prev_y = mouse.y;
    mouse.prev_left = mouse.left; mouse.prev_right = mouse.right;

    if (input_mouse_sys(&mi) != 0) {
        /* Ошибки ввода не роняем кадр -- просто держим старое. */
        return;
    }

    {
        int mx = (int)mi.x;
        int my = (int)mi.y;

        /* Страховка от координат вне кадра (см. ipc_input_mouse). */
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (scr_w > 0 && mx >= scr_w) mx = scr_w - 1;
        if (scr_h > 0 && my >= scr_h) my = scr_h - 1;

        mouse.x = mx;
        mouse.y = my;
    }
    mouse.left  = (mi.buttons & 1) != 0;
    mouse.right = (mi.buttons & 2) != 0;
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

/* Двойной клик по тайтлбару -> zoom (toggle_maximize). */
static int dbl_frame = -1000;
static int dbl_x = -1000, dbl_y = -1000;

/*
 * Window-space -> canvas-space для AX_EV_MOUSE.
 *
 * Приложение рисует и хит-тестит свои кнопки В СВОЁМ канвасе.
 * Если канвас не совпадает с клиентской областью окна (приложение
 * не пере-вызвало ax_resize после максимизации/fullscreen), контент
 * рисуется масштабированным blit'ом -- и координаты мыши обязаны
 * масштабироваться ТАК ЖЕ, иначе все кнопки внутри приложения
 * уходят мимо (баг "окна при масштабировании не реагируют").
 * Когда канвас == клиент (обычный случай) -- масштаб 1:1.
 */
static int ev_canvas_x(const ax_surface_win *w, int wx)
{
    if (w->w > 0 && w->canvas_w > 0)
        return wx * (int)w->canvas_w / w->w;
    return wx;
}

static int ev_canvas_y(const ax_surface_win *w, int wy)
{
    if (w->h > 0 && w->canvas_h > 0)
        return wy * (int)w->canvas_h / w->h;
    return wy;
}

void process_input(void)
{
    /*
     * Lion: пока открыт Launchpad или Mission Control, весь ввод
     * принадлежит оверлею -- окна и меню его не видят.
     */
    if (lion_overlay_open()) {
        if (mouse.clicked) lion_overlay_click(mouse.x, mouse.y);
        return;
    }

    /*
     * Lion fullscreen: у полноэкранного окна свой тайтлбар В ТОЙ ЖЕ
     * верхней полосе экрана, где обычно живёт меню-бар (тайтлбар
     * 28px даже ВЫШЕ бара 24px).  Пока полноэкранное окно активно,
     * хром скрыт -- и клики обязаны уходить ОКНУ, а не невидимому
     * меню-бару/доку/виджетам (баг "светофоры не работают").
     * Меню-бар отвечает только в зоне reveal (курсор у самого
     * верхнего края, y < 3 -- только там он и рисуется).
     */
    ax_surface_win *fw = surf_fullscreen_active();

    /* About dialog: click anywhere inside closes it */
    if (about_open && mouse.clicked) {
        if (mouse.x >= about_x && mouse.x < about_x + AX_ABOUT_W &&
            mouse.y >= about_y && mouse.y < about_y + AX_ABOUT_H) { about_open = false; return; }
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
        /* Menu bar clicks (в fullscreen -- только зона reveal y<3) */
        if (mouse.y < AX_MENUBAR_H && (!fw || mouse.y < 3)) {
            serial_puts_ax("MENUBAR_CLICK: x=");
            debug_putnum((uint64_t)mouse.x);
            serial_puts_ax(" y=");
            debug_putnum((uint64_t)mouse.y);
            serial_putc_ax('\n');
            /* Logo badge hit area: AX_LOGO_X (8) .. AX_LOGO_X + AX_LOGO_S (26) */
            if (mouse.x >= 8 && mouse.x < 26) {
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

        /* Dock clicks -- док СКРЫТ в fullscreen: клики идут окну */
        if (!fw && mouse.y > scr_h - AX_DOCK_H - 8) { dock_click(mouse.x, mouse.y); return; }

        /* Widget drag pickup -- виджеты СКРЫТЫ в fullscreen */
        if (!fw) {
            for (int wi = 0; wi < WIDGET_COUNT; wi++) {
                if (point_in_widget(widgets[wi], mouse.x, mouse.y)) {
                    dragging_widget = wi;
                    widgets[wi]->dragging = true;
                    widgets[wi]->drag_dx = mouse.x - widgets[wi]->x;
                    widgets[wi]->drag_dy = mouse.y - widgets[wi]->y;
                    return;
                }
            }
        }

        /* Window clicks */
        ax_surface_win *w = surf_at(mouse.x, mouse.y);
        if (w) {
            surf_focus(w);
            int lx = mouse.x - w->x;
            int ly = mouse.y - w->y;
            if (in_resize_grip(w, mouse.x, mouse.y)) {
                if (!w->fullscreen) w->resizing = true;
            } else if (ly < AX_TITLEBAR_H) {
                /* Traffic lights -- Lion geometry (AX_BTN_C1/C2/C3,
                 * radius AX_BTN_R): the click zone matches the
                 * hover zone in draw_titlebar_ex (R+2). */
                if (lx >= AX_BTN_C1 - AX_BTN_R - 2 && lx < AX_BTN_C1 + AX_BTN_R + 2) {
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
                /* Minimize button (yellow): genie-to-dock animation */
                if (lx >= AX_BTN_C2 - AX_BTN_R - 2 && lx < AX_BTN_C2 + AX_BTN_R + 2) {
                    win_minimize_anim(w);
                    return;
                }
                /*
                 * Zoom button (green): в OS X 10.7 Lion зелёный
                 * светофор -- это FULLSCREEN, а не maximize.
                 * Zoom остался по двойному клику на тайтлбаре.
                 */
                if (lx >= AX_BTN_C3 - AX_BTN_R - 2 && lx < AX_BTN_C3 + AX_BTN_R + 2) {
                    toggle_fullscreen(w);
                    return;
                }
                /*
                 * Double-click on the titlebar -> zoom (toggle_maximize),
                 * a single click starts dragging.  In fullscreen mode
                 * dragging is meaningless -- only the double-click works.
                 */
                if (ax_frame - dbl_frame < 20 &&
                    mouse.x >= dbl_x - 6 && mouse.x < dbl_x + 6 &&
                    mouse.y >= dbl_y - 6 && mouse.y < dbl_y + 6) {
                    dbl_frame = -1000;
                    if (!w->fullscreen) toggle_maximize(w);
                    return;
                }
                dbl_frame = ax_frame;
                dbl_x = mouse.x;
                dbl_y = mouse.y;

                if (!w->fullscreen) {
                    w->dragging = true;
                    w->drag_dx = mouse.x - w->x;
                    w->drag_dy = mouse.y - w->y;
                }
            } else {
                ax_event ev = {0};
                ev.type = AX_EV_MOUSE;
                ev.mx = ev_canvas_x(w, lx);
                ev.my = ev_canvas_y(w, ly - AX_TITLEBAR_H);
                ev.buttons = (uint32_t)((mouse.left ? 1 : 0) |
                                        (mouse.right ? 2 : 0));
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
     * Keyboard events: drain the driver queue via INPUT_KEY and
     * forward translated keys to the focused window.
     */
    for (;;)
    {
        ax_key_info_t ki;

        if (!input_key_sys(&ki))
            break;

        int keycode = (int)ki.key;

        int is_break = (keycode & KBE_RELEASE) != 0;
        int is_ext   = (keycode & KBE_EXT) != 0;
        int rawonly  = (keycode & KBE_RAWONLY) != 0;
        int sc       = keycode & (int)KBE_SC_MASK;

        /*
         * Системные клавиши Lion (F3 Mission Control, F4 Launchpad,
         * F11 fullscreen, Esc) перехватываются РАНЬШЕ доставки в
         * окно -- они работают даже без фокуса, как в OS X.
         */
        if (!is_break && lion_system_key(sc))
            continue;

        /*
         * Пока оверлей открыт, остальная клавиатура приложениям
         * не доставляется -- система владеет экраном.
         */
        if (lion_overlay_open())
            continue;

        if (active_surface < 0 ||
            active_surface >= AX_MAX_WINDOWS)
        {
            /* r14-fsfix: active_surface сбрасывается, когда курсор уходит
             * с окна, а вернуть его некому.  Берём окно с флагом focused. */
            int fi = -1;
            for (int i = 0; i < AX_MAX_WINDOWS; i++) {
                ax_surface_win *s = &surfaces[i];
                if (s->used && s->visible && !s->minimized && s->focused) {
                    fi = i;
                    break;
                }
            }
            if (fi < 0)
                continue;
            active_surface = fi;
        }

        ax_surface_win *w = &surfaces[active_surface];

        if (!w->used || !w->focused || !w->visible || w->minimized)
        {
            /* Спасаем фокус, потерянный при открытии сюрфейса/анимации
             * fullscreen: если окно негодно, клавиши получает
             * полноэкранное окно (оно точно видно и на переднем плане). */
            ax_surface_win *alt = surf_fullscreen_active();

            if (!alt || alt == w)
                continue;

            surf_focus(alt);
            w = alt;
        }

        /*
         * Кодировка драйвера (drivers/keyboard_map.h, KBE_*):
         * младшие 8 бит -- сканкод set 1, старшие -- флаги
         * release / extended / raw-only.  Обычное нажатие
         * обычной клавиши по-прежнему 0x00..0x7F.
         */

        /*
         * Старые спецкоды драйвера (0xF1..0xF5) -- не сканкоды
         * и существуют только в виде make.
         */
        if (!is_break && !is_ext && !rawonly && keycode > 0x7F)
        {
            ax_event sev = {0};
            sev.type = AX_EV_KEY;

            switch (keycode)
            {
                case 0xF1: sev.key = AX_KEY_CTRL_S;    break;  /* Ctrl+S */
                case 0xF4: sev.key = AX_KEY_CTRL_O;    break;  /* Ctrl+O */
                case 0xF5: sev.key = AX_KEY_CTRL_N;    break;  /* Ctrl+N */
                case 0xF2: sev.key = AX_KEY_ENTER;     break;  /* Enter   */
                case 0xF3: sev.key = AX_KEY_BACKSPACE; break;  /* Bksp    */
                default: continue;
            }

            surf_push_event(w, sev);
            continue;
        }

        /*
         * RAW-событие для игр: приходит на make И break любого
         * сканкода, включая Shift/Ctrl/Alt/CapsLock и стрелки.
         */
        {
            ax_event rev = {0};
            rev.type    = AX_EV_RAWKEY;
            rev.key     = (uint32_t)sc;
            rev.buttons = is_ext ? 1u : 0u;
            rev.w       = is_break ? 0u : 1u;
            surf_push_event(w, rev);
        }

        /*
         * Отпускания, расширенные клавиши и "raw-only" события
         * (модификаторы, Ctrl+буква) в ASCII не транслируются:
         * иначе Ctrl+S печатал бы "s" в документ.
         */
        if (is_break || is_ext || rawonly)
            continue;

        ax_event ev = {0};
        ev.type = AX_EV_KEY;

        /*
         * Plain scancode -> ASCII, honouring Shift and CapsLock
         * (модификаторы приходят из INPUT_KEY).
         */
        if (keyboard_map[sc])
        {
            unsigned char base = keyboard_map[sc];

            char ascii;
            if (ki.shift)
            {
                if (ki.caps && base >= 'a' && base <= 'z')
                    ascii = (char)base;   /* Shift+Caps+letter = lowercase */
                else
                    ascii = (char)keyboard_map_shift[sc];
            }
            else if (ki.caps)
                ascii = (char)keyboard_map_caps[sc];
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
            ev.mx = ev_canvas_x(w, mouse.x - w->x);
            ev.my = ev_canvas_y(w, mouse.y - w->y - AX_TITLEBAR_H);
            ev.buttons = (uint32_t)((mouse.left ? 1 : 0) |
                                    (mouse.right ? 2 : 0));
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
