/*
 * ============================================================
 * axshell_surface.c
 * ============================================================
 *
 * Window manager + AX user-space syscalls.
 *
 * Owns the per-window table (surfaces[]), z-ordering, focus,
 * the per-window event queue, and the AX syscall surface that
 * user-space apps talk to (canvas alloc, poll, commit, term
 * read/write, time, screen).
 *
 * The window struct itself (ax_surface_win) and the shared
 * externs live in axshell_internal.h.
 */

#include "axshell_internal.h"

#include "../drivers/keyboard.h"
#include "../fs/fs.h"
#include "../fs/task.h"
#include "../mm/kheap.h"
#include "../dev/pty.h"

#define malloc kmalloc
#define free   kfree

/*
 * ============================================================
 * Module state
 * ============================================================
 */

ax_surface_win surfaces[AX_MAX_WINDOWS];
int z_counter = 1;
int active_surface = -1;

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
     * terminal (owns a PTY), direct keyboard feeding goes there.
     * GUI windows have no PTY at all (lazy allocation), so we just
     * keep the last terminal's PTY -- terminal apps pump keys via
     * AX_EV_KEY events anyway.
     */
    if (w->pty)
        keyboard_set_pty(w->pty);
}

/*
 * ============================================================
 * Per-window event queue
 * ============================================================
 */

void surf_push_event(ax_surface_win *w, ax_event ev)
{
    if (!w || !w->used) {
        serial_puts_ax("ERROR: push_event to null/used window\n");
        return;
    }

    int next = (w->ev_head + 1) % 16;
    if (next == w->ev_tail) {
        serial_puts_ax("ERROR: event queue full for ");
        serial_puts_ax(w->title);
        serial_putc_ax('\n');
        return;
    }

    w->ev_queue[w->ev_head] = ev;
    w->ev_head = next;

    serial_puts_ax("PUSH_EVENT: type=");
    debug_putnum(ev.type);
    serial_puts_ax(" to ");
    serial_puts_ax(w->title);
    serial_puts_ax(" head=");
    debug_putnum(w->ev_head);
    serial_puts_ax(" tail=");
    debug_putnum(w->ev_tail);
    serial_putc_ax('\n');
}

/*
 * ============================================================
 * Allocation
 * ============================================================
 */

int surf_alloc(const char *title, int w, int h, ax_app_kind kind)
{
    serial_puts_ax("SURF_ALLOC: ");
    serial_puts_ax(title);
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
        s->owner_pid = runningTask ? runningTask->pid : -1;
        s->kind = kind;
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

        /* Heap-backed canvas (was static buffer originally) */
        s->canvas = (uint32_t*)malloc(w * h * 4);
        if (!s->canvas) {
            serial_puts_ax("SURF_ALLOC_MALLOC_FAIL\n");
            s->used = false;
            return -1;
        }

        serial_puts_ax("CANVAS_ALLOC:\n");

        for (int p = 0; p < w * h; p++) {
            s->canvas[p] = ARGB(0xF0, 28, 28, 34);
        }

        int k = 0;
        while (title[k] && k < 47) {
            s->title[k] = title[k];
            k++;
        }
        s->title[k] = 0;

        /*
         * App display name for the global menu bar: taken from the
         * window title ("Notes", "XSH", "Files", ...).  All windows
         * of one process share it, so the menu always shows the
         * frontmost app.
         */
        k = 0;
        while (title[k] && k < (int)sizeof(s->app_name) - 1) {
            s->app_name[k] = title[k];
            k++;
        }
        s->app_name[k] = 0;

        s->anim = 0;

        serial_puts_ax("SURF_ALLOC_OK id=");
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
         * task is gone -- either because the app exited cleanly
         * after marking the window closing, or because it died
         * with windows still alive (crash or plain exit()).
         */
        if (w->used && !task_is_alive(w->owner_pid)) {
            serial_puts_ax("REAP_WIN: ");
            serial_puts_ax(w->title);
            serial_putc_ax('\n');

            /*
             * The process is gone -- its registered menu bar
             * must go with it (macOS model: menus belong to
             * the app, not to the shell).
             */
            menu_drop(w->owner_pid);

            if (w->canvas) { free(w->canvas); w->canvas = 0; }
            pty_free(w->pty); w->pty = 0;
            w->used = false;
            w->visible = false;
            w->closing = false;
            if (active_surface == w->id) active_surface = -1;
        }
    }
}

