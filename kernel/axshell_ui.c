/*
 * ============================================================
 * axshell_ui.c
 * ============================================================
 *
 * All the chrome around the window manager:
 *
 *   - desktop widgets (clock / calendar / system monitor)
 *   - top menu bar + drop-down menus
 *   - bottom dock + dock click routing
 *   - right-click context menu
 *   - About dialog
 *   - mouse cursor renderer
 *   - shared UI helpers (RTC, calendar math, int->str, icons)
 *
 * No AX syscall lives here -- those are in axshell_surface.c.
 */

#include "axshell_internal.h"

#include "../drivers/keyboard.h"

/*
 * ============================================================
 * Module state
 * ============================================================
 */

ax_widget_state w_clock   = { 460, 30,  false, 0, 0, true };
ax_widget_state w_cal     = { 440, 130, false, 0, 0, true };
ax_widget_state w_sysmon  = { 440, 300, false, 0, 0, false };
ax_widget_state *widgets[] = { &w_clock, &w_cal, &w_sysmon };
int dragging_widget = -1;

/*
 * macOS-style global menu bar layout:
 *
 *   [logo]  <AppName>   <app-owned menus...>      ...  clock
 *
 * menus[0] is the SYSTEM menu (the logo) and menus[1] the
 * dynamic app menu of the frontmost process (About / Hide /
 * Quit) -- both are shell-owned, like the Apple menu and the
 * bold app menu in macOS.
 *
 * menus[2..] belong to the FRONTMOST APP: the app registers its
 * own menus through AX_SYS_MENU_SET (ax_menu_set()), the shell
 * only renders them and forwards clicks as AX_EV_MENU.
 * Apps that never register get the built-in File/Edit/View
 * fallback, so the bar is never empty.
 */
static char app_menu_label[24];
static char app_item_about[32];
static char app_item_hide [32];
static char app_item_quit [32];

static const char *app_menu_items[] = { app_item_about, app_item_hide, app_item_quit };

ax_menu_t menus[8];
int menu_count = 5;
int open_menu = -1;

/* Fallback menus for apps without their own registration. */
static const char *menu_file_items[]  = { "New Window", "Close Window" };
static const char *menu_edit_items[]  = { "Cut", "Copy", "Paste", "Select All" };
static const char *menu_view_items[]  = { "Toggle Widgets", "Gradient WP", "Photo WP", "Slideshow", "Next Accent" };
static const char *menu_logo_items[]  = { "Sleep", "Reboot", "Power Off", "System Info", "Program Manager", "Control Panel" };
static const char *menu_logo_label    = "X";

/*
 * Rebuild the whole menu bar from the active window:
 *
 *   menus[0]  system logo menu (shell)
 *   menus[1]  app menu: About / Hide / Quit <App> (shell)
 *   menus[2..] menus[2..] of the app's own registered bar,
 *             or the File/Edit/View fallback (shell)
 *
 * Returns false when no app is frontmost (shell desktop).
 */
static bool app_menu_refresh(void)
{
    const char *name = 0;
    ax_app_menubar_t *mb = 0;

    ax_surface_win *w = surf_active_window();
    if (w && w->app_name[0]) {
        /*
         * macOS: the bold app-name label comes from the app
         * itself (its registration), not from the window
         * title -- "Terminal" stays "Terminal" even when the
         * window is called "zsh (80x24)".
         */
        mb = menu_lookup(w->owner_pid);
        if (mb && mb->app_name[0])
            name = mb->app_name;
        else
            name = w->app_name;
    }

    if (!name) {
        menus[1].label = "Desktop";
        menus[1].items = 0;
        menus[1].item_count = 0;
        menu_count = 5;
        menus[2].label = "File"; //menus[2].items = menu_file_items; menus[2].item_count = 2;
        menus[3].label = "Edit"; menus[3].items = menu_edit_items; menus[3].item_count = 4;
        menus[4].label = "View"; menus[4].items = menu_view_items; menus[4].item_count = 5;
        return false;
    }

    int p = 0;
    while (name[p] && p < (int)sizeof(app_menu_label) - 1) {
        app_menu_label[p] = name[p];
        p++;
    }
    app_menu_label[p] = 0;

    p = 0;
    { const char *s = "About "; while (*s && p < 30) app_item_about[p++] = *s++; }
    { int q = 0; while (app_menu_label[q] && p < 30) app_item_about[p++] = app_menu_label[q++]; }
    app_item_about[p] = 0;

    p = 0;
    { const char *s = "Hide "; while (*s && p < 30) app_item_hide[p++] = *s++; }
    { int q = 0; while (app_menu_label[q] && p < 30) app_item_hide[p++] = app_menu_label[q++]; }
    app_item_hide[p] = 0;

    p = 0;
    { const char *s = "Quit "; while (*s && p < 30) app_item_quit[p++] = *s++; }
    { int q = 0; while (app_menu_label[q] && p < 30) app_item_quit[p++] = app_menu_label[q++]; }
    app_item_quit[p] = 0;

    menus[1].label = app_menu_label;
    menus[1].items = app_menu_items;
    menus[1].item_count = 3;

    /*
     * The app's own menus replace the fallback.  The labels point
     * into the kernel-side registration table, which lives as long
     * as the process does (menu_lookup drops dead entries).
     */
    if (mb) {
        menu_count = 2 + mb->menu_count;
        if (menu_count > 8) menu_count = 8;

        for (int i = 0; i < menu_count - 2; i++) {
            menus[2 + i].label = mb->menus[i].title;
            menus[2 + i].items = mb->menus[i].items;
            menus[2 + i].item_count = mb->menus[i].item_count;
        }
    } else {
        menu_count = 5;
        menus[2].label = "File"; //menus[2].items = menu_file_items; menus[2].item_count = 2;
        menus[3].label = "Edit"; menus[3].items = menu_edit_items; menus[3].item_count = 4;
        menus[4].label = "View"; menus[4].items = menu_view_items; menus[4].item_count = 5;
    }

    return true;
}

