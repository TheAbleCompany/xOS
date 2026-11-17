#ifndef AXSHELL_INTERNAL_H
#define AXSHELL_INTERNAL_H

/*
 * ============================================================
 * axshell_internal.h
 * ============================================================
 *
 * Private header shared by the axshell split files.
 *
 * The public axshell.h exposes only the entry point and the
 * small public surface (wallpaper / accent setters, mouse/key
 * externs).  Everything below -- the window manager state, the
 * widget structs, the menu tables, the cross-file helpers -- is
 * internal to the axshell implementation and must not be
 * included from outside kernel/axshell*.c.
 *
 * The split mirrors the syscalls.c -> open.c / rw.c / exec.c
 * split: each subsystem owns its own .c file and exposes the
 * pieces the other subsystems need through this header.
 *
 *   axshell.c            main loop + compose + serial helpers
 *   axshell_surface.c    window manager + AX syscalls
 *   axshell_wallpaper.c  wallpaper / photo / BMP loader
 *   axshell_ui.c         widgets, menubar, dock, menus, about
 *   axshell_input.c      mouse + keyboard + cursor
 *   axshell_launch.c     app launch trampoline
 */

#include "axshell.h"

#include "../lib/gfxlib.h"

/* axshell.h defines AX_KEY_ENTER as '\n'; axipc.h defines it as 13.
 * Undef before pulling in axipc.h so axipc.h's value (13, which is
 * what user-space apps see) wins cleanly without a redefinition
 * warning.  The original monolithic axshell.c got the same effect
 * by include-ordering, but emitted a warning each build. */
#ifdef AX_KEY_ENTER
#undef AX_KEY_ENTER
#endif
#ifdef AX_KEY_BACK
#undef AX_KEY_BACK
#endif
#ifdef AX_KEY_ESC
#undef AX_KEY_ESC
#endif

#include "../lib/axipc.h"
#include "../lib/common.h"
#include "../dev/pty.h"
#include "../fs/fs.h"

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

#define AX_WALL_GRADIENT  0
#define AX_WALL_PHOTO     1
#define AX_WALL_SLIDESHOW 2

#define MAX_PHOTOS 8
#define SLIDESHOW_INTERVAL 600
#define WALLPAPER_COUNT 5

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
 * The on-screen window descriptor.  Distinct from the legacy
 * ax_window in axshell.h (which is unused inside the shell).
 */