/*
 * Destroy ONE window immediately (the app asked for it through
 * AX_SYS_WIN_CLOSE).  The owner process keeps running.
 */
void surf_destroy(ax_surface_win *w)
{
    if (!w || !w->used) return;

    serial_puts_ax("SURF_DESTROY: ");
    serial_puts_ax(w->title);
    serial_putc_ax('\n');

    if (w->canvas) { free(w->canvas); w->canvas = 0; }
    pty_free(w->pty); w->pty = 0;
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
     * The CANVAS (w->canvas / canvas_w / canvas_h) is NOT
     * reallocated here -- it stays at the original size, and
     * draw_surface() blits it SCALED to the new window rect
     * until the owning app sees the AX_EV_RESIZE event we push
     * below and calls ax_resize() itself to grow the canvas
     * buffer.
     *
     * We don't resize the canvas from the kernel because the
     * app holds the only copy of the canvas pointer (returned
     * by ax_surface()); reallocating it would invalidate the
     * app's stored pointer without any way to communicate the
     * new one through this event.  Apps that DO want the canvas
     * to match the window size handle this in their
     * AX_EV_RESIZE handler by calling ax_resize().
     *
     * Apps that fail to call ax_resize() on AX_EV_RESIZE will
     * see blurry scaled rendering (canvas stays small) -- but
     * NOT a crash, because they keep their old canvas ptr.
     * (Previously apps would set g_cw/g_ch to ev.w/h without
     * resizing, which overran the small canvas and caused a
     * GPF + kernel heap corruption.)
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
 * Drawing
 * ============================================================
 */

static void draw_titlebar(ax_surface_win *w)
{
    uint32_t base = w->focused ? ARGB(0xCC,60,60,70) : ARGB(0xAA,45,45,52);

    gfx_rounded_rect(w->x, w->y, w->w, AX_TITLEBAR_H, 6, base);
    gfx_rounded_outline(w->x, w->y, w->w, AX_TITLEBAR_H, 6, 0x60FFFFFF);

    /* Top highlight */
    for (int j = 0; j < 6; j++) {
        for (int i = 4; i < w->w - 4; i++) {
            int px = w->x + i, py = w->y + j;
            int alpha = 0x30 - (j * 0x30 / 6);
            if (alpha < 0) alpha = 0;
            uint32_t c = gfx_get_pixel(px, py);
            if (c != 0) {
                gfx_pixel(px, py, gfx_blend(c, ((uint32_t)alpha << 24) | 0x00FFFFFF));
            }
        }
    }

    int cy = w->y + AX_TITLEBAR_H / 2;
    gfx_circle(w->x + 16, cy, 6, RGB(255,95,86));
    gfx_circle(w->x + 36, cy, 6, RGB(255,189,46));
    gfx_circle(w->x + 56, cy, 6, RGB(39,201,63));

    int tw = gfx_text_width(w->title);
    gfx_text(w->title, w->x + w->w/2 - tw/2, w->y + 10,
             w->focused ? RGB(255,255,255) : RGB(170,170,180));
}

void draw_surface(ax_surface_win *w)
{
    if (!w->visible || w->minimized) return;

    int alpha = 0xFF;
    if (w->anim < AX_FADE_FRAMES) {
        alpha = (w->anim * 0xFF) / AX_FADE_FRAMES;
        w->anim++;
    }
    /* alpha drives the fade counter above; the actual fade blit was
     * removed but the counter is still advanced for future use. */
    (void)alpha;

    /* Drop shadow */
    gfx_rounded_rect(w->x + 4, w->y + AX_TITLEBAR_H + 4, w->w, w->h, 6, 0x40000000);

    /* Outline */
    gfx_rounded_outline(w->x, w->y + AX_TITLEBAR_H, w->w, w->h, 6, 0x40FFFFFF);

    /* Always blit the canvas, even when not dirty */
    if (w->canvas) {
        if (w->w == w->canvas_w && w->h == w->canvas_h) {
            gfx_blit_argb(w->canvas, w->canvas_w, w->canvas_h, w->x + 2, w->y + AX_TITLEBAR_H + 2);
        } else {
            gfx_blit_argb_scaled(w->canvas, w->canvas_w, w->canvas_h,
                                  w->x + 2, w->y + AX_TITLEBAR_H + 2, w->w - 4, w->h - 4);
        }
        w->dirty = false;
    } else {
        serial_puts_ax("NO_CANVAS\n");
    }

    draw_titlebar(w);

    /* Resize grip */
    gfx_line(w->x + w->w - AX_RESIZE_GRIP, w->y + AX_TITLEBAR_H + w->h - 2,
             w->x + w->w - 2, w->y + AX_TITLEBAR_H + w->h - AX_RESIZE_GRIP,
             ARGB(0x70,255,255,255));
    gfx_line(w->x + w->w - AX_RESIZE_GRIP + 5, w->y + AX_TITLEBAR_H + w->h - 2,
             w->x + w->w - 2, w->y + AX_TITLEBAR_H + w->h - AX_RESIZE_GRIP + 5,
             ARGB(0x70,255,255,255));
}

/*
 * ============================================================
 * AX syscalls -- surface + terminal + poll/commit + time/screen
 * ============================================================
 */

int64_t ax_syscall_surface(const char *title, int w, int h)
{
    if (w < AX_WIN_MIN_W) w = AX_WIN_MIN_W;
    if (h < AX_WIN_MIN_H) h = AX_WIN_MIN_H;
    if (w > scr_w) w = scr_w;
    if (h > scr_h - AX_MENUBAR_H - AX_DOCK_H) h = scr_h - AX_MENUBAR_H - AX_DOCK_H;

    int id = surf_alloc(title, w, h, AX_APP_NONE);
    if (id < 0) return 0;

    /*
     * macOS model: a window has NO PTY by default.  Terminals get
     * one lazily on the first ax_term_read/ax_term_write call
     * (see below), so GUI apps with many windows never exhaust
     * the 8-slot PTY pool.
     */
    surfaces[id].pty = 0;

    return (int64_t)(uintptr_t)surfaces[id].canvas;
}

/*
 * Find the PTY of a window, allocating it lazily on first use.
 * The caller is always the owning task (a terminal emulator):
 * wire its controlling terminal to the PTY slave so children
 * spawned via exec() inherit it.
 */
static ax_pty_t *win_ensure_pty(ax_surface_win *s)
{
    if (!s->pty) {
        s->pty = pty_alloc();
        if (s->pty) {
            keyboard_set_pty(s->pty);
            if (runningTask)
                runningTask->ctty_slave = &s->pty->slave_node;
        }
    }
    return s->pty;
}

/* App drains its window's pty (child stdout + echo) */
int ax_syscall_term_read(uint32_t canvas_ptr, uint8_t *buf, uint32_t maxlen)
{
    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *s = &surfaces[i];
        if (s->used && (uint32_t)(uintptr_t)s->canvas == canvas_ptr) {
            ax_pty_t *pty = win_ensure_pty(s);
            if (!pty) return 0;
            return (int)pty->master_node.read(&pty->master_node, 0, maxlen, buf);
        }
    }
    return -1;
}