bool ctx_menu_open = false;
int  ctx_x, ctx_y;

uint32_t cursor_bg[16][16];
int cursor_old_x = -1, cursor_old_y = -1;

char prev_key_char = 0;

bool about_open = false;
int  about_x, about_y;
int  about_anim = 0;

static const uint32_t accent_cycle[] = {
    RGB(10,132,255), RGB(255,69,58), RGB(48,209,88), RGB(255,159,10), RGB(191,90,242)
};
static int accent_idx = 0;

static int fake_cpu_load = 30;
static int fake_mem_load = 45;

/*
 * ============================================================
 * Mouse cursor renderer
 * ============================================================
 */

void draw_cursor(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + 16 > scr_w) x = scr_w - 16;
    if (y + 16 > scr_h) y = scr_h - 16;

    /*
     * The cursor is now drawn straight onto the backbuffer with no
     * save/restore of the underlying pixels.  This used to be done
     * with a 16x16 backing-store ("cursor_bg") that was saved
     * before drawing and restored next frame -- but that scheme
     * breaks the moment anything else touches the backbuffer in
     * between (e.g. an app committing, a window moving, the
     * wallpaper animating).  The saved snapshot becomes stale and
     * gets pasted back over valid content, producing the classic
     * "cursor trail" / flicker artefact.
     *
     * With ax_syscall_commit() no longer pushing partial frames
     * (see axshell_surface.c) and compose() redrawing the whole
     * desktop every frame -- wallpaper first, then windows, then
     * cursor last -- the previous frame's cursor pixels are
     * naturally overwritten by the next wallpaper fill, so there
     * is nothing to restore.  No backing store needed.
     *
     * The cursor_bg[] / cursor_old_x / cursor_old_y symbols are
     * kept (other files reference them) but no longer used for
     * rendering; compose() resets cursor_old_x/y to -1 when there
     * are no windows, which is harmless.
     */
    cursor_old_x = x;
    cursor_old_y = y;

    /* Draw the arrow cursor */
    uint32_t blk = RGB(0,0,0), wht = RGB(255,255,255);
    for (int j = 0; j < 16; j++) {
        for (int i = 0; i <= j && i < 11; i++) {
            int px = x + i;
            int py = y + j;
            if (px >= 0 && px < scr_w && py >= 0 && py < scr_h) {
                if (i == 0 || i == j || j == 15) {
                    gfx_pixel(px, py, blk);
                } else {
                    gfx_pixel(px, py, wht);
                }
            }
        }
    }
}

/*
 * ============================================================
 * Icon glyphs
 * ============================================================
 */

