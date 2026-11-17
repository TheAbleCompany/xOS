/*
 * ============================================================
 * wserver_ui.c  --  Mac OS X Lion edition (userspace WindowServer)
 * ============================================================
 *
 * All the chrome around the window manager, restyled after
 * Mac OS X 10.7 (Lion) -- портировано из kernel/axshell_ui.c
 * в userspace wserver один-в-один:
 *
 *   - translucent light menu bar blurred over the wallpaper
 *     (dark text, Aqua-blue hover, dynamic per-app menus)
 *   - white translucent drop-down menus with soft shadows
 *   - light glass dock with icon magnification, running-app
 *     indicator dots and hover tooltips
 *   - brushed-metal About panel ("About This System")
 *   - light dashboard widgets (calendar / system monitor)
 *     and the glossy dark clock
 *   - black-and-white Lion arrow cursor
 *   - right-click context menu (Lion menu panel style)
 *   - shared UI helpers (RTC, calendar math, int->str, icons)
 *
 * Все примитивы -- gfx_* из wserver_gfx.c, побайтово те же
 * алгоритмы, что в клиентском lib/axquartz.h, поэтому хром
 * сервера и содержимое окон (AquaKit) выглядят одинаково.
 *
 * Клиентских запросов здесь нет -- они в wserver_surface.c.
 * RTC/батарея читаются напрямую портами (сервер -- ring 0).
 */

#include "wserver_internal.h"

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
 * Quit) -- both are server-owned, like the Apple menu and the
 * bold app menu in macOS.
 *
 * menus[2..] belong to the FRONTMOST APP: the app registers its
 * own menus through MSG_REQ_MENU_SET (ax_menu_set()), the server
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
static const char *menu_edit_items[]  = { "Cut", "Copy", "Paste", "Select All" };
static const char *menu_view_items[]  = { "Toggle Widgets", "Space Wallpaper", "Gradient WP",
                                          "Photo WP", "Slideshow", "Next Accent" };
static const char *menu_logo_items[]  = { "Sleep", "Restart...", "Shut Down...",
                                          "About This System", "Mission Control",
                                          "Launchpad", "System Preferences" };
static const char *menu_logo_label    = "X";

/*
 * Rebuild the whole menu bar from the active window:
 *
 *   menus[0]  system logo menu (server)
 *   menus[1]  app menu: About / Hide / Quit <App> (server)
 *   menus[2..] menus[2..] of the app's own registered bar,
 *             or the File/Edit/View fallback (server)
 *
 * Returns false when no app is frontmost (server desktop).
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
        menus[2].label = "File";
        menus[3].label = "Edit"; menus[3].items = menu_edit_items; menus[3].item_count = 4;
        menus[4].label = "View"; menus[4].items = menu_view_items; menus[4].item_count = 6;
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
     * into the server-side registration table (wserver_menu.c),
     * which lives as long as the process does.
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
        menus[2].label = "File";
        menus[3].label = "Edit"; menus[3].items = menu_edit_items; menus[3].item_count = 4;
        menus[4].label = "View"; menus[4].items = menu_view_items; menus[4].item_count = 6;
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

/* accent color with a fresh alpha channel (menu highlights) */
static inline uint32_t accent_alpha(uint32_t a)
{
    return ((uint32_t)a << 24) | (accent & 0x00FFFFFF);
}

/*
 * ============================================================
 * Mouse cursor renderer -- black Lion arrow, white outline
 * ============================================================
 */

