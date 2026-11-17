#ifndef WSERVER_INTERNAL_H
#define WSERVER_INTERNAL_H

/*
 * ============================================================
 * wserver_internal.h
 * ============================================================
 *
 * Private header of the WindowServer (wserver) -- the separate
 * GUI server process, ArtyomX64's analogue of WindowServer in
 * MacOS X / NeXTSTEP.
 *
 * The server owns ALL of the GUI state:
 *
 *   wserver.c            entry + main loop + compose + serial
 *   wserver_surface.c    window table + client request handlers
 *   wserver_wallpaper.c  wallpaper / photo / BMP loader
 *   wserver_ui.c         widgets, menubar, dock, menus, about
 *   wserver_input.c      mouse + keyboard + cursor
 *   wserver_launch.c     app launching (exec) + focus matching
 *   wserver_menu.c       per-app registered menu bars
 *   wserver_axgl.c       AxGL (mini-OpenGL) software rasterizer
 *
 * Clients (apps) never touch this state directly: everything
 * goes through the "wserver" message port (lib/axport.h).
 *
 * The server is a plain userspace ELF (like any app), but it
 * runs at ring 0 in this OS, so driver-style port I/O (serial,
 * RTC, EC battery) is done inline, exactly like the old
 * kernel-resident shell did.
 */

#include "wserver_gfx.h"

#include "../lib/common.h"
#include "../lib/axipc.h"
#include "../lib/axport.h"

/*
 * libc-обвязки (unistd.h) включаются РОВНО в одну единицу
 * трансляции -- wserver.c (это определения, а не прототипы).
 * Остальные файлы сервера видят их через extern-объявления ниже.
 */
int  u_read(int fd, void *buf, unsigned long count);
int  u_write(int fd, const void *buf, unsigned long count);
int  u_open(void *path, int flags, int mode);
int  u_close(int fd);

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * ============================================================
 * Shared colour + layout macros
 * ============================================================
 */

#define ARGB(a,r,g,b) (((uint32_t)(a)<<24)|((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))
#define RGB(r,g,b)    ARGB(0xFF,r,g,b)

#define AX_RESIZE_GRIP 14
#define AX_WIN_MIN_W   160
#define AX_WIN_MIN_H   100

#define AX_FADE_FRAMES  18

/*
 * ============================================================
 * Mac OS X Lion design language ("Lion Edition" userspace)
 * ============================================================
 *
 * Палитра В ТОЧНОСТИ та же, что в lib/axaqua.h (AQ_*-токены
 * зеркалят её) -- приложение, нарисованное AquaKit'ом, выглядит
 * бесшовным продолжением хрома сервера.
 */

/* menu bar (translucent light bar over the blurred wallpaper) */
#define LION_MBAR_TOP      ARGB(0xC8,250,250,252)
#define LION_MBAR_BOT      ARGB(0xBE,225,225,231)
#define LION_MBAR_TEXT     RGB(45,45,50)
#define LION_MBAR_TEXT_DIM RGB(120,120,128)
#define LION_MBAR_SHADOW   ARGB(0x50,0,0,0)

/* drop-down menus / context menus (white translucent panels) */
#define LION_MENU_BG_TOP   ARGB(0xF2,252,252,254)
#define LION_MENU_BG_BOT   ARGB(0xEC,242,242,246)
#define LION_MENU_TEXT     RGB(35,35,40)
#define LION_MENU_DISABLED RGB(160,160,168)
#define LION_MENU_BORDER   ARGB(0x66,40,40,50)

/* selected-item highlight: the blue vertical gradient of the
 * Lion CSS3 reconstruction (items AND open menu-bar titles) */
#define LION_MENU_HL_TOP   RGB(119,147,245)   /* #7793F5 */
#define LION_MENU_HL_MID   RGB(64,109,242)    /* #406DF2 */
#define LION_MENU_HL_BOT   RGB(39,90,241)     /* #275AF1 */
#define LION_MENU_HL_EDGE  RGB(23,70,215)     /* #1746D7 */

/*
 * Window titlebar (brushed-metal light gradient).
 * Тайтлбар из реконструкции: #E9E9E9 -> #B2B2B2 у активного
 * окна, заголовок #3C3C3C с эмбоссом #E7E7E7, рамка окна
 * rgba(0,0,0,.3); неактивное окно светлее и холоднее.
 */