typedef struct {
    int  id;
    bool used;
    bool visible;
    bool focused;
    bool minimized;
    bool maximized;
    bool dragging;
    bool resizing;
    bool closing;      /* window asked to close, waiting for task exit */
    int  owner_pid;    /* which task owns this window */
    int  x, y, w, h;
    int  restore_x, restore_y, restore_w, restore_h;
    int  z;
    int  drag_dx, drag_dy;
    char title[48];
    char app_name[20]; /* display name of the owning app (menu bar) */
    ax_app_kind kind;
    uint32_t *canvas;
    int       canvas_w, canvas_h;
    ax_event  ev_queue[16];
    int       ev_head, ev_tail;
    int anim;
    bool dirty;
    ax_pty_t *pty;
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

/* axshell.c -- main */
extern int scr_w, scr_h;
extern ax_mouse_state mouse;
extern char     pending_launch[32];
extern volatile int launch_request;

/* axshell_surface.c */
extern ax_surface_win surfaces[AX_MAX_WINDOWS];
extern int z_counter;
extern int active_surface;

/* axshell_wallpaper.c */
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

/* axshell_ui.c */
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

/* axshell_launch.c */
extern void *app_entry_ptr;
extern uint32_t *app_canvas_ptr;

/* axshell_menu.c -- per-app registered menu bars.
 * Layout must stay in sync with the AX_* limits from axipc.h.
 * item_text holds the copies; items[] points into item_text so
 * the menubar renderer (which wants const char **) can walk
 * the labels without type confusion. */
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

int               ax_syscall_menu_set(const ax_menubar_def_t *udef);
ax_app_menubar_t *menu_lookup(int pid);
void              menu_drop(int pid);

/*
 * ============================================================
 * Cross-file function declarations
 * ============================================================
 */

/* axshell.c -- serial helpers + main */
void serial_putc_ax(char c);
void serial_puts_ax(const char *s);
void compose(void);

/* axshell_surface.c */
ax_surface_win *surf_at(int x, int y);
void surf_focus(ax_surface_win *w);
void surf_push_event(ax_surface_win *w, ax_event ev);
int  surf_alloc(const char *title, int w, int h, ax_app_kind kind);
void draw_surface(ax_surface_win *w);
void reap_closed_windows(void);
void toggle_maximize(ax_surface_win *w);
void surf_destroy(ax_surface_win *w);
ax_surface_win *surf_active_window(void);
void app_request_newwin(void);
void app_request_quit(int owner_pid);

int64_t ax_syscall_surface(const char *title, int w, int h);
int ax_syscall_term_read(uint32_t canvas_ptr, uint8_t *buf, uint32_t maxlen);
int ax_syscall_term_write(uint32_t canvas_ptr, const uint8_t *buf, uint32_t len);
int ax_syscall_poll(uint32_t canvas_ptr, ax_event *out);
int ax_syscall_commit(uint32_t canvas_ptr);
int ax_syscall_time(ax_time_t *out);
int ax_syscall_screen(ax_screen_t *out);
int ax_syscall_win_close(uint32_t canvas_ptr);
int ax_syscall_win_focus(uint32_t canvas_ptr);

/*
 * AX_SYS_WIN_RESIZE -- an app changes the canvas resolution of ONE
 * of its own windows.  The kernel:
 *   - finds the surface whose canvas ptr matches `canvas_ptr`,
 *   - validates that the caller is the owner (runningTask->pid),
 *   - clamps (w,h) to the screen + AX_WIN_MIN_* limits,
 *   - reallocates the canvas buffer (kfree old, kmalloc new),
 *   - updates s->canvas / canvas_w / canvas_h / w / h,
 *   - pushes an AX_EV_RESIZE event back so the app re-layouts.
 *
 * Returns the NEW canvas pointer (cast to int64_t).  The OLD
 * pointer is INVALID after this call -- the app MUST replace its
 * stored canvas handle with the return value before drawing again.
 *
 * Returns 0 (== NULL) on failure (not found / not owner /
 * allocation failed / bad dimensions); the app keeps its old
 * canvas unchanged in that case.
 */
int64_t ax_syscall_win_resize(uint32_t canvas_ptr, int w, int h);

void ax_send_event_to_canvas(uint32_t canvas_ptr, ax_event *ev);
void ax_send_event_to_active(ax_event *ev);

/* axshell_wallpaper.c */
void draw_wallpaper(void);
void wallpaper_tick(void);
void scan_photos(void);
/* ax_set_wallpaper / ax_get_wallpaper / ax_set_accent are public
 * (declared in axshell.h). */

/* axshell_ui.c */
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

/* axshell_input.c */
void update_mouse(void);
void process_input(void);

/* axshell_launch.c */
void request_launch(const char *elf);
void app_trampoline(void);
int  app_focus_running(const char *elf_name);

/*
 * ============================================================
 * External kernel helpers used across the split files
 * ============================================================
 */

extern u8int  inb(u16int port);
extern void   outb(u16int port, u8int val);
extern void   outw(u16int port, u16int val);
extern void   handle_mouse(void);
extern int    exec(const char *path);
extern void   yield(void);
extern void  *image_load(char *elf_start, unsigned int size);
extern u32int read_fs(fs_node_t *node, u32int offset, u32int size, u8int *buffer);
extern fs_node_t *finddir_fs(fs_node_t *node, char *name);
extern fs_node_t *fs_root;
extern void   debug_putnum(uint64_t n);
extern void   debug_puts(const char *s);
extern void   debug_putc(char c);
extern int    get_battery_percentage_fast(void);
extern int    keyboard_pop_event(void);
extern void   keyboard_set_pty(ax_pty_t *pty);

extern int    is_caps_pressed;
extern int    ax_shift_pressed;
extern unsigned char keyboard_map[128];
extern unsigned char keyboard_map_caps[128];
extern unsigned char keyboard_map_shift[128];

extern int    m_cursor_x;
extern int    m_cursor_y;
extern int    mouse_buttons;
extern char   current_key;
extern int    is_enter_pressed;

/*
 * User-ELF address boundary (multitask/task.h).  Duplicated here
 * so axshell_launch.c does not need to pull in the full task.h
 * (which would clash with fs/task.h's identical typedefs).
 */
#define AX_USER_VA_MAX 0x0000800000000000ULL
extern void aximage_pending_discard(void);

#endif /* AXSHELL_INTERNAL_H */