/* 0 = transparent, 1 = white outline, 2 = black body */
static const uint8_t cursor_arrow[18][12] = {
    {2,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,0,0,0,0,0,0,0,0,0,0},
    {2,1,2,0,0,0,0,0,0,0,0,0},
    {2,1,1,2,0,0,0,0,0,0,0,0},
    {2,1,1,1,2,0,0,0,0,0,0,0},
    {2,1,1,1,1,2,0,0,0,0,0,0},
    {2,1,1,1,1,1,2,0,0,0,0,0},
    {2,1,1,1,1,1,1,2,0,0,0,0},
    {2,1,1,1,1,1,1,1,2,0,0,0},
    {2,1,1,1,1,1,1,1,1,2,0,0},
    {2,1,1,1,1,2,2,2,2,2,2,0},
    {2,1,2,1,2,0,0,0,0,0,0,0},
    {2,2,0,1,2,0,0,0,0,0,0,0},
    {2,0,0,1,2,0,0,0,0,0,0,0},
    {0,0,0,0,1,2,0,0,0,0,0,0},
    {0,0,0,0,1,2,0,0,0,0,0,0},
    {0,0,0,0,0,1,2,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
};

void draw_cursor(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + 16 > scr_w) x = scr_w - 16;
    if (y + 20 > scr_h) y = scr_h - 20;

    /*
     * Like the old renderer this draws straight onto the
     * backbuffer with no save/restore: compose() repaints the
     * whole desktop every frame, so the previous cursor pixels
     * are naturally overwritten (no backing store, no trails).
     */
    cursor_old_x = x;
    cursor_old_y = y;

    uint32_t blk = RGB(28,28,32);
    uint32_t wht = RGB(255,255,255);

    for (int j = 0; j < 18; j++)
        for (int i = 0; i < 12; i++) {
            uint8_t v = cursor_arrow[j][i];
            if (!v) continue;
            int px = x + i, py = y + j;
            if (px < 0 || py < 0 || px >= scr_w || py >= scr_h) continue;
            gfx_pixel(px, py, (v == 2) ? blk : wht);
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
#ifdef HOST_HARNESS
    /* Host harness: RTC-портов нет -- фиксированное "время". */
    t->second = 8; t->minute = 9; t->hour = 10;
    t->day = 12; t->month = 11; t->year = 2026;
#else
    t->second = bcd2bin(rtc_read(0x00));
    t->minute = bcd2bin(rtc_read(0x02));
    t->hour   = bcd2bin(rtc_read(0x04));
    t->day    = bcd2bin(rtc_read(0x07));
    t->month  = bcd2bin(rtc_read(0x08));
    t->year   = 2000 + bcd2bin(rtc_read(0x09));
#endif
}

static const char *month_names[] = {
    "", "January","February","March","April","May","June",
    "July","August","September","October","November","December"
};
static const char *month_short[] = {
    "", "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char *wday_short[] = {
    "Sun","Mon","Tue","Wed","Thu","Fri","Sat"
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

/* integer sqrt helper for the clock glass dome (local copy) */
static int sqrt_i32_local(int v)
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
 * ============================================================
 * Desktop widgets -- Lion Dashboard look
 * ============================================================
 */

/* glossy dark clock, Lion widget style */
static void widget_clock(int cx, int cy, int r)
{
    ax_time_t t; ax_time_now(&t);

    /* ambient shadow + chrome rim (anti-aliased outer skirt) */
    gfx_circle_aa(cx + 3, cy + 4, r + 6, ARGB(0x38,0,0,0));
    gfx_circle_aa(cx, cy, r + 7, ARGB(0x80,72,72,80));
    gfx_circle(cx, cy, r + 6, ARGB(0xC0,72,72,80));
    gfx_circle(cx, cy, r + 4, ARGB(0xFF,236,236,240));
    gfx_circle(cx, cy, r, ARGB(0xFF,32,32,38));

    /* glass dome highlight on the upper half */
    for (int j = -r; j < -r / 3; j++) {
        int span2 = (r - 2) * (r - 2) - j * j;
        if (span2 < 0) continue;
        int span = sqrt_i32_local(span2);
        int k = j + r;
        int alpha = 0x50 - (k * 0x50) / (r > 0 ? r : 1);
        if (alpha <= 0) continue;
        gfx_fill_rect_alpha(cx - span, cy + j, span * 2 + 1, 1,
                            ((uint32_t)alpha << 24) | 0x00FFFFFF);
    }

    /* minute ticks */
    for (int hh = 0; hh < 12; hh++) {
        int ang = hh * 30;
        int sx = cx + (r - 7) * gfx_cos_deg(ang) / 1000;
        int sy = cy + (r - 7) * gfx_sin_deg(ang) / 1000;
        gfx_fill_rect(sx - 1, sy - 1, 3, 3, RGB(190,190,200));
    }

    int hour_ang = (t.hour % 12) * 30 + t.minute / 2 - 90;
    int min_ang  = t.minute * 6 - 90;
    int sec_ang  = t.second * 6 - 90;

    gfx_line(cx, cy, cx + (r/2) * gfx_cos_deg(hour_ang) / 1000,
                     cy + (r/2) * gfx_sin_deg(hour_ang) / 1000, RGB(250,250,252));
    gfx_line(cx, cy, cx + (r*3/4) * gfx_cos_deg(min_ang) / 1000,
                     cy + (r*3/4) * gfx_sin_deg(min_ang) / 1000, RGB(225,225,235));
    gfx_line(cx, cy, cx + (r-9) * gfx_cos_deg(sec_ang) / 1000,
                     cy + (r-9) * gfx_sin_deg(sec_ang) / 1000, accent);
    gfx_circle(cx, cy, 3, RGB(240,240,245));
    gfx_circle(cx, cy, 1, accent);
}

/* light paper calendar card with a red iCal-style header */
static void widget_calendar(int x, int y)
{
    ax_time_t t; ax_time_now(&t);
    int w = 172, h = 158;

    gfx_soft_shadow(x, y, w, h, 10, 3);

    /* red header (rounded top) */
    gfx_rounded_vgradient(x, y, w, h, 10, RGB(216,74,62), RGB(178,44,38));
    /* paper body over the lower part */
    gfx_rounded_vgradient(x, y + 22, w, h - 22, 6, RGB(252,252,254), RGB(238,238,242));
    gfx_rounded_outline(x, y, w, h, 10, ARGB(0x50,0,0,0));

    char hdr[24]; int p = 0;
    const char *mn = month_names[t.month >= 1 && t.month <= 12 ? t.month : 1];
    while (mn[p] && p < 18) { hdr[p] = mn[p]; p++; } hdr[p] = 0;
    int hw = gfx_text_width(hdr);
    gfx_text(hdr, x + w/2 - hw/2, y + 7, RGB(255,255,255));

    static const char *wd = "S  M  T  W  T  F  S";
    gfx_text(wd, x + 12, y + 30, RGB(150,150,158));

    int dow = day_of_week(1, t.month, t.year);
    int dim = days_in_month(t.month, t.year);
    int col = dow, row = 0;
    for (int d = 1; d <= dim; d++) {
        int cellx = x + 12 + col * 22;
        int celly = y + 46 + row * 17;
        if (d == t.day) {
            gfx_circle_aa(cellx + 5, celly + 4, 8, RGB(206,62,52));
            gfx_circle_aa(cellx + 5, celly + 3, 7, RGB(224,84,70));
        }
        char ds[4];
        if (d < 10) { ds[0] = '0' + d; ds[1] = 0; }
        else        { ds[0] = '0' + d/10; ds[1] = '0' + d%10; ds[2] = 0; }
        gfx_text(ds, cellx, celly,
                 (d == t.day) ? RGB(255,255,255) : RGB(70,70,78));
        col++;
        if (col > 6) { col = 0; row++; }
    }
}

/* light glass system monitor card */
static void widget_sysmon(int x, int y)
{
    int w = 172, h = 104;

    gfx_soft_shadow(x, y, w, h, 10, 3);
    gfx_rounded_vgradient_alpha(x, y, w, h, 10,
                                ARGB(0xE6,252,252,254), ARGB(0xE0,236,236,242));
    gfx_rounded_outline(x, y, w, h, 10, ARGB(0x50,0,0,0));

    gfx_text("System Monitor", x + 12, y + 8, RGB(80,80,90));

    fake_cpu_load = (fake_cpu_load + 7) % 100;
    fake_mem_load = 40 + ((fake_mem_load + 3) % 40);

    gfx_text("CPU", x + 12, y + 34, RGB(120,120,130));
    gfx_rounded_rect(x + 50, y + 34, 110, 8, 4, RGB(222,222,228));
    gfx_rounded_rect(x + 50, y + 34, fake_cpu_load, 8, 4, RGB(84,180,70));

    gfx_text("MEM", x + 12, y + 56, RGB(120,120,130));
    gfx_rounded_rect(x + 50, y + 56, 110, 8, 4, RGB(222,222,228));
    gfx_rounded_rect(x + 50, y + 56, fake_mem_load, 8, 4, accent);

    gfx_text(AX_OS_NAME " x86_64", x + 12, y + 80, RGB(150,150,158));
}

void draw_widgets(void)
{
    /* Clock - top right */
    w_clock.x = scr_w - 130;
    w_clock.y = AX_MENUBAR_H + 20;

    /* Calendar - under clock */
    w_cal.x = scr_w - 184;
    w_cal.y = AX_MENUBAR_H + 150;

    /* System monitor - under calendar */
    w_sysmon.x = scr_w - 184;
    w_sysmon.y = AX_MENUBAR_H + 322;
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
        int cx = ws->x + 50, cy = ws->y + 50, r = 58;
        int dx = mx - cx, dy = my - cy;
        return dx*dx + dy*dy <= r*r;
    }
    if (ws == &w_cal)   return mx >= ws->x && mx < ws->x + 172 && my >= ws->y && my < ws->y + 158;
    if (ws == &w_sysmon) return mx >= ws->x && mx < ws->x + 172 && my >= ws->y && my < ws->y + 104;
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
     * box (8..26px), so it never takes part in the label reflow
     * and its label is never drawn by the menubar loop -- the
     * logo badge is rendered separately by draw_menubar().
     */
    menus[0].label = menu_logo_label;
    menus[0].x_pos = 14;          /* dropdown lands at x=8, under the badge */
    menus[0].w = 18;
    menus[0].open = false;
    menus[0].items = menu_logo_items;
    menus[0].item_count = 6;

    menus[1].label = "Desktop";
    menus[1].x_pos = 44;
    menus[1].w = 40;
    menus[1].open = false;
    menus[1].items = 0;
    menus[1].item_count = 0;

    menus[2].label = "File";
    menus[2].x_pos = 94;
    menus[2].w = 40;
    menus[2].open = false;
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
    menus[4].item_count = 6;

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

/*
 * The system badge -- ONE button, like the Apple menu item.
 * The badge itself is the hit target: it just tints to the
 * accent blue when hovered or open.  No separate highlight
 * rectangle is painted behind it (that read as "two buttons
 * in one").
 */
#define AX_LOGO_X   8
#define AX_LOGO_Y   3
#define AX_LOGO_S   18

static void draw_logo_badge(void)
{
    bool hot = menus[0].open ||
               (mouse.x >= AX_LOGO_X && mouse.x < AX_LOGO_X + AX_LOGO_S &&
                mouse.y >= 0 && mouse.y < AX_MENUBAR_H);

    uint32_t top = hot ? RGB(112,146,236) : RGB(104,104,112);
    uint32_t bot = hot ? RGB(52,88,196)   : RGB(56,56,64);

    gfx_rounded_vgradient(AX_LOGO_X, AX_LOGO_Y, AX_LOGO_S, AX_LOGO_S, 5, top, bot);
    gfx_rounded_outline(AX_LOGO_X, AX_LOGO_Y, AX_LOGO_S, AX_LOGO_S, 5,
                        ARGB(hot ? 0x70 : 0x40,255,255,255));
    gfx_text("X", AX_LOGO_X + 5,
             AX_LOGO_Y + (AX_LOGO_S - gfx_font_h() + 1) / 2, RGB(250,250,252));
}

/*
 * Blue gradient item highlight from the Lion CSS3 reconstruction:
 * #7793F5 -> #406DF2 (50%) -> #275AF1, 1px #1746D7 bottom edge.
 * Opaque colors, drawn as a ROUNDED pill (radius 5) exactly like
 * the real 10.7 item highlight.
 */
static void draw_blue_highlight(int x, int y, int w, int h)
{
    int half = h / 2;
    int r    = h < 10 ? 3 : 5;

    for (int row = 0; row < h; row++) {
        /* rounded corner inset for this row */
        int d    = row < r ? row : (h - 1 - row < r ? h - 1 - row : r);
        int inset;
        uint32_t c;

        if (d >= r) {
            inset = 0;
        } else {
            int t = r - d;

            inset = r - sqrt_i32_local(r * r - t * t);
        }

        c = (row < half)
            ? gfx_lerp_color(LION_MENU_HL_TOP, LION_MENU_HL_MID,
                             row * 256 / (half > 0 ? half : 1))
            : gfx_lerp_color(LION_MENU_HL_MID, LION_MENU_HL_BOT,
                             (row - half) * 256 /
                             (h - half > 0 ? h - half : 1));
        gfx_fill_rect(x + inset, y + row, w - inset * 2, 1, c);
    }
    gfx_fill_rect_alpha(x + r, y + h - 1, w - 2 * r, 1,
                        ARGB(0x50,23,70,215));
}

/* Vertical text offset inside a fixed-height menu item row. */
static int ity(void)
{
    return (AX_MENU_ITEM_H - gfx_font_h() + 1) / 2;
}

/*
 * Shared Lion drop-down panel: heavy soft shadow, frosted blur of
 * the backdrop, translucent white vertical gradient, ~10px rounded
 * corners and a hairline border (the REAL 10.7 menu look).
 * Alpha-blended rounded primitives -- never raw gfx_hline.
 */
static void draw_menu_panel(int mx0, int my0, int mw, int mh)
{
    gfx_soft_shadow(mx0, my0 - 2, mw, mh + 8, 16, 5);
    gfx_blur_region(mx0 + 4, my0 + 4, mw - 8, mh - 8, 3);
    gfx_rounded_vgradient_alpha(mx0, my0, mw, mh, 10,
                                LION_MENU_BG_TOP, LION_MENU_BG_BOT);
    gfx_rounded_outline(mx0, my0, mw, mh, 10, LION_MENU_BORDER);
    gfx_rounded_outline(mx0 + 1, my0 + 1, mw - 2, mh - 2, 9,
                        ARGB(0x55,255,255,255));
}

/* width of a dropdown for the given menu */
static int menu_panel_width(const ax_menu_t *m)
{
    int mw = 120;
    for (int j = 0; j < m->item_count; j++) {
        int tw = gfx_text_width(m->items[j]);
        if (tw > mw) mw = tw;
    }
    return mw + 30;
}

void draw_menubar(void)
{
    /*
     * Lion translucency: blur whatever the wallpaper drew under
     * the bar, then lay a light vertical gradient over it.
     */
    gfx_blur_region(0, 0, scr_w, AX_MENUBAR_H, 2);
    gfx_vgradient_alpha(0, 0, scr_w, AX_MENUBAR_H, LION_MBAR_TOP, LION_MBAR_BOT);
    /* no rule under the bar at all: gfx_hline is alpha-blind, so
     * any "subtle" line paints as a solid dark/bright stripe --
     * the clean glass edge IS the border (user request). */

    /*
     * Rebuild the dynamic app menu and reflow the whole bar:
     * x positions are computed from the actual label widths so
     * any app name and any app-owned menu set fits (macOS
     * reflows the same way).
     */
    app_menu_refresh();

    /*
     * Reflow starts AFTER the logo box: menus[0] is the logo
     * menu and must not be drawn as a text label here.
     */
    int x = 40;
    for (int i = 1; i < menu_count; i++) {
        menus[i].x_pos = x;
        menus[i].w = menus[i].label ? gfx_text_width(menus[i].label) + 14 : 40;
        x = menus[i].x_pos + menus[i].w + 2;
    }

    /* --- logo badge (single button; its menu is drawn below) --- */
    draw_logo_badge();

    bool app_active = (menus[1].items != 0);

    /*
     * The loop covers ALL menus including the logo: menus[0]
     * only skips the text-label pass (the badge IS its label),
     * but its dropdown panel is rendered like any other menu.
     */
    for (int i = 0; i < menu_count; i++) {
        if (i == 0) {
            /* dropdown only -- the badge was already drawn */
            if (menus[0].open && menus[0].items && menus[0].item_count > 0) {
                int mw = menu_panel_width(&menus[0]);
                int mh = menus[0].item_count * AX_MENU_ITEM_H + 8;
                int mx0 = menus[0].x_pos - 6;
                int my0 = AX_MENUBAR_H;

                if (mx0 + mw > scr_w) mx0 = scr_w - mw - 2;

                draw_menu_panel(mx0, my0, mw, mh);

                for (int j = 0; j < menus[0].item_count; j++) {
                    int iy = my0 + 4 + j * AX_MENU_ITEM_H;
                    bool ihover = mouse.x >= mx0 && mouse.x < mx0 + mw &&
                                 mouse.y >= iy && mouse.y < iy + AX_MENU_ITEM_H;
                    if (ihover) {
                        draw_blue_highlight(mx0 + 4, iy, mw - 8, AX_MENU_ITEM_H);
                        gfx_text(menus[0].items[j], mx0 + 12, iy + ity(), RGB(255,255,255));
                    } else {
                        gfx_text(menus[0].items[j], mx0 + 12, iy + ity(), LION_MENU_TEXT);
                    }
                }
            }
            continue;
        }
        bool hover = mouse.x >= menus[i].x_pos && mouse.x < menus[i].x_pos + menus[i].w &&
                     mouse.y >= 0 && mouse.y < AX_MENUBAR_H;

        /* square title highlight, blue gradient like the items */
        if (menus[i].open)
            draw_blue_highlight(menus[i].x_pos - 6, 2, menus[i].w + 8,
                                AX_MENUBAR_H - 4);
        else if (hover)
            gfx_fill_rect_alpha(menus[i].x_pos - 6, 2, menus[i].w + 8, AX_MENUBAR_H - 4,
                                ARGB(0x46,0,0,0));

        uint32_t color = (menus[i].open || hover) ? RGB(255,255,255) : LION_MBAR_TEXT;

        /*
         * The frontmost app's name is the "bold app menu" cue:
         * faked with a 1px double-draw (8x8 bitmap font).
         */
        if (menus[i].label) {
            int ty = (AX_MENUBAR_H - gfx_font_h() + 1) / 2;
            if (i == 1 && app_active && !menus[i].open && !hover) {
                gfx_text(menus[i].label, menus[i].x_pos,     ty, RGB(15,15,18));
                gfx_text(menus[i].label, menus[i].x_pos + 1, ty, RGB(15,15,18));
            } else {
                gfx_text(menus[i].label, menus[i].x_pos, ty, color);
            }
        }

        /* --- dropdown panel --- */
        if (menus[i].open && menus[i].items && menus[i].item_count > 0) {
            int mw = menu_panel_width(&menus[i]);
            int mh = menus[i].item_count * AX_MENU_ITEM_H + 8;
            int mx0 = menus[i].x_pos - 6;
            int my0 = AX_MENUBAR_H;

            if (mx0 + mw > scr_w) mx0 = scr_w - mw - 2;

            draw_menu_panel(mx0, my0, mw, mh);

            for (int j = 0; j < menus[i].item_count; j++) {
                int iy = my0 + 4 + j * AX_MENU_ITEM_H;
                bool ihover = mouse.x >= mx0 && mouse.x < mx0 + mw &&
                              mouse.y >= iy && mouse.y < iy + AX_MENU_ITEM_H;
                if (ihover) {
                    draw_blue_highlight(mx0 + 4, iy, mw - 8, AX_MENU_ITEM_H);
                    gfx_text(menus[i].items[j], mx0 + 12, iy + ity(), RGB(255,255,255));
                } else {
                    gfx_text(menus[i].items[j], mx0 + 12, iy + ity(), LION_MENU_TEXT);
                }
            }
        }
    }

    /* --- right side: date + clock + battery (Lion layout) --- */
    ax_time_t t; ax_time_now(&t);

    char clk[8];
    clk[0] = '0' + t.hour/10; clk[1] = '0' + t.hour%10; clk[2] = ':';
    clk[3] = '0' + t.minute/10; clk[4] = '0' + t.minute%10;
    clk[5] = 0;

    int wd = day_of_week(t.day, t.month, t.year);
    char date[16];
    int p = 0;
    const char *ds1 = wday_short[wd];
    while (*ds1) date[p++] = *ds1++;
    date[p++] = ' ';
    const char *ds2 = month_short[t.month >= 1 && t.month <= 12 ? t.month : 1];
    while (*ds2) date[p++] = *ds2++;
    date[p++] = ' ';
    int_to_str(t.day, date + p, 4);
    while (date[p]) p++;          /* int_to_str returns status, not length */
    date[p] = 0;

    char buf[8];
    int_to_str(get_battery_percentage_fast(), buf, sizeof(buf));

    int bat_w = gfx_text_width(buf);
    int bat_x = scr_w - 8 - bat_w;

    /* battery glyph: outline + fill + nub */
    int gx = bat_x - 20;
    gfx_rect_outline(gx, 8, 14, 8, RGB(70,70,78));
    gfx_vline(gx + 14, 10, 4, RGB(70,70,78));
    int fill = get_battery_percentage_fast() * 10 / 100;
    if (fill > 10) fill = 10;
    if (fill < 0) fill = 0;
    if (fill > 0) gfx_fill_rect(gx + 2, 10, fill, 4, RGB(80,170,70));

    int time_w = gfx_text_width(clk);
    int date_w = gfx_text_width(date);
    int my     = (AX_MENUBAR_H - gfx_font_h() + 1) / 2;

    gfx_text(clk,  gx - 12 - time_w, my, LION_MBAR_TEXT);
    gfx_text(date, gx - 12 - time_w - 10 - date_w, my, LION_MBAR_TEXT);
    gfx_text(buf, bat_x, my, LION_MBAR_TEXT);
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
    if (open_menu < 0) return;
    if (open_menu >= menu_count) return;
    ax_menu_t *m = &menus[open_menu];
    int mw = menu_panel_width(m);
    int mh = (m->items ? m->item_count : 0) * AX_MENU_ITEM_H + 8;
    int mx0 = m->x_pos - 6;
    int my2 = AX_MENUBAR_H;
    if (mx0 + mw > scr_w) mx0 = scr_w - mw - 2;
    if (mx < mx0 || mx >= mx0 + mw || my < my2 || my >= my2 + mh) {
        m->open = false;
        open_menu = -1;
        return;
    }
    int idx = (my - my2 - 4) / AX_MENU_ITEM_H;
    if (idx >= 0 && idx < m->item_count) {

        /* ---- App menu (dynamic, macOS style) ---- */
        if (open_menu == 1) {
            ax_surface_win *w = surf_active_window();
            switch (idx) {
                case AX_APPITEM_ABOUT: /* About <App> */
                    launch_about();
                    break;
                case AX_APPITEM_HIDE: /* Hide <App>: genie into the dock */
                    if (w) win_minimize_anim(w);
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
         * APPLICATION decide what to do -- exactly like macOS.
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
                 * server still owns File / Edit / View.
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
                            case 0: w_clock.visible = !w_clock.visible;
                                    w_cal.visible = !w_cal.visible;
                                    w_sysmon.visible = !w_sysmon.visible; break;
                            case 1: wallpaper_mode = AX_WALL_SPACE; break;
                            case 2: wallpaper_mode = AX_WALL_GRADIENT; break;
                            case 3: if (photo_count > 0) wallpaper_mode = AX_WALL_PHOTO; break;
                            case 4: if (photo_count > 1) wallpaper_mode = AX_WALL_SLIDESHOW; break;
                            case 5: { static int ai = 0;
                                      static const uint32_t ac[] = {
                                          RGB(10,132,255), RGB(255,69,58), RGB(48,209,88),
                                          RGB(255,159,10), RGB(191,90,242) };
                                      ai = (ai + 1) % 5; accent = ac[ai]; break; }
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
                case 4: lion_toggle_mission(); break;     /* Mission Control */
                case 5: lion_toggle_launchpad(); break;   /* Launchpad       */
                case 6: request_launch("prefs"); break;
            }
        }
    }
    m->open = false;
    open_menu = -1;
}

/*
 * ============================================================
 * Dock -- Lion glass shelf
 * ============================================================
 *
 *   * light translucent shelf blurred over the wallpaper
 *   * icons magnify around the cursor and push their neighbours
 *     aside (the classic Lion magnification)
 *   * icons and the running-app lights mirror in the glass
 *   * running applications get a glowing indicator light under
 *     the icon -- it burns even when the window is minimized
 *   * hovering an icon pops a black tooltip above the shelf
 */

/*
 * Элементы дока.  kind:
 *   DOCK_KIND_APP  -- обычное приложение (elf), exec через request_launch
 *   DOCK_KIND_ABOUT-- системная панель "About This System"
 *   DOCK_KIND_LP   -- Launchpad (Lion): открывает оверлей, не exec
 */
#define DOCK_KIND_APP   0
#define DOCK_KIND_ABOUT 1
#define DOCK_KIND_LP    2

static struct {
    const char *elf;
    const char *label;
    uint32_t color;
    int kind;
    int icon;          /* LION_ICON_* */
} dock_items[] = {
    { 0,        "Launchpad", RGB(220,224,236), DOCK_KIND_LP,    LION_ICON_LAUNCHPAD },
    { "temu",   "Term",  RGB(52,60,72)   , DOCK_KIND_APP,   LION_ICON_TERMINAL },
    { "files",  "Files", RGB(70,140,240) , DOCK_KIND_APP,   LION_ICON_FOLDER   },
    { "notes",  "Notes", RGB(255,214,90) , DOCK_KIND_APP,   LION_ICON_NOTES    },
    { "calc",   "Calc",  RGB(90,98,110)  , DOCK_KIND_APP,   LION_ICON_CALC     },
    { "cube",   "GL",    RGB(52,199,89)  , DOCK_KIND_APP,   LION_ICON_GL       },
    { "prefs",  "Prefs", RGB(150,150,160), DOCK_KIND_APP,   LION_ICON_PREFS    },
    { "doom",   "DOOM",  RGB(150,32,28)  , DOCK_KIND_APP,   LION_ICON_DOOM     },
    { "quake",  "Quake", RGB(110,78,48)  , DOCK_KIND_APP,   LION_ICON_QUAKE    },
    { 0,        "About", RGB(191,90,242) , DOCK_KIND_ABOUT, LION_ICON_ABOUT    },
};
#define DOCK_COUNT 10
#define DOCK_ICON  52      /* big icons STAND on the glass ledge, Lion-style */
#define DOCK_GAP   14
#define DOCK_PAD   42      /* generous glass margins left/right of the row   */
#define DOCK_MAG   14      /* size gain at the cursor (subtle, Lion-like)    */
#define DOCK_R     85      /* magnification influence radius                 */
#define DOCK_SPREAD 120    /* neighbour push, 1/256 of the extra             */

/*
 * Адаптивные метрики: на узких экранах (<=860px по ширине) док
 * сжимается, иначе 10 иконок не влезают.
 */
static int dock_icon_w(void) { return scr_w <= 860 ? 40 : DOCK_ICON; }
static int dock_gap_w(void)  { return scr_w <= 860 ? 8  : DOCK_GAP;  }
static int dock_pad_w(void)  { return scr_w <= 860 ? 20 : DOCK_PAD;  }

/* shelf geometry (fixed per frame; the shelf itself doesn't move)
 * Shape matches the 10.7 reference: a THIN WIDE horizontal glass
 * ledge (~42 px tall) with the big icons standing on it -- their
 * bottoms rest on the ledge and their tops rise above its upper
 * edge.  Wide margins of empty glass flank the icon row.  The
 * slab is a TABLE LYING IN PERSPECTIVE: the back (upper) edge is
 * narrower than the front (lower) one and the sides slant out. */
static int dock_shelf_y(void) { return scr_h - 48; }
static int dock_shelf_h(void) { return 42; }

/* perspective glass slab geometry */
#define DOCK_PERSP 8        /* back-edge inset per side (px)      */
#define DOCK_RND   0        /* sharp corners, like the real Lion  */

/* horizontal inset of a rounded corner on row y (0..sh-1) */
static int dock_corner_inset(int y, int sh, int r)
{
    int dy;
    if (y < r)          dy = y;
    else if (y >= sh - r) dy = sh - 1 - y;
    else                return 0;
    int t = r - dy;                        /* 1..r */
    return r - sqrt_i32_local(r * r - t * t);
}

/* magnified layout, recomputed by both draw and click paths */
static int  dock_cx[DOCK_COUNT];
static int  dock_sz[DOCK_COUNT];
static int  dock_x0, dock_total;
static int  dock_hover = -1;

static int dock_base_total(void)
{
    int iw = dock_icon_w(), gap = dock_gap_w(), pad = dock_pad_w();
    return pad * 2 + DOCK_COUNT * iw + (DOCK_COUNT - 1) * gap;
}

/*
 * Compute the magnified layout for the current mouse position.
 * Two passes: the growth of the row shifts x0 (the dock stays
 * centred), so the influence distances are computed twice.
 */
static void dock_layout(void)
{
    bool over_dock = mouse.y >= dock_shelf_y() - 30 && mouse.y < scr_h;
    int iw = dock_icon_w(), gap = dock_gap_w();

    for (int pass = 0; pass < 2; pass++) {
        int x0 = scr_w / 2 - dock_base_total() / 2;
        int extra[DOCK_COUNT];

        for (int i = 0; i < DOCK_COUNT; i++) {
            int bc = x0 + dock_pad_w() + i * (iw + gap) + iw / 2;
            int d = mouse.x - bc;
            int e = 0;
            if (over_dock && d < DOCK_R && d > -DOCK_R) {
                int fall = DOCK_R * DOCK_R - d * d;
                e = DOCK_MAG * fall / (DOCK_R * DOCK_R);
                if (e < 0) e = 0;
            }
            extra[i] = e;
        }

        int sum = 0;
        for (int i = 0; i < DOCK_COUNT; i++) sum += extra[i];

        dock_total = dock_base_total() + (sum * DOCK_SPREAD) / 256;
        dock_x0 = scr_w / 2 - dock_total / 2;

        int push = 0;
        for (int i = 0; i < DOCK_COUNT; i++) {
            dock_cx[i] = dock_x0 + dock_pad_w() + i * (iw + gap)
                       + iw / 2 + (push * DOCK_SPREAD) / 256;
            push += extra[i];
            dock_sz[i] = iw + extra[i];
        }
    }

    dock_hover = -1;
    for (int i = 0; i < DOCK_COUNT; i++) {
        int half = dock_sz[i] / 2 + 4;
        if (mouse.x >= dock_cx[i] - half && mouse.x < dock_cx[i] + half &&
            mouse.y >= dock_shelf_y() - 30 && mouse.y < dock_shelf_y() + dock_shelf_h())
            dock_hover = i;
    }
}

/*
 * Horizontal centre of a dock slot (no magnification) -- used by
 * the minimize/restore genie in wserver_surface.c.
 */
int dock_icon_cx_by_elf(const char *elf_name)
{
    if (!elf_name) return -1;
    for (int i = 0; i < DOCK_COUNT; i++)
        if (dock_items[i].elf &&
            strcmp((char *)dock_items[i].elf, (char *)elf_name) == 0)
            return scr_w / 2 - dock_base_total() / 2
                 + dock_pad_w() + i * (dock_icon_w() + dock_gap_w())
                 + dock_icon_w() / 2;
    return -1;
}

/* ---- procedural Aqua icons (scaled to the magnified size) ---- */

static void icon_terminal(int x, int y, int s)
{
    gfx_rounded_vgradient(x, y, s, s, 3, RGB(70,76,90), RGB(30,34,42));
    gfx_rounded_outline(x, y, s, s, 3, ARGB(0x60,255,255,255));
    gfx_fill_rect(x + 2, y + 2, s - 4, 3, RGB(210,210,220));       /* title strip */
    int u = s / 8;
    gfx_line(x + 3*u, y + 6*u, x + 4*u, y + 7*u, RGB(120,255,120)); /* > */
    gfx_line(x + 4*u, y + 7*u, x + 3*u, y + 8*u, RGB(120,255,120));
    gfx_fill_rect(x + 5*u, y + 8*u - 1, 2*u, 2, RGB(120,255,120));  /* _ */
}

static void icon_folder(int x, int y, int s)
{
    int tab_h = s / 4;
    gfx_rounded_rect(x, y, s / 2, tab_h, 2, RGB(52,110,200));
    gfx_rounded_vgradient(x, y + tab_h - 2, s, s - tab_h + 2, 3,
                          RGB(96,166,250), RGB(40,92,190));
    gfx_rounded_outline(x, y + tab_h - 2, s, s - tab_h + 2, 3, ARGB(0x50,255,255,255));
    gfx_fill_rect_alpha(x + 2, y + tab_h, s - 4, 2, ARGB(0x50,255,255,255));
}

static void icon_notes(int x, int y, int s)
{
    gfx_rounded_vgradient(x, y, s, s, 3, RGB(255,240,180), RGB(245,220,120));
    gfx_rounded_outline(x, y, s, s, 3, ARGB(0x50,120,100,40));
    gfx_fill_rect(x + 2, y + 2, s - 4, s / 5, RGB(232,180,60));
    int u = s / 8;
    for (int l = 0; l < 3; l++)
        gfx_fill_rect(x + 2*u, y + 4*u + l * 2*u, s - 4*u, 1, RGB(140,120,70));
}

static void icon_calc(int x, int y, int s)
{
    gfx_rounded_vgradient(x, y, s, s, 3, RGB(120,126,138), RGB(60,64,74));
    gfx_rounded_outline(x, y, s, s, 3, ARGB(0x50,255,255,255));
    int u = s / 8;
    gfx_fill_rect(x + 2*u, y + u, s - 4*u, 2*u, RGB(140,220,120));   /* display */
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++)
            gfx_fill_rect(x + 2*u + c * 2*u, y + 4*u + r * 2*u,
                          u + 1, u + 1, RGB(220,222,230));
}

static void icon_gl(int x, int y, int s)
{
    gfx_rounded_vgradient(x, y, s, s, 3, RGB(90,210,110), RGB(30,140,60));
    gfx_rounded_outline(x, y, s, s, 3, ARGB(0x50,255,255,255));
    int m = s / 2;
    /* wireframe cube hint: diamond + inner edges */
    gfx_line(x + m, y + 3, x + s - 4, y + m, RGB(240,255,240));
    gfx_line(x + s - 4, y + m, x + m, y + s - 4, RGB(240,255,240));
    gfx_line(x + m, y + s - 4, x + 4, y + m, RGB(240,255,240));
    gfx_line(x + 4, y + m, x + m, y + 3, RGB(240,255,240));
    gfx_line(x + m, y + 3, x + m, y + s - 4, ARGB(0xB0,240,255,240));
    gfx_line(x + 4, y + m, x + s - 4, y + m, ARGB(0xB0,240,255,240));
}

static void icon_prefs(int x, int y, int s)
{
    gfx_rounded_vgradient(x, y, s, s, 3, RGB(200,202,210), RGB(140,142,152));
    gfx_rounded_outline(x, y, s, s, 3, ARGB(0x50,255,255,255));
    int u = s / 8;
    for (int l = 0; l < 3; l++) {
        int ly = y + 2*u + l * 2*u;
        gfx_fill_rect(x + u, ly, s - 2*u, 2, RGB(250,250,252));
        int kx = x + (l == 0 ? 4 : l == 1 ? 2 : 5) * u;
        gfx_fill_rect(kx, ly - 1, 2*u, 4, RGB(70,74,84));
    }
}

static void icon_about(int x, int y, int s)
{
    gfx_rounded_vgradient(x, y, s, s, 3, RGB(210,140,250), RGB(130,50,190));
    gfx_rounded_outline(x, y, s, s, 3, ARGB(0x50,255,255,255));
    int u = s / 8;
    /* "i" glyph: dot + stem */
    gfx_fill_rect(x + 3*u, y + u, 2*u, 2*u, RGB(255,255,255));
    gfx_fill_rect(x + 3*u, y + 4*u, 2*u, 3*u, RGB(255,255,255));
}

/*
 * Raycaster -- лазерный прицел на тёмном фоне (запускает
 * apps/raycaster.bin).
 */
static void icon_raycaster(int x, int y, int s)
{
    gfx_rounded_vgradient(x, y, s, s, 3, RGB(64,58,52), RGB(26,22,18));
    gfx_rounded_outline(x, y, s, s, 3, ARGB(0x50,255,255,255));
    int cx = x + s / 2, cy = y + s / 2;
    int r = s / 3;
    int u = s / 8;
    gfx_circle_aa(cx, cy, r, RGB(230,120,60));
    gfx_circle_aa(cx, cy, u, RGB(255,190,120));
    gfx_line(cx - r - u, cy, cx - u, cy, RGB(230,120,60));
    gfx_line(cx + u, cy, cx + r + u, cy, RGB(230,120,60));
    gfx_line(cx, cy - r - u, cx, cy - u, RGB(230,120,60));
    gfx_line(cx, cy + u, cx, cy + r + u, RGB(230,120,60));
}

/* Bench -- спидометр (запускает apps/bench.bin). */
static void icon_bench(int x, int y, int s)
{
    gfx_rounded_vgradient(x, y, s, s, 3, RGB(60,110,190), RGB(24,52,110));
    gfx_rounded_outline(x, y, s, s, 3, ARGB(0x50,255,255,255));
    int cx = x + s / 2;
    int cy = y + s / 2 + s / 6;
    int r  = s / 3;
    /* дуга спидометра + стрелка */
    for (int a = -150; a <= -30; a += 10) {
        int px = cx + r * gfx_cos_deg(a) / 1000;
        int py = cy + r * gfx_sin_deg(a) / 1000;
        gfx_fill_rect(px - 1, py - 1, 3, 3, RGB(220,230,250));
    }
    int na = -120;
    gfx_line(cx, cy, cx + (r - 3) * gfx_cos_deg(na) / 1000,
             cy + (r - 3) * gfx_sin_deg(na) / 1000, RGB(255,90,60));
    gfx_circle_aa(cx, cy, 2, RGB(255,255,255));
}

/* GUI Test -- Aqua-глазок (запускает apps/guitest.bin). */
static void icon_guitest(int x, int y, int s)
{
    gfx_rounded_vgradient(x, y, s, s, 3, RGB(120,90,190), RGB(60,40,120));
    gfx_rounded_outline(x, y, s, s, 3, ARGB(0x50,255,255,255));
    int cx = x + s / 2, cy = y + s / 2;
    int r = s / 3;
    gfx_circle_aa(cx, cy, r, RGB(250,250,255));
    gfx_circle_aa(cx, cy, r / 2, RGB(40,140,230));
    gfx_circle_aa(cx - r / 4, cy - r / 4, r / 6, RGB(255,255,255));
}

/*
 * Launchpad -- серебряная ракета на стеклянной плитке (в доке
 * и в самой сетке Launchpad).
 */
static void icon_launchpad(int x, int y, int s)
{
    gfx_rounded_vgradient(x, y, s, s, 3, RGB(235,238,246), RGB(178,182,196));
    gfx_rounded_outline(x, y, s, s, 3, ARGB(0x50,255,255,255));
    int u = s / 8;
    int cx = x + s / 2;
    /* корпус -- вертикальная капсула */
    gfx_rounded_vgradient(cx - u, y + u, 2 * u, 5 * u, u,
                          RGB(250,250,255), RGB(210,214,226));
    gfx_rounded_outline(cx - u, y + u, 2 * u, 5 * u, u, ARGB(0x60,90,94,110));
    /* нос */
    gfx_circle_aa(cx, y + u + 1, u, RGB(226,60,50));
    /* иллюминатор */
    gfx_circle_aa(cx, y + 3 * u, u / 2 + 1, RGB(70,110,190));
    /* стабилизаторы */
    gfx_line(cx - u, y + 5 * u, cx - 3 * u, y + 7 * u, RGB(226,60,50));
    gfx_line(cx + u, y + 5 * u, cx + 3 * u, y + 7 * u, RGB(226,60,50));
    /* огонь */
    gfx_circle_aa(cx, y + 6 * u + u / 2, u / 2, RGB(255,170,60));
}

/*
 * DOOM -- череп на тёмно-красном камне (иконка запускает
 * apps/doom.bin; см. apps/doom/README-DOOM.md).
 */
static void icon_doom(int x, int y, int s)
{
    int u = s / 8;

    /* череп */
    gfx_rounded_rect(x + 2*u, y + u, s - 4*u, 4*u, u, RGB(232,226,206));
    gfx_fill_rect(x + 3*u, y + 5*u - 1, s - 6*u, 2*u, RGB(232,226,206));

    /* глазницы */
    gfx_fill_rect(x + 3*u, y + 2*u, u + 1, u + 1, RGB(40,8,8));
    gfx_fill_rect(x + s - 4*u - 1, y + 2*u, u + 1, u + 1, RGB(40,8,8));

    /* нос и зубы */
    gfx_fill_rect(x + 4*u - 1, y + 4*u - 1, 2, u, RGB(110,90,80));
    gfx_fill_rect(x + 4*u, y + 6*u - 1, 1, u - 1, RGB(70,14,14));
    gfx_fill_rect(x + 5*u, y + 6*u - 1, 1, u - 1, RGB(70,14,14));
}

/*
 * Quake -- каменное кольцо с руной-хвостом (запускает apps/quake.bin).
 */
static void icon_quake(int x, int y, int s)
{
    int cx = x + s / 2;
    int cy = y + s / 2;
    int r  = s / 2 - s / 8;
    int u  = s / 8;

    /* каменное кольцо */
    gfx_circle_aa(cx, cy, r, RGB(198,180,152));
    gfx_circle_aa(cx, cy, r - u / 2, RGB(58,38,30));

    /* хвост буквы Q */
    gfx_line(cx + u / 2, cy + u / 2, cx + r, cy + r, RGB(198,180,152));
    gfx_line(cx + u / 2, cy + u / 2 + 1, cx + r, cy + r + 1, RGB(118,94,72));
}

/*
 * ============================================================
 * Общий диспетчер иконок -- док (dock icon) и Launchpad
 * (wserver_lion.c) рисуют ОДНИ И ТЕМ ЖЕ плитки: kind ->
 * lion_icon_tile() = фон-градиент + глянец + art.
 * ============================================================
 */

/* art -- содержимое плитки (внутренний рисунок) */
static void lion_icon_art(int kind, int x, int y, int s)
{
    switch (kind) {
    case LION_ICON_TERMINAL:  icon_terminal(x, y, s); break;
    case LION_ICON_FOLDER:    icon_folder(x, y, s);   break;
    case LION_ICON_NOTES:     icon_notes(x, y, s);    break;
    case LION_ICON_CALC:      icon_calc(x, y, s);     break;
    case LION_ICON_GL:        icon_gl(x, y, s);       break;
    case LION_ICON_PREFS:     icon_prefs(x, y, s);    break;
    case LION_ICON_DOOM:      icon_doom(x, y, s);     break;
    case LION_ICON_QUAKE:     icon_quake(x, y, s);    break;
    case LION_ICON_ABOUT:     icon_about(x, y, s);    break;
    case LION_ICON_RAYCAST:   icon_raycaster(x, y, s); break;
    case LION_ICON_BENCH:     icon_bench(x, y, s);    break;
    case LION_ICON_GUITEST:   icon_guitest(x, y, s);  break;
    case LION_ICON_LAUNCHPAD: icon_launchpad(x, y, s); break;
    default:                  icon_about(x, y, s);    break;
    }
}

/* плитка целиком: подложка + глянец + рамка + art */
void lion_icon_tile(int kind, int x, int y, int s, uint32_t color)
{
    uint32_t col_top = gfx_lerp_color(color, RGB(255,255,255), 90);

    gfx_rounded_rect(x, y + 3, s, s, 8, ARGB(0x32,0,0,0));   /* drop shadow */
    gfx_rounded_vgradient(x, y, s, s, 8, col_top, color);
    gfx_gloss_top(x, y, s, s, 8, s / 3);
    gfx_rounded_outline(x, y, s, s, 8, ARGB(0x66,255,255,255));

    lion_icon_art(kind, x + s/6, y + s/6, s - s/3);
}

static void draw_dock_icon(int i, int x, int y, int s)
{
    lion_icon_tile(dock_items[i].icon, x, y, s, dock_items[i].color);
}

/*
 * Perspective glass slab: the BACK (upper) edge is DOCK_PERSP px
 * narrower on each side than the front one, sides slant outward
 * and the corners stay softly rounded.  Everything is drawn with
 * alpha-blended 1px rows so the blurred wallpaper reads through
 * the glass (gfx_hline/gfx_pixel are alpha-blind -- never use
 * them on translucent ARGB).
 */
static void draw_dock_slab(int gx, int gw, int sy, int sh)
{
    for (int y = 0; y < sh; y++) {
        /* perspective: inset lerps DOCK_PERSP (back) -> 0 (front) */
        int ins = DOCK_PERSP * (sh - 1 - y) / (sh - 1);
        ins += dock_corner_inset(y, sh, DOCK_RND);

        int x0 = gx + ins;
        int x1 = gx + gw - ins;                /* exclusive */
        int w  = x1 - x0;
        if (w <= 2) continue;

        uint32_t c = gfx_lerp_color(LION_DOCK_TOP, LION_DOCK_BOT,
                                    y * 256 / (sh - 1));
        gfx_fill_rect_alpha(x0, sy + y, w, 1, c);

        /* slanted side edges (alpha-blended) */
        gfx_fill_rect_alpha(x0,     sy + y, 1, 1, LION_DOCK_EDGE);
        gfx_fill_rect_alpha(x1 - 1, sy + y, 1, 1, LION_DOCK_EDGE);

        /* bevels that follow the trapezoid edges */
        if (y == 0)
            gfx_fill_rect_alpha(x0, sy, w, 1, ARGB(0x82,255,255,255));
        else if (y == 1)
            gfx_fill_rect_alpha(x0 + 2, sy + 1, w - 4, 1,
                                ARGB(0x30,255,255,255));
        else if (y == sh - 2)
            gfx_fill_rect_alpha(x0 + 2, sy + sh - 2, w - 4, 1,
                                ARGB(0x28,40,44,66));
        else if (y == sh - 1)
            gfx_fill_rect_alpha(x0, sy + sh - 1, w, 1,
                                ARGB(0x40,20,22,40));
    }
}

void draw_dock(void)
{
    dock_layout();

    int sy = dock_shelf_y();
    int sh = dock_shelf_h();

    int gx = dock_x0 - 10, gw = dock_total + 20;

    /*
     * Lion glass shelf: a soft drop shadow under the slab, the
     * wallpaper blurred behind it, then the perspective slab on
     * top so the backdrop clearly reads through the glass.
     */
    gfx_soft_shadow(gx, sy, gw, sh, 12, 4);
    gfx_blur_region(gx - 6, sy - 6, gw + 12, sh + 10, 2);
    draw_dock_slab(gx, gw, sy, sh);

    /* icons: bottoms rest on the ledge (a 12px glass strip below
     * carries their mirror reflection), tops rise above its edge */
    for (int i = 0; i < DOCK_COUNT; i++) {
        int sz = dock_sz[i];
        int ix = dock_cx[i] - sz / 2;
        int iy = sy + sh - 12 - sz;            /* standing on the shelf */

        draw_dock_icon(i, ix, iy, sz);

        /*
         * Icon reflection through the glass: mirrored rows of the
         * just-rendered icon fading out (the monolithic look the
         * user asked back).  gfx_pixel writes a PRE-blended value
         * (gfx_blend does the mixing first), so this is safe --
         * raw alpha-blind writes would be forbidden here.
         */
        int ib = iy + sz;
        for (int r = 0; r < 12 && ib + r < sy + sh - 1; r++) {
            int src_y = ib - 1 - r;
            if (src_y < 0) break;
            int a = (0x48 * (11 - r)) / 11;
            for (int px = ix + 2; px < ix + sz - 2; px++) {
                if (px < 0 || px >= scr_w || ib + r >= scr_h) continue;
                uint32_t s = gfx_get_pixel(px, src_y);
                if ((s >> 24) == 0) continue;
                uint32_t dst = gfx_get_pixel(px, ib + r);
                uint32_t bl = gfx_blend(dst, ((uint32_t)a << 24) |
                                             (s & 0x00FFFFFF));
                gfx_pixel(px, ib + r, bl);
            }
        }

        /*
         * Running-app indicator light: white core + cyan glow.
         * app_is_running() scans every used surface, so the light
         * burns whenever the app owns a live window -- including
         * one that is currently minimized.  Right beneath the
         * light its own mirror smears down the glass.
         */
        if (dock_items[i].elf && app_is_running(dock_items[i].elf)) {
            int dy = sy + sh - 4;
            gfx_circle_aa(dock_cx[i], dy, 5, ARGB(0x30,75,255,255));
            gfx_circle_aa(dock_cx[i], dy, 3, ARGB(0x66,75,255,255));
            gfx_circle_aa(dock_cx[i], dy, 2, ARGB(0xE0,255,255,255));

            /* the light reflected in the glass below it */
            gfx_fill_rect_alpha(dock_cx[i] - 3, sy + sh - 3, 7, 1,
                                ARGB(0x38,120,255,255));
            gfx_fill_rect_alpha(dock_cx[i] - 2, sy + sh - 2, 5, 1,
                                ARGB(0x24,120,255,255));
            gfx_fill_rect_alpha(dock_cx[i] - 1, sy + sh - 1, 3, 1,
                                ARGB(0x14,120,255,255));
        }
    }

    /* tooltip above the hovered icon */
    if (dock_hover >= 0) {
        const char *label = dock_items[dock_hover].label;
        int tw = gfx_text_width(label);
        int bh = gfx_font_h() + 8;
        int bw = tw + 16;
        int bx = dock_cx[dock_hover] - bw / 2;
        int by = dock_shelf_y() - 30 - bh;

        if (bx < 2) bx = 2;
        if (bx + bw > scr_w - 2) bx = scr_w - 2 - bw;

        gfx_soft_shadow(bx, by, bw, bh, 5, 2);
        gfx_rounded_vgradient(bx, by, bw, bh, 5, RGB(64,64,70), RGB(30,30,34));
        gfx_rounded_outline(bx, by, bw, bh, 5, ARGB(0x60,255,255,255));
        gfx_text(label, bx + 8, by + (bh - gfx_font_h() + 1) / 2, RGB(245,245,250));
    }
}

void dock_click(int mx, int my)
{
    dock_layout();
    int sy = dock_shelf_y();
    if (my < sy - 40 || my > sy + dock_shelf_h()) return;

    for (int i = 0; i < DOCK_COUNT; i++) {
        int half = dock_sz[i] / 2 + 6;
        if (mx >= dock_cx[i] - half && mx < dock_cx[i] + half) {
            switch (dock_items[i].kind) {
            case DOCK_KIND_LP:
                /* Launchpad: тогглится, второй клик закрывает */
                lion_toggle_launchpad();
                break;
            case DOCK_KIND_ABOUT:
                launch_about();
                break;
            default:
                /*
                 * macOS dock behaviour: if the app is already
                 * running, clicking its icon focuses (and
                 * genie-restores) one of its windows instead of
                 * starting a second instance.
                 */
                if (app_focus_running(dock_items[i].elf) < 0)
                    request_launch(dock_items[i].elf);
                break;
            }
            return;
        }
    }
}

/*
 * ============================================================
 * Right-click context menu (Lion menu panel style)
 * ============================================================
 */

static const char *ctx_items[] = { "Refresh", "Space Wallpaper", "Gradient WP",
                                   "Photo WP", "Slideshow", "Next Accent", "About" };
#define CTX_COUNT 7
#define CTX_W     170
#define CTX_ITEM_H 20

void draw_context_menu(void)
{
    if (!ctx_menu_open) return;
    int h = CTX_COUNT * CTX_ITEM_H + 8;
    int cx = ctx_x, cy = ctx_y;
    if (cx + CTX_W > scr_w) cx = scr_w - CTX_W - 2;
    if (cy + h > scr_h) cy = scr_h - h - 2;

    draw_menu_panel(cx, cy, CTX_W, h);

    for (int i = 0; i < CTX_COUNT; i++) {
        int iy = cy + 4 + i * CTX_ITEM_H;
        if (mouse.x >= cx && mouse.x < cx + CTX_W &&
            mouse.y >= iy && mouse.y < iy + CTX_ITEM_H) {
            draw_blue_highlight(cx + 4, iy, CTX_W - 8, CTX_ITEM_H);
            gfx_text(ctx_items[i], cx + 12, iy + ity(), RGB(255,255,255));
        } else {
            gfx_text(ctx_items[i], cx + 12, iy + ity(), LION_MENU_TEXT);
        }
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
        case 1: wallpaper_mode = AX_WALL_SPACE; break;
        case 2: wallpaper_mode = AX_WALL_GRADIENT; break;
        case 3: if (photo_count > 0) wallpaper_mode = AX_WALL_PHOTO; break;
        case 4: if (photo_count > 1) wallpaper_mode = AX_WALL_SLIDESHOW; break;
        case 5: accent_idx = (accent_idx + 1) % 5; ax_set_accent(accent_cycle[accent_idx]); break;
        case 6: launch_about(); break;
    }
}

/*
 * ============================================================
 * About dialog -- "About This System" (brushed metal, Lion)
 * ============================================================
 */

void launch_about(void)
{
    about_open = true;
    about_x = scr_w/2 - AX_ABOUT_W/2;
    about_y = scr_h/2 - AX_ABOUT_H/2;
    about_anim = 0;
}

void draw_about(void)
{
    if (!about_open) return;
    int w = AX_ABOUT_W, h = AX_ABOUT_H;
    int alpha = 0xFF;
    if (about_anim < AX_FADE_FRAMES) {
        alpha = (about_anim * 0xFF) / AX_FADE_FRAMES;
        about_anim++;
    }

    gfx_soft_shadow(about_x, about_y, w, h, 12, 4);
    gfx_rounded_vgradient(about_x, about_y, w, h, 12,
                          RGB(248,248,250), RGB(226,226,232));
    gfx_rounded_outline(about_x, about_y, w, h, 12, ARGB(0x88,60,60,70));
    gfx_fill_rect_alpha(about_x + 2, about_y + 1, w - 4, 1, ARGB(0x70,255,255,255));

    /* dark system badge, like the menubar logo */
    gfx_rounded_vgradient(about_x + 28, about_y + 28, 44, 44, 9,
                          RGB(96,96,104), RGB(52,52,60));
    gfx_rounded_outline(about_x + 28, about_y + 28, 44, 44, 9, ARGB(0x50,255,255,255));
    gfx_text_scaled("X", about_x + 28 + 9, about_y + 28 + 7, RGB(250,250,252), 3);

    /* title, faked bold via a 1px double-draw */
    gfx_text_scaled(AX_OS_NAME, about_x + 92, about_y + 34, RGB(25,25,30), 3);
    gfx_text_scaled(AX_OS_NAME, about_x + 94, about_y + 34, RGB(25,25,30), 3);

    gfx_text("Mac OS X Lion Edition", about_x + 94, about_y + 64, accent);
    gfx_text("Shell: " AX_SHELL_NAME "  v" AX_VERSION, about_x + 94, about_y + 82, RGB(100,100,110));

    gfx_text("64-bit UNIX compatible", about_x + 28, about_y + 110, RGB(90,90,100));
    gfx_text("GPU: Software render", about_x + 28, about_y + 126, RGB(90,90,100));
    gfx_text("Powered by iXlinx and ArtyomX", about_x + 28, about_y + 142, RGB(120,120,130));

    /* separator + hint (alpha-blended; hline would paint it solid) */
    gfx_fill_rect_alpha(about_x + 24, about_y + h - 34, w - 48, 1, ARGB(0x30,0,0,0));
    gfx_text("[ click to close ]", about_x + 28, about_y + h - 24, RGB(150,150,158));

    if (alpha < 0xFF) {
        uint32_t fade_a = (uint32_t)(0xFF - alpha) << 24;
        gfx_fill_rect_alpha(about_x, about_y, w, h, fade_a);
    }
}

/*
 * ============================================================
 * Embedded Controller (батарея) -- перенос из kernel/main.c
 * ============================================================
 *
 * Сервер исполняется в ring 0 (как и весь код ОС), поэтому
 * EC-регистры ASUS читаются напрямую портами 0x62/0x66.
 */

#define EC_DATA_PORT    0x62
#define EC_CMD_PORT     0x66

#define EC_CMD_READ     0x80

#define EC_STAT_OBF     0x01
#define EC_STAT_IBF     0x02

static void ec_wait_write(void) {
    int timeout = 100000;
    while ((inb(EC_CMD_PORT) & EC_STAT_IBF) && timeout--) {
        __asm__ volatile("pause");
    }
}

static void ec_wait_read(void) {
    int timeout = 100000;
    while (!(inb(EC_CMD_PORT) & EC_STAT_OBF) && timeout--) {
        __asm__ volatile("pause");
    }
}

static uint8_t ec_read_register(uint8_t reg) {
    ec_wait_write();
    outb(EC_CMD_PORT, EC_CMD_READ);

    ec_wait_write();
    outb(EC_DATA_PORT, reg);

    ec_wait_read();
    return inb(EC_DATA_PORT);
}

/*
 * ============================================================
 * r12-bootfix: переносимая батарея (было: только ASUS)
 * ============================================================
 *
 * Регистр 0xB0 с процентом заряда -- специфика Embedded
 * Controller ноутбуков ASUS.  На других машинах:
 *   - EC может вообще отсутствовать (читаются 0xFF);
 *   - регистр 0xB0 может значить что-то другое.
 *
 * Правила переносимости:
 *   1. EC опрашивается ОДИН раз при старте; если ответ не похож
 *      на процент (0..100) -- порты 0x62/0x66 больше не трогаем
 *      вообще.
 *   2. Если EC есть, значение кэшируется и обновляется не чаще
 *      одного раза в 4096 обращений (примерно раз в минуту при
 *      60 fps), а не на каждой перерисовке меню-бара.
 */
static int  ec_probed   = 0;   /* тест-чтение уже выполнялось */
static int  ec_present  = 0;   /* EC отвечает процентом батареи */
static int  ec_value    = 0;
static long ec_cooldown = 0;

int get_battery_percentage_fast(void) {
    if (!ec_probed) {
        uint8_t percent;

        ec_probed = 1;

        percent = ec_read_register(0xb0);

        if (percent <= 100) {
            ec_present = 1;
            ec_value   = percent;
        } else {
            /* Не ASUS EC (или нет EC) -- больше не дёргаем порты. */
            ec_present = 0;
            ec_value   = 0;
        }

        ec_cooldown = 4096;
        return ec_value;
    }

    if (!ec_present)
        return 0;

    if (--ec_cooldown <= 0) {
        uint8_t percent;

        ec_cooldown = 4096;

        percent = ec_read_register(0xb0);

        if (percent <= 100) {
            ec_value = percent;
        } else {
            /* EC перестал отвечать -- отключаемся тихо. */
            ec_present = 0;
            ec_value   = 0;
        }
    }

    return ec_value;
}