#define LION_TITLE_F_TOP   RGB(233,233,233)   /* #E9E9E9 */
#define LION_TITLE_F_BOT   RGB(178,178,178)   /* #B2B2B2 */
#define LION_TITLE_U_TOP   RGB(238,238,240)
#define LION_TITLE_U_BOT   RGB(214,214,220)
#define LION_TITLE_TEXT    RGB(60,60,60)      /* #3C3C3C */
#define LION_TITLE_TEXT_U  RGB(150,150,158)
#define LION_FRAME_EDGE    ARGB(0x4D,0,0,0)   /* rgba(0,0,0,.3) */

/*
 * Aqua traffic lights (focused / dimmed-unfocused variants).
 * Градиенты кнопок из демо: #FD4E4E, #F3BB55, #96D16F с тёмными
 * кромками; серые -- для неактивного окна.
 */
#define LION_RED_TOP       RGB(254,94,94)
#define LION_RED_BOT       RGB(248,64,64)
#define LION_RED_RIM       RGB(139,26,23)
#define LION_YEL_TOP       RGB(245,195,95)
#define LION_YEL_BOT       RGB(238,178,62)
#define LION_YEL_RIM       RGB(148,98,25)
#define LION_GRN_TOP       RGB(158,216,122)
#define LION_GRN_BOT       RGB(142,201,98)
#define LION_GRN_RIM       RGB(56,107,38)
#define LION_GRAY_TOP      RGB(186,186,188)
#define LION_GRAY_BOT      RGB(176,176,178)
#define LION_GRAY_RIM      RGB(122,122,126)
#define LION_BTN_GLYPH     ARGB(0xB4,80,50,40)

/* dock -- translucent glass shelf (the wallpaper reads through) */
#define LION_DOCK_TOP      ARGB(0x60,255,255,255)
#define LION_DOCK_BOT      ARGB(0x4A,222,226,240)
#define LION_DOCK_EDGE     ARGB(0x50,255,255,255)
#define LION_DOCK_UNDERSHD ARGB(0x3C,10,10,20)
#define LION_DOCK_LABEL_BG ARGB(0xE0,32,32,36)

#define AX_WALL_GRADIENT  0
#define AX_WALL_PHOTO     1
#define AX_WALL_SLIDESHOW 2
/* Lion Andromeda: procedural deep-space wallpaper (stars + nebula) */
#define AX_WALL_SPACE     3

/* About dialog ("About This System") */
#define AX_ABOUT_W 380
#define AX_ABOUT_H 236

/*
 * Window animation kinds (ax_surface_win.anim_kind):
 *   0 = idle, 1 = zoom-open, 2 = minimize-to-dock, 3 = restore
 */
#define AX_ANIM_NONE   0
#define AX_ANIM_OPEN   1
#define AX_ANIM_MINI   2
#define AX_ANIM_RESTR  3
#define AX_ANIM_FRAMES 14

/*
 * Lion traffic-light geometry, shared by the renderer
 * (wserver_surface.c) and the click router (wserver_input.c):
 * three 7px buttons with centers at +19/+39/+59 in the titlebar.
 */
#define AX_BTN_R      7
#define AX_BTN_C1     19
#define AX_BTN_C2     39
#define AX_BTN_C3     59

/* Height of one drop-down menu item (shared draw + click paths). */
#define AX_MENU_ITEM_H 20

/*
 * ============================================================
 * Mac OS X Lion system overlays (server/wserver_lion.c)
 * ============================================================
 *
 * Процедурные Aqua-иконки -- общий каталог для дока (wserver_ui.c)
 * и Launchpad (wserver_lion.c): один kind -> одна картинка.
 */
#define LION_ICON_TERMINAL  0
#define LION_ICON_FOLDER    1
#define LION_ICON_NOTES     2
#define LION_ICON_CALC      3
#define LION_ICON_GL        4
#define LION_ICON_PREFS     5
#define LION_ICON_DOOM      6
#define LION_ICON_QUAKE     7
#define LION_ICON_ABOUT     8
#define LION_ICON_RAYCAST   9
#define LION_ICON_BENCH     10
#define LION_ICON_GUITEST   11
#define LION_ICON_LAUNCHPAD 12

void lion_icon_tile(int kind, int x, int y, int s, uint32_t color);

/* Launchpad / Mission Control (wserver_lion.c). */
bool lion_launchpad_open(void);
bool lion_mission_open(void);
bool lion_overlay_open(void);
void lion_toggle_launchpad(void);
void lion_toggle_mission(void);
void lion_overlays_close(void);
bool lion_system_key(int sc);          /* true = клавиша съедена */
void lion_overlay_click(int mx, int my);
void lion_overlays_draw(void);