/* GUI window feeds a keystroke into its pty (line discipline
   in dev/pty.c handles echo/backspace/Enter) */
int ax_syscall_term_write(uint32_t canvas_ptr, const uint8_t *buf, uint32_t len)
{
    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *s = &surfaces[i];
        if (s->used && (uint32_t)(uintptr_t)s->canvas == canvas_ptr) {
            ax_pty_t *pty = win_ensure_pty(s);
            if (!pty) return -1;
            return (int)pty->master_node.write(&pty->master_node, 0, len, (u8int*)buf);
        }
    }
    return -1;
}

/*
 * AX_SYS_WIN_CLOSE: the app destroys ONE of its own windows.
 * The process keeps running while it still has other windows.
 */
int ax_syscall_win_close(uint32_t canvas_ptr)
{
    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *s = &surfaces[i];
        if (s->used && (uint32_t)(uintptr_t)s->canvas == canvas_ptr) {

            /* Only the owner may destroy its window. */
            if (runningTask && s->owner_pid != runningTask->pid) {
                serial_puts_ax("WIN_CLOSE: not owner\n");
                return -1;
            }

            surf_destroy(s);
            return 0;
        }
    }
    return -1;
}

/*
 * AX_SYS_WIN_FOCUS: the app brings one of its own windows to
 * the front (typical right after ax_surface()).
 */