static const uint8_t ICON_FILES[16][16] = {
    {0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,0,0,1,2,2,2,2,2,2,2,2,1,0,0,0},
    {0,0,0,1,2,2,2,2,2,2,2,2,1,0,0,0},
    {0,0,0,1,2,2,3,3,3,2,2,2,1,0,0,0},
    {0,0,0,1,2,2,2,2,2,2,2,2,1,0,0,0},
    {0,0,0,1,2,2,3,3,3,3,3,2,1,0,0,0},
    {0,0,0,1,2,2,2,2,2,2,2,2,1,0,0,0},
    {0,0,0,1,2,2,3,3,3,2,2,2,1,0,0,0},
    {0,0,0,1,2,2,2,2,2,2,2,2,1,0,0,0},
    {0,0,0,1,2,2,2,2,2,2,2,2,1,0,0,0},
    {0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

static void draw_icon(const uint8_t icon[16][16], const uint32_t *pal,
                      int x, int y, int scale)
{
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++) {
            uint8_t idx = icon[j][i];
            if (idx == 0) continue;
            gfx_fill_rect(x + i * scale, y + j * scale, scale, scale, pal[idx]);
        }
}

/*
 * ============================================================
 * RTC + calendar math
 * ============================================================
 */

static int rtc_read(int reg)
{
    outb(0x70, reg);
    return inb(0x71);
}
static int bcd2bin(int v) { return (v & 0x0F) + ((v >> 4) * 10); }

void ax_time_now(ax_time_t *t)
{
    t->second = bcd2bin(rtc_read(0x00));
    t->minute = bcd2bin(rtc_read(0x02));
    t->hour   = bcd2bin(rtc_read(0x04));
    t->day    = bcd2bin(rtc_read(0x07));
    t->month  = bcd2bin(rtc_read(0x08));
    t->year   = 2000 + bcd2bin(rtc_read(0x09));
}

static const char *month_names[] = {
    "", "January","February","March","April","May","June",
    "July","August","September","October","November","December"
};
static int days_in_month(int m, int y)
{
    int d[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && ((y%4==0 && y%100!=0) || y%400==0)) return 29;
    if (m >= 1 && m <= 12) return d[m];
    return 30;
}
static int day_of_week(int d, int m, int y)
{
    static int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

/*
 * ============================================================
 * int -> string
 * ============================================================
 */

int int_to_str(int value, char *buf, size_t len) {
    if (len == 0) return -1;

    int negative = 0;
    unsigned int n;

    if (value < 0) {
        /* Safe abs(): avoid INT_MIN overflow */
        negative = 1;
        n = (unsigned int)(-(value + 1)) + 1;
    } else {
        n = (unsigned int)value;
    }

    char *p = buf;

    do {
        if (len <= 1) return -1;
        *p++ = (char)('0' + (n % 10));
        n /= 10;
        len--;
    } while (n != 0);

    if (negative) {
        if (len <= 1) return -1;
        *p++ = '-';
    }

    *p = '\0';

    /* Reverse in place */
    char *start = buf;
    char *end = p - 1;
    while (start < end) {
        char tmp = *start;
        *start = *end;
        *end = tmp;
        start++;
        end--;
    }

    return 0;
}

/*
 * ============================================================
 * Desktop widgets
 * ============================================================
 */

static void widget_clock(int cx, int cy, int r)
{
    ax_time_t t; ax_time_now(&t);

    gfx_circle(cx + 3, cy + 3, r + 6, 0x40000000);

    gfx_circle(cx, cy, r + 6, ARGB(0x60,30,30,40));
    gfx_circle(cx, cy, r + 6, 0x40FFFFFF);
    gfx_circle(cx, cy, r, ARGB(0xE0,250,250,252));
    gfx_circle(cx, cy, r - 3, ARGB(0xFF,40,42,52));

    for (int hh = 0; hh < 12; hh++) {
        int ang = hh * 30;
        int sx = cx + (r - 6) * gfx_cos_deg(ang) / 1000;
        int sy = cy + (r - 6) * gfx_sin_deg(ang) / 1000;
        gfx_fill_rect(sx - 1, sy - 1, 3, 3, RGB(220,220,230));
    }

    int hour_ang = (t.hour % 12) * 30 + t.minute / 2 - 90;
    int min_ang  = t.minute * 6 - 90;
    int sec_ang  = t.second * 6 - 90;

    gfx_line(cx, cy, cx + (r/2) * gfx_cos_deg(hour_ang) / 1000,
                     cy + (r/2) * gfx_sin_deg(hour_ang) / 1000, RGB(255,255,255));
    gfx_line(cx, cy, cx + (r*3/4) * gfx_cos_deg(min_ang) / 1000,
                     cy + (r*3/4) * gfx_sin_deg(min_ang) / 1000, RGB(220,220,230));
    gfx_line(cx, cy, cx + (r-8) * gfx_cos_deg(sec_ang) / 1000,
                     cy + (r-8) * gfx_sin_deg(sec_ang) / 1000, accent);
    gfx_circle(cx, cy, 3, accent);
}

static void widget_calendar(int x, int y)
{
    ax_time_t t; ax_time_now(&t);
    int w = 168, h = 150;

    gfx_rounded_rect(x + 3, y + 3, w, h, 8, 0x40000000);

    gfx_rounded_rect(x, y, w, h, 8, ARGB(0x80,30,30,42));
    gfx_rounded_outline(x, y, w, h, 8, 0x40FFFFFF);

    char hdr[32]; int p = 0;
    const char *mn = month_names[t.month >= 1 && t.month <= 12 ? t.month : 1];
    while (mn[p] && p < 20) { hdr[p] = mn[p]; p++; } hdr[p] = 0;
    gfx_text(hdr, x + 12, y + 8, RGB(255,255,255));

    const char *wd = "S  M  T  W  T  F  S";
    gfx_text(wd, x + 10, y + 26, accent);

    int dow = day_of_week(1, t.month, t.year);
    int dim = days_in_month(t.month, t.year);
    int col = dow, row = 0;
    for (int d = 1; d <= dim; d++) {
        int cellx = x + 10 + col * 22;
        int celly = y + 42 + row * 16;
        if (d == t.day) {
            gfx_circle(cellx + 6, celly + 4, 9, accent);
        }
        char ds[4];
        if (d < 10) { ds[0] = '0' + d; ds[1] = 0; }
        else        { ds[0] = '0' + d/10; ds[1] = '0' + d%10; ds[2] = 0; }
        gfx_text(ds, cellx, celly, RGB(230,230,235));
        col++;
        if (col > 6) { col = 0; row++; }
    }
}

static void widget_sysmon(int x, int y)
{
    int w = 168, h = 96;

    gfx_rounded_rect(x + 3, y + 3, w, h, 8, 0x40000000);

    gfx_rounded_rect(x, y, w, h, 8, ARGB(0x80,30,40,38));
    gfx_rounded_outline(x, y, w, h, 8, 0x40FFFFFF);

    gfx_text("System Monitor", x + 12, y + 8, RGB(255,255,255));

    fake_cpu_load = (fake_cpu_load + 7) % 100;
    fake_mem_load = 40 + ((fake_mem_load + 3) % 40);

    gfx_text("CPU", x + 12, y + 30, RGB(200,200,210));
    gfx_rounded_rect(x + 50, y + 30, 100, 8, 4, ARGB(0xFF,50,50,60));
    gfx_rounded_rect(x + 50, y + 30, fake_cpu_load, 8, 4, RGB(48,209,88));

    gfx_text("MEM", x + 12, y + 52, RGB(200,200,210));
    gfx_rounded_rect(x + 50, y + 52, 100, 8, 4, ARGB(0xFF,50,50,60));
    gfx_rounded_rect(x + 50, y + 52, fake_mem_load, 8, 4, accent);

    gfx_text(AX_OS_NAME " x86_64", x + 12, y + 74, RGB(150,150,160));
}

void draw_widgets(void)
{
    /* Clock - top right */
    w_clock.x = scr_w - 120;
    w_clock.y = AX_MENUBAR_H + 20;

    /* Calendar - under clock */
    w_cal.x = scr_w - 180;
    w_cal.y = AX_MENUBAR_H + 140;

    /* System monitor - under calendar */
    w_sysmon.x = scr_w - 180;
    w_sysmon.y = AX_MENUBAR_H + 310;
    if (w_clock.visible) {
        widget_clock(w_clock.x + 50, w_clock.y + 50, 50);
    }
    if (w_cal.visible) {
        widget_calendar(w_cal.x, w_cal.y);
    }
    if (w_sysmon.visible) {
        widget_sysmon(w_sysmon.x, w_sysmon.y);
    }
}

bool point_in_widget(ax_widget_state *ws, int mx, int my)
{
    if (!ws->visible) return false;
    if (ws == &w_clock) {
        int cx = ws->x + 50, cy = ws->y + 50, r = 56;
        int dx = mx - cx, dy = my - cy;
        return dx*dx + dy*dy <= r*r;
    }
    if (ws == &w_cal)   return mx >= ws->x && mx < ws->x + 168 && my >= ws->y && my < ws->y + 150;
    if (ws == &w_sysmon) return mx >= ws->x && mx < ws->x + 168 && my >= ws->y && my < ws->y + 96;
    return false;
}

/*
 * ============================================================
 * Menu bar
 * ============================================================
 */

void init_menus(void)
{
    /*
     * menus[0] is the LOGO menu: its hit area is the fixed logo
     * box (8..30px), so it never takes part in the label reflow
     * and its label is never drawn by the menubar loop -- the
     * logo glyph is rendered separately by draw_menubar().
     * (Before the per-app menus this label was ALSO drawn by the
     * loop, which printed the "X" twice in a row.)
     */
    menus[0].label = menu_logo_label;
    menus[0].x_pos = 12;
    menus[0].w = 22;
    menus[0].open = false;
    menus[0].items = menu_logo_items;
    menus[0].item_count = 6;

    menus[1].label = "Desktop";//AX_OS_NAME;
    menus[1].x_pos = 44;
    menus[1].w = 40;
    menus[1].open = false;
    menus[1].items = 0;
    menus[1].item_count = 0;

    menus[2].label = "File";
    menus[2].x_pos = 94;
    menus[2].w = 40;
    menus[2].open = false;
    //menus[2].items = menu_file_items;
    menus[2].item_count = 2;

    menus[3].label = "Edit";
    menus[3].x_pos = 144;
    menus[3].w = 40;
    menus[3].open = false;
    menus[3].items = menu_edit_items;
    menus[3].item_count = 4;

    menus[4].label = "View";
    menus[4].x_pos = 194;
    menus[4].w = 44;
    menus[4].open = false;
    menus[4].items = menu_view_items;
    menus[4].item_count = 5;

    for (int i = 5; i < 8; i++) {
        menus[i].label = 0;
        menus[i].x_pos = 0;
        menus[i].w = 0;
        menus[i].open = false;
        menus[i].items = 0;
        menus[i].item_count = 0;
    }

    menu_count = 5;
}

static void draw_logo_glyph(int x, int y)
{
    gfx_text("X", x, y, RGB(255,255,255));
}

void draw_menubar(void)
{
    gfx_rect(0, AX_MENUBAR_H, scr_w, 2, 0x40000000);
    gfx_fill_rect(0, 0, scr_w, AX_MENUBAR_H, ARGB(0xB0,20,20,26));

    /*
     * Rebuild the dynamic app menu and reflow the whole bar:
     * x positions are computed from the actual label widths so
     * any app name and any app-owned menu set fits (macOS
     * reflows the same way).
     */
    app_menu_refresh();

    /*
     * Reflow starts AFTER the logo box: menus[0] is the logo
     * menu and must not be drawn as a text label here (that
     * used to duplicate the "X" glyph).
     */
    int x = 44;
    for (int i = 1; i < menu_count; i++) {
        menus[i].x_pos = x;
        menus[i].w = menus[i].label ? gfx_text_width(menus[i].label) + 12 : 40;
        x = menus[i].x_pos + menus[i].w + 4;
    }

    int logo_w = 22;
    bool logo_hover = mouse.x >= 8 && mouse.x < 8 + logo_w && mouse.y >= 0 && mouse.y < AX_MENUBAR_H;
    if (logo_hover || menus[0].open)
        gfx_fill_rect_alpha(8 - 4, 2, logo_w + 8, AX_MENUBAR_H - 4, accent & 0x40FFFFFF);
    draw_logo_glyph(12, 8);

    bool app_active = (menus[1].items != 0);

    for (int i = 0; i < menu_count; i++) {
        bool hover = i != 0 &&
                     mouse.x >= menus[i].x_pos && mouse.x < menus[i].x_pos + menus[i].w &&
                     mouse.y >= 0 && mouse.y < AX_MENUBAR_H;
        if (menus[i].open || (hover && i!=0))
            gfx_fill_rect_alpha(menus[i].x_pos - 4, 2, menus[i].w + 8, AX_MENUBAR_H - 4,
                                accent & 0x40FFFFFF);

        uint32_t color = (menus[i].open || hover) ? RGB(255,255,255) : RGB(210,210,220);

        /*
         * The frontmost app's name takes the accent color -- the
         * macOS "bold app menu" cue.
         */
        if (i == 1 && app_active && !menus[i].open && !hover)
            color = accent;

        /* The logo menu's label is the glyph drawn above. */
        if (i != 0 && menus[i].label)
            gfx_text(menus[i].label, menus[i].x_pos, 8, color);

        if (menus[i].open && menus[i].items) {
            int mw = 160;
            int mh = menus[i].item_count * 22 + 8;
            int mx0 = menus[i].x_pos - 4;
            int my0 = AX_MENUBAR_H;

            if (mx0 + mw > scr_w) mx0 = scr_w - mw - 2;

            gfx_rounded_rect(mx0 + 3, my0 + 3, mw, mh, 6, 0x40000000);
            gfx_rounded_rect(mx0, my0, mw, mh, 6, ARGB(0xE0,40,40,48));
            gfx_rounded_outline(mx0, my0, mw, mh, 6, 0x60FFFFFF);

            for (int j = 0; j < menus[i].item_count; j++) {
                int iy = my0 + 4 + j * 22;
                if (mouse.x >= mx0 && mouse.x < mx0 + mw &&
                    mouse.y >= iy && mouse.y < iy + 22 && i!=0)
                    gfx_fill_rect_alpha(mx0 + 4, iy, mw - 8, 22, accent & 0x60FFFFFF);
                gfx_text(menus[i].items[j], mx0 + 10, iy + 7, RGB(230,230,240));
            }
        }
    }

    ax_time_t t; ax_time_now(&t);
    char clk[16];
    clk[0] = '0' + t.hour/10; clk[1] = '0' + t.hour%10; clk[2] = ':';
    clk[3] = '0' + t.minute/10; clk[4] = '0' + t.minute%10;
    clk[5] = 0;
    gfx_text(clk, scr_w - 60, 8, RGB(255,255,255));

    char buf[16];
    int_to_str(get_battery_percentage_fast(), buf, sizeof(buf));
    gfx_text(buf, scr_w - 120, 8, RGB(255,255,255));
}

/*
 * ============================================================
 * Power
 * ============================================================
 */

void system_power_sleep(void){ /*__asm__ volatile("hlt");*/ }
void reboot(void) { outb(0x64, 0xFE); /*for(;;) __asm__ volatile("hlt");*/ }
void system_power_off(void){
    outw(0xB004, 0x2000);
    outw(0x604, 0x2000);
    outw(0x4004, 0x3400);
    /* ASUS */
    outw(0x0404, 0x3C00);
}

/*
 * ============================================================
 * Menu click routing
 * ============================================================
 */

void menu_click(int mx, int my)
{
    serial_puts_ax("MENU_CLICK: menu=");
    debug_putnum((uint64_t)open_menu);
    serial_puts_ax(" x=");
    debug_putnum((uint64_t)mx);
    serial_putc_ax('\n');
    if (open_menu < 0) return;
    if (open_menu >= menu_count) return;
    ax_menu_t *m = &menus[open_menu];
    int mw = 160;
    int mh = (m->items ? m->item_count : 0) * 22 + 8;
    int mx0 = m->x_pos - 4;
    int my2 = AX_MENUBAR_H;
    if (mx < mx0 || mx >= mx0 + mw || my < my2 || my >= my2 + mh) {
        m->open = false;
        open_menu = -1;
        return;
    }
    int idx = (my - my2 - 4) / 22;
    if (idx >= 0 && idx < m->item_count) {

        /* ---- App menu (dynamic, macOS style) ---- */
        if (open_menu == 1) {
            ax_surface_win *w = surf_active_window();
            switch (idx) {
                case AX_APPITEM_ABOUT: /* About <App> */
                    launch_about();
                    break;
                case AX_APPITEM_HIDE: /* Hide <App>: minimize the focused window */
                    if (w) w->minimized = true;
                    break;
                case AX_APPITEM_QUIT: /* Quit <App>: Cmd+Q for the whole process */
                    if (w)
                        app_request_quit(w->owner_pid);
                    break;
            }
        }
        /*
         * ---- App-owned menus ----
         *
         * menus[2..] belong to the frontmost app: forward the
         * click to its focused window as AX_EV_MENU
         * (ev.key = menu index, ev.w = item index) and let the
         * APPLICATION decide what to do -- the shell does not
         * interpret app menus, exactly like macOS.
         *
         * Only when the app never registered its own bar do we
         * fall back to the built-in File/Edit/View handling.
         */
        else if (open_menu >= 2) {
            ax_surface_win *w = surf_active_window();
            bool app_owned = w && menu_lookup(w->owner_pid) != 0;

            if (app_owned) {
                if (open_menu - 2 < menu_lookup(w->owner_pid)->menu_count) {
                    ax_event ev = {0};
                    ev.type = AX_EV_MENU;
                    ev.key  = (uint32_t)(open_menu - 2);  /* menu index  */
                    ev.w    = (uint32_t)idx;              /* item index  */
                    surf_push_event(w, ev);
                }
            }
            else {
                /*
                 * Fallback for apps without their own bar: the
                 * shell still owns File / Edit / View.
                 */
                switch (open_menu) {
                    case 2: /* File */
                        switch (idx) {
                            case 0: /* New Window -> AX_EV_NEWWIN */
                                app_request_newwin();
                                break;
                            case 1: /* Close Window -> the focused window only */
                                if (active_surface >= 0 && active_surface < AX_MAX_WINDOWS) {
                                    ax_surface_win *cw = &surfaces[active_surface];
                                    if (cw->used && !cw->minimized) {
                                        ax_event ev = {0};
                                        ev.type = AX_EV_CLOSE;
                                        surf_push_event(cw, ev);
                                        cw->visible = false;
                                        cw->closing = true;

                                        int new_active = -1, highest_z = -1;
                                        for (int i = 0; i < AX_MAX_WINDOWS; i++) {
                                            if (surfaces[i].used && surfaces[i].visible && !surfaces[i].minimized &&
                                                surfaces[i].z > highest_z) {
                                                highest_z = surfaces[i].z;
                                                new_active = i;
                                            }
                                        }
                                        if (new_active >= 0) surf_focus(&surfaces[new_active]);
                                        else active_surface = -1;
                                    }
                                }
                                break;
                        }
                        break;

                    case 3: /* Edit -- clipboard is not wired yet */
                        break;

                    case 4: /* View */
                        switch (idx) {
                            case 0: w_clock.visible = !w_clock.visible; w_cal.visible = !w_cal.visible; w_sysmon.visible = !w_sysmon.visible; break;
                            case 1: wallpaper_mode = AX_WALL_GRADIENT; break;
                            case 2: if (photo_count > 0) wallpaper_mode = AX_WALL_PHOTO; break;
                            case 3: if (photo_count > 1) wallpaper_mode = AX_WALL_SLIDESHOW; break;
                            case 4: { static int ai = 0; static const uint32_t ac[] = { RGB(10,132,255), RGB(255,69,58), RGB(48,209,88), RGB(255,159,10), RGB(191,90,242) }; ai = (ai + 1) % 5; accent = ac[ai]; break; }
                        }
                        break;
                }
            }
        }
        /* ---- Logo (system) menu ---- */
        else if (open_menu == 0) {
            switch (idx) {
                case 0: system_power_sleep(); break;
                case 1: reboot(); break;
                case 2: system_power_off(); break;
                case 3: launch_about(); break;
                case 4: request_launch("progman"); break;
                case 5: request_launch("prefs"); break;
            }
        }
    }
    m->open = false;
    open_menu = -1;
}

/*
 * ============================================================
 * Dock
 * ============================================================
 */

static struct {
    const char *elf;
    const char *label;
    uint32_t color;
} dock_items[] = {
    { "temu",   "Term",  RGB(40,40,48)   },
    { "files",  "Files",  RGB(255,214,90)    },
    { "notes",  "Notes", RGB(255,214,90) },
    { "calc",   "Calc",  RGB(255,159,10) },
    { "cube",   "GL",    RGB(52,199,89) },
    { "prefs",  "Prefs", RGB(150,150,160)},
    { 0,        "About", RGB(191,90,242) },
};
#define DOCK_COUNT 7
#define DOCK_ICON  40

static int dock_x0(void)
{
    int total = DOCK_COUNT * (DOCK_ICON + 14) + 14;
    return scr_w/2 - total/2;
}
static int dock_y0(void) { return scr_h - AX_DOCK_H - 4; }

static const uint32_t icon_pal_files[4] = { 0, RGB(40,90,160), RGB(90,160,250), RGB(220,235,255) };

void draw_dock(void)
{
    int total = DOCK_COUNT * (DOCK_ICON + 14) + 14;
    int x0 = dock_x0();
    int y0 = dock_y0();

    gfx_rounded_rect(x0 + 3, y0 + 3, total, AX_DOCK_H, 18, 0x40000000);
    gfx_rounded_rect(x0, y0, total, AX_DOCK_H, 18, ARGB(0x88,40,40,50));
    gfx_rounded_outline(x0, y0, total, AX_DOCK_H, 18, 0x60FFFFFF);

    for (int i = 0; i < DOCK_COUNT; i++) {
        int ix = x0 + 14 + i * (DOCK_ICON + 14);
        int iy = y0 + 8;
        bool hover = mouse.x >= ix && mouse.x < ix + DOCK_ICON &&
                     mouse.y >= iy && mouse.y < iy + DOCK_ICON;
        int sz = hover ? DOCK_ICON + 4 : DOCK_ICON;
        int ox = ix - (sz - DOCK_ICON) / 2;
        int oy = iy - (sz - DOCK_ICON);

        gfx_rounded_rect(ox + 2, oy + 2, sz, sz, 10, 0x40000000);
        gfx_rounded_rect(ox, oy, sz, sz, 10, dock_items[i].color);
        gfx_rounded_outline(ox, oy, sz, sz, 10, ARGB(0x50,255,255,255));

        if (i == 0)
            draw_icon(ICON_FILES, icon_pal_files, ox + 4, oy + 4, 2);
        int tw = gfx_text_width(dock_items[i].label);
        gfx_text(dock_items[i].label, ix + DOCK_ICON/2 - tw/2, iy + DOCK_ICON + 2, RGB(220,220,230));
    }
}

void dock_click(int mx, int my)
{
    int x0 = dock_x0();
    int y0 = dock_y0();
    if (my < y0 + 4 || my > y0 + AX_DOCK_H) return;
    for (int i = 0; i < DOCK_COUNT; i++) {
        int ix = x0 + 14 + i * (DOCK_ICON + 14);
        int iy = y0 + 8;
        int sz = DOCK_ICON + 4;
        if (mx >= ix - 2 && mx < ix + sz && my >= iy - 4 && my < iy + sz) {
            if (dock_items[i].elf) {
                /*
                 * macOS dock behaviour: if the app is already
                 * running, clicking its icon focuses (and
                 * un-minimizes) one of its windows instead of
                 * starting a second instance.
                 */
                if (app_focus_running(dock_items[i].elf) < 0)
                    request_launch(dock_items[i].elf);
            } else
                launch_about();
            return;
        }
    }
}

/*
 * ============================================================
 * Right-click context menu
 * ============================================================
 */

static const char *ctx_items[] = { "Refresh", "Next Wallpaper", "Photo WP", "Slideshow", "Next Accent", "About" };
#define CTX_COUNT 6
#define CTX_W 160
#define CTX_ITEM_H 26

void draw_context_menu(void)
{
    if (!ctx_menu_open) return;
    int h = CTX_COUNT * CTX_ITEM_H + 8;
    int cx = ctx_x, cy = ctx_y;
    if (cx + CTX_W > scr_w) cx = scr_w - CTX_W - 2;
    if (cy + h > scr_h) cy = scr_h - h - 2;

    gfx_rounded_rect(cx + 3, cy + 3, CTX_W, h, 8, 0x40000000);
    gfx_rounded_rect(cx, cy, CTX_W, h, 8, ARGB(0xE0,40,40,48));
    gfx_rounded_outline(cx, cy, CTX_W, h, 8, 0x60FFFFFF);

    for (int i = 0; i < CTX_COUNT; i++) {
        int iy = cy + 4 + i * CTX_ITEM_H;
        if (mouse.x >= cx && mouse.x < cx + CTX_W &&
            mouse.y >= iy && mouse.y < iy + CTX_ITEM_H)
            gfx_fill_rect_alpha(cx + 4, iy, CTX_W - 8, CTX_ITEM_H, accent & 0x80FFFFFF);
        gfx_text(ctx_items[i], cx + 14, iy + 9, RGB(230,230,240));
    }
}

void context_menu_click(int mx, int my)
{
    int h = CTX_COUNT * CTX_ITEM_H + 8;
    int cx = ctx_x, cy = ctx_y;
    if (cx + CTX_W > scr_w) cx = scr_w - CTX_W - 2;
    if (cy + h > scr_h) cy = scr_h - h - 2;
    if (mx < cx || mx >= cx + CTX_W || my < cy || my >= cy + h) {
        ctx_menu_open = false; return;
    }
    int idx = (my - cy - 4) / CTX_ITEM_H;
    ctx_menu_open = false;
    switch (idx) {
        case 0: break;
        case 1: ax_set_wallpaper((wallpaper_index + 1) % WALLPAPER_COUNT);
                if (wallpaper_mode == AX_WALL_PHOTO || wallpaper_mode == AX_WALL_SLIDESHOW) {
                    current_photo = (current_photo + 1) % (photo_count > 0 ? photo_count : 1);
                }
                break;
        case 2: if (photo_count > 0) wallpaper_mode = AX_WALL_PHOTO; break;
        case 3: if (photo_count > 1) wallpaper_mode = AX_WALL_SLIDESHOW; break;
        case 4: accent_idx = (accent_idx + 1) % 5; ax_set_accent(accent_cycle[accent_idx]); break;
        case 5: launch_about(); break;
    }
}

/*
 * ============================================================
 * About dialog
 * ============================================================
 */

void launch_about(void) { about_open = true; about_x = scr_w/2 - 180; about_y = scr_h/2 - 110; about_anim = 0; }

void draw_about(void)
{
    if (!about_open) return;
    int w = 360, h = 220;
    int alpha = 0xFF;
    if (about_anim < AX_FADE_FRAMES) {
        alpha = (about_anim * 0xFF) / AX_FADE_FRAMES;
        about_anim++;
    }

    gfx_rounded_rect(about_x + 4, about_y + 4, w, h, 12, 0x50000000);
    gfx_rounded_rect(about_x, about_y, w, h, 12, ARGB(0xF0,30,30,42));
    gfx_rounded_outline(about_x, about_y, w, h, 12, 0x60FFFFFF);

    gfx_text_scaled(AX_OS_NAME, about_x + 30, about_y + 26, RGB(255,255,255), 3);
    gfx_text("Shell: " AX_SHELL_NAME "  v" AX_VERSION, about_x + 30, about_y + 70, accent);
    gfx_text("64-bit UNIX compatible", about_x + 30, about_y + 92, RGB(210,210,220));
    gfx_text("GPU: Software render", about_x + 30, about_y + 108, RGB(255,200,100));
    gfx_text("Powered by iXlinx and ArtyomX", about_x + 30, about_y + 124, RGB(180,180,190));
    gfx_text("[ click to close ]", about_x + 30, about_y + h - 26, RGB(140,140,150));

    if (alpha < 0xFF) {
        uint32_t fade_a = (uint32_t)(0xFF - alpha) << 24;
        gfx_fill_rect_alpha(about_x, about_y, w, h, fade_a);
    }
}