#define MAX_PHOTOS 8
#define SLIDESHOW_INTERVAL 600
#define WALLPAPER_COUNT 5

/* Kernel tty service (pty) -- терминальные окна держат id. */
#define AX_TTY_NONE (-1)

/*
 * Window-server geometry (было в kernel/axshell.h).
 */
#define AX_OS_NAME    "xOS"
#define AX_SHELL_NAME "AXWS (WindowServer)"
#define AX_VERSION    "3.0 Aqua"

#define AX_MAX_WINDOWS 16
#define AX_TITLEBAR_H  28
#define AX_MENUBAR_H   24
#define AX_DOCK_H      56

typedef struct {
    int  x, y;
    int  prev_x, prev_y;
    bool left, right;
    bool prev_left, prev_right;
    bool clicked;
    bool released;
} ax_mouse_state;

/*
 * ============================================================
 * Shared types
 * ============================================================
 */

typedef struct {
    uint32_t *pixels;
    int w, h;
    bool valid;
} ax_photo_t;

/*
 * The on-screen window descriptor (SERVER-side).
 *
 * Отличие от старой ядровой версии: очередь событий ev_queue
 * заменена портом клиента (client_port) -- события уходят
 * клиенту СООБЩЕНИЯМИ через ядро, как в Mach.  canvas по-прежнему
 * разделяемая память: клиент рисует сам, сервер композитит.
 */
typedef struct {
    int  id;
    bool used;
    bool visible;
    bool focused;
    bool minimized;
    bool maximized;
    bool fullscreen;   /* Lion fullscreen: окно занимает весь экран */
    bool dragging;
    bool resizing;
    bool closing;      /* window asked to close, waiting for task exit */
    int  owner_pid;    /* which task owns this window                  */
    int  client_port;  /* порт клиента -- туда летят события           */
    int  tty;          /* id tty (pty) терминального окна или -1       */
    int  x, y, w, h;
    int  restore_x, restore_y, restore_w, restore_h;
    int  fs_x, fs_y, fs_w, fs_h;   /* геометрия до fullscreen          */
    int  z;
    int  drag_dx, drag_dy;
    char title[48];
    char app_name[20]; /* display name of the owning app (menu bar) */
    uint32_t *canvas;
    int       canvas_w, canvas_h;
    int anim;          /* legacy fade-in counter (AX_FADE_FRAMES)  */
    int anim_kind;     /* AX_ANIM_*: what the window is doing now  */
    int anim_t;        /* frame counter of the current animation   */
    bool dirty;
} ax_surface_win;

typedef struct {
    int x, y;
    bool dragging;
    int drag_dx, drag_dy;
    bool visible;
} ax_widget_state;

typedef struct {
    const char *label;
    int x_pos;
    int w;
    bool open;
    const char **items;
    int item_count;
} ax_menu_t;

/*
 * ============================================================
 * Shared globals (defined in the file that owns each one)
 * ============================================================
 */

/* wserver.c -- main */
extern int scr_w, scr_h;
extern ax_mouse_state mouse;
extern int srv_port;          /* порт "wserver" */
extern const char *WSERVER_PORT_NAME;
extern int ax_frame;          /* счётчик кадров (double-click и т.п.) */

void serial_putc_ax(char c);
void serial_puts_ax(const char *s);
void compose(void);

/* wserver_surface.c */
extern ax_surface_win surfaces[AX_MAX_WINDOWS];
extern int z_counter;
extern int active_surface;

ax_surface_win *surf_at(int x, int y);
void surf_focus(ax_surface_win *w);
void surf_push_event(ax_surface_win *w, ax_event ev);
int  surf_alloc(const char *title, int w, int h, int client_port);
void draw_surface(ax_surface_win *w);
void reap_closed_windows(void);
void toggle_maximize(ax_surface_win *w);
void toggle_fullscreen(ax_surface_win *w);   /* Lion green button / F11 */
ax_surface_win *surf_fullscreen_active(void);
void surf_destroy(ax_surface_win *w);
ax_surface_win *surf_active_window(void);
void app_request_newwin(void);
void app_request_quit(int owner_pid);
ax_surface_win *surf_by_token(uint32_t token);

/* Диспетчер клиентских запросов (wserver_surface.c). */
void wserver_handle_requests(void);
void wserver_dispatch(const ax_msg_t *m);