int ax_syscall_win_focus(uint32_t canvas_ptr)
{
    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *s = &surfaces[i];
        if (s->used && (uint32_t)(uintptr_t)s->canvas == canvas_ptr) {
            if (runningTask && s->owner_pid != runningTask->pid)
                return -1;
            s->minimized = false;
            s->visible = true;
            surf_focus(s);
            return 0;
        }
    }
    return -1;
}

/*
 * AX_SYS_WIN_RESIZE -- an app changes the canvas resolution of
 * one of its own windows.  Used by the per-app "View > Resolution"
 * submenu in apps that want to switch between e.g. 640x480,
 * 800x600, 1024x768, 1280x720 at runtime.
 *
 * Flow:
 *   1. Validate caller is the owner.
 *   2. Clamp (w,h) to AX_WIN_MIN_* + screen-bounds-with-chrome.
 *   3. Allocate a NEW canvas buffer of size w*h*4.
 *   4. If allocation succeeded: free the OLD canvas, swap in the
 *      new pointer + dims.  If allocation failed: leave the old
 *      canvas intact and return 0 (NULL).
 *   5. Push AX_EV_RESIZE back to the app so it re-layouts and
 *      re-renders at the new resolution.
 *
 * The app MUST replace its stored canvas handle with the return
 * value before drawing again.  Old pointer is invalid afterwards.
 */
int64_t ax_syscall_win_resize(uint32_t canvas_ptr, int w, int h)
{
    ax_surface_win *s = 0;

    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *cand = &surfaces[i];
        if (cand->used && (uint32_t)(uintptr_t)cand->canvas == canvas_ptr) {
            s = cand;
            break;
        }
    }

    if (!s) {
        serial_puts_ax("WIN_RESIZE: canvas not found\n");
        return 0;
    }

    if (runningTask && s->owner_pid != runningTask->pid) {
        serial_puts_ax("WIN_RESIZE: not owner\n");
        return 0;
    }

    /* Clamp to sane limits (same rules as ax_syscall_surface). */
    if (w < AX_WIN_MIN_W) w = AX_WIN_MIN_W;
    if (h < AX_WIN_MIN_H) h = AX_WIN_MIN_H;
    if (w > scr_w)                                  w = scr_w;
    if (h > scr_h - AX_MENUBAR_H - AX_DOCK_H)       h = scr_h - AX_MENUBAR_H - AX_DOCK_H;

    /* Allocate the NEW canvas BEFORE freeing the old one so that
     * on allocation failure the app keeps its old canvas intact. */
    uint32_t *new_canvas = (uint32_t *)malloc((uint32_t)w * (uint32_t)h * 4);
    if (!new_canvas) {
        serial_puts_ax("WIN_RESIZE: malloc failed -- keep old canvas\n");
        return 0;
    }

    /* Pre-fill with the same dark grey the window manager uses on
     * surf_alloc() so the first frame after resize doesn't show
     * uninitialized garbage while the app is re-rendering. */
    for (int p = 0; p < w * h; p++)
        new_canvas[p] = ARGB(0xF0, 28, 28, 34);

    /* Swap: free old, install new. */
    if (s->canvas) free(s->canvas);
    s->canvas   = new_canvas;
    s->canvas_w = w;
    s->canvas_h = h;
    s->w        = w;
    s->h        = h;
    s->dirty    = true;

    /* Tell the app it needs to re-layout at the new resolution.
     * ev.w/ev.h carry the new canvas dims. */
    ax_event ev = {0};
    ev.type = AX_EV_RESIZE;
    ev.w    = (uint32_t)w;
    ev.h    = (uint32_t)h;
    surf_push_event(s, ev);

    serial_puts_ax("WIN_RESIZE: ok ");
    debug_putnum((uint64_t)w);
    serial_puts_ax("x");
    debug_putnum((uint64_t)h);
    serial_putc_ax('\n');

    return (int64_t)(uintptr_t)s->canvas;
}