/* wserver_wallpaper.c */
extern uint32_t accent;
extern int wallpaper_index;
extern int wallpaper_mode;
extern ax_photo_t photos[MAX_PHOTOS];
extern int photo_count;
extern int current_photo;
extern int slideshow_counter;
extern int wallpaper_fade;
extern int wallpaper_fade_dir;
extern int prev_wallpaper_index;
extern int prev_photo_index;

void draw_wallpaper(void);
void wallpaper_tick(void);
void scan_photos(void);
void ax_set_wallpaper(int index);
int  ax_get_wallpaper(void);
void ax_set_accent(uint32_t color);

/* wserver_ui.c */
extern ax_widget_state w_clock;
extern ax_widget_state w_cal;
extern ax_widget_state w_sysmon;
extern ax_widget_state *widgets[];
#define WIDGET_COUNT 3
extern int dragging_widget;
extern ax_menu_t menus[8];
extern int menu_count;
extern int open_menu;
extern bool ctx_menu_open;
extern int  ctx_x, ctx_y;
extern uint32_t cursor_bg[16][16];
extern int cursor_old_x, cursor_old_y;
extern char prev_key_char;
extern bool about_open;
extern int  about_x, about_y;
extern int  about_anim;

void draw_widgets(void);
bool point_in_widget(ax_widget_state *ws, int mx, int my);
void init_menus(void);
void draw_menubar(void);
void menu_click(int mx, int my);
void draw_dock(void);
void dock_click(int mx, int my);
void draw_context_menu(void);
void context_menu_click(int mx, int my);
void launch_about(void);
void draw_about(void);
void draw_cursor(int x, int y);
void ax_time_now(ax_time_t *t);
int  int_to_str(int value, char *buf, size_t len);
void system_power_sleep(void);
void reboot(void);
void system_power_off(void);
int  get_battery_percentage_fast(void);

/* wserver_input.c */
void update_mouse(void);
void process_input(void);

/* wserver_launch.c */
extern char     pending_launch[32];
extern volatile int launch_request;
void request_launch(const char *elf);
void app_trampoline(void);
int  app_focus_running(const char *elf_name);

/* Lion dock cooperation (wserver_surface.c + wserver_ui.c). */
bool app_is_running(const char *elf_name);
int  dock_icon_cx_by_elf(const char *elf_name);   /* -1 when absent */
void win_minimize_anim(ax_surface_win *w);        /* genie-to-dock  */

/* Общие сисколл-обвязки (wserver_surface.c). */
int  exec_sys(const char *path, int argc, char *argv[]);
int  proc_name_sys(int pid, char *buf, int len);

/* wserver_menu.c -- per-app registered menu bars.
 * Layout must stay in sync with the AX_* limits from axipc.h. */
typedef struct {
    char title[AX_MENU_TITLE_MAX];
    int  item_count;
    char item_text[AX_MENU_ITEMS_MAX][AX_MENU_ITEM_MAX];
    const char *items[AX_MENU_ITEMS_MAX];
} ax_app_menu_t;

typedef struct {
    int  pid;
    char app_name[20];
    int  menu_count;
    ax_app_menu_t menus[AX_APP_MENUS_MAX];
} ax_app_menubar_t;

int               menu_set_wire(int pid, const ax_menubar_wire_t *w);
ax_app_menubar_t *menu_lookup(int pid);
void              menu_drop(int pid);

/* wserver_axgl.c */
int  axgl_backend_name(char *out, int cap);
int  axgl_set_mode(int mode);
int  axgl_begin(uint32_t token, int w, int h);
int  axgl_triangles(uint32_t token, const void *verts, uint32_t count);

/*
 * ============================================================
 * Raw port I/O + driver-level helpers available to the server
 * (ring 0 in this OS, как и весь остальной код).
 * ============================================================
 */

extern u8int  inb(u16int port);
extern void   outb(u16int port, u8int val);
extern void   outw(u16int port, u16int val);

/* Keyboard driver state (drivers/keyboard.c) -- INPUT_KEY. */
extern int    ax_shift_pressed;
extern int    is_caps_pressed;
extern unsigned char keyboard_map[128];
extern unsigned char keyboard_map_caps[128];
extern unsigned char keyboard_map_shift[128];

/*
 * User-ELF address boundary (multitask/task.h).  Duplicated here
 * so server files don't need to pull in the full task.h.
 */
#define AX_USER_VA_MAX 0x0000800000000000ULL

#endif /* WSERVER_INTERNAL_H */