int ax_syscall_poll(uint32_t canvas_ptr, ax_event *out)
{
    static int poll_count = 0;
    poll_count++;

    if (poll_count % 100 == 0) {
        serial_puts_ax("POLL: count=");
        debug_putnum(poll_count);
        serial_puts_ax(" canvas=0x");
        serial_putc_ax('\n');
    }

    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *s = &surfaces[i];
        if (!s->used) continue;

        uint32_t s_canvas = (uint32_t)(uintptr_t)s->canvas;
        if (s_canvas == canvas_ptr) {
            if (poll_count % 100 == 0) {
                serial_puts_ax("POLL: found window ");
                serial_puts_ax(s->title);
                serial_puts_ax(" head=");
                debug_putnum(s->ev_head);
                serial_puts_ax(" tail=");
                debug_putnum(s->ev_tail);
                serial_putc_ax('\n');
            }

            if (s->ev_head != s->ev_tail) {
                *out = s->ev_queue[s->ev_tail];
                s->ev_tail = (s->ev_tail + 1) % 16;

                if (poll_count % 100 == 0) {
                    serial_puts_ax("POLL: event type=");
                    debug_putnum(out->type);
                    serial_putc_ax('\n');
                }
                return 1;
            }

            out->type = AX_EV_NONE;
            return 0;
        }
    }

    if (poll_count % 100 == 0) {
        serial_puts_ax("POLL: canvas not found\n");
    }
    out->type = AX_EV_NONE;
    return -1;
}

int ax_syscall_commit(uint32_t canvas_ptr)
{
    /*
     * Just mark the surface dirty -- the shell's compose() loop is
     * the only place that draws into the backbuffer and calls
     * gfx_present().  Rendering here used to push a HALF-PAINTED
     * frame (this surface + stale background + no cursor + no
     * other windows) straight to the framebuffer, which is the
     * primary cause of window flicker: every time an app
     * committed, the cursor vanished for one frame and other
     * windows briefly showed through the wallpaper.
     *
     * compose() runs continuously in axshell_main(), so the new
     * canvas content appears on the very next composed frame
     * (typically < 16 ms later at 60 Hz).
     */
    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *s = &surfaces[i];
        if (s->used && (uint32_t)(uintptr_t)s->canvas == canvas_ptr) {
            s->dirty = true;
            return 0;
        }
    }
    return -1;
}

int ax_syscall_time(ax_time_t *out)  { ax_time_now(out); return 0; }
int ax_syscall_screen(ax_screen_t *out) { out->width = scr_w; out->height = scr_h; return 0; }

/*
 * ============================================================
 * Event routing helpers
 * ============================================================
 */

void ax_send_event_to_canvas(uint32_t canvas_ptr, ax_event *ev)
{
    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *s = &surfaces[i];
        if (s->used && (uint32_t)(uintptr_t)s->canvas == canvas_ptr) {
            surf_push_event(s, *ev);
            return;
        }
    }
}

void ax_send_event_to_active(ax_event *ev)
{
    if (active_surface >= 0 && active_surface < AX_MAX_WINDOWS) {
        ax_surface_win *s = &surfaces[active_surface];
        if (s->used && s->focused) {
            surf_push_event(s, *ev);
        }
    }
}

/*
 * macOS model helpers -- the global menu talks to the ACTIVE
 * APP, not just to one window.
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
 * every window of the owner process.  Multi-window apps close
 * all windows and exit; legacy single-window apps see QUIT on
 * their main loop and exit as well.
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

    serial_puts_ax("APP_QUIT: windows=");
    debug_putnum((uint64_t)delivered);
    serial_putc_ax('\n');
}
