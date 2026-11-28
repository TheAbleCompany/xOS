/*
 * ============================================================
 * Notes -- multi-window text editor for ArtyomX64
 * ============================================================
 *
 * macOS process model demo: ONE process owns MANY windows.
 *
 *   * Each window is an independent document with its own
 *     buffer, filename, dirty flag and Open-prompt state.
 *
 *   * ax_surface() is called once per window; the canvas
 *     handle identifies the window in every AX call.
 *
 *   * Closing one window (red button / File > Close Window)
 *     destroys just that window via ax_win_close().  The
 *     process exits only when its LAST window is gone.
 *
 *   * File > New Window (global menu) sends AX_EV_NEWWIN to
 *     the focused window -- the app opens one more window in
 *     the same process.
 *
 *   * App menu > Quit sends AX_EV_QUIT to every window --
 *     the app destroys all windows and exits (Cmd+Q).
 *
 * Editor keys:
 *
 *     Ctrl+S   save the document to the current file
 *     Ctrl+O   open a file (inline filename prompt)
 *     Ctrl+N   new document in the SAME window
 *     Esc      cancel the Open prompt
 *
 * The initial file name is taken from argv[1] ("notes test.txt"
 * from a terminal, or double click in Files).
 *
 * ============================================================
 */

#include "../lib/libax.h"
#include "../lib/axquartz.h"
#include "../lib/axaqua.h"
#include "../lib/unistd.h"
#include "../lib/axttf.h"

typedef unsigned long long  u64int;
typedef          long long  s64int;
typedef unsigned int        u32int;
typedef          int        s32int;
typedef unsigned short      u16int;
typedef          short      s16int;
typedef unsigned char       u8int;
typedef          char       s8int;

/*
 * open() flags (duplicated from unistd.h with guards).
 */
#ifndef O_RDONLY
#define O_RDONLY 0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY 0x0001
#endif
#ifndef O_CREAT
#define O_CREAT  0x0040
#endif
#ifndef O_TRUNC
#define O_TRUNC  0x0200
#endif

/*
 * Layout.  CW/CH are the INITIAL canvas size; the actual
 * dimensions live in g_cw/g_ch (per-window, set by bind()).
 * Macros below depend on g_cw/g_ch so they expand correctly
 * after ax_resize() changes the canvas resolution at runtime.
 *
 * Aqua layout: 24 px toolbar, white paper content area,
 * 20 px status bar (all drawn by the shared AquaKit).
 */

#define CW 360
#define CH 280

#define TOOL_H      24
#define TEXT_X0     10
#define TEXT_Y0     (TOOL_H + 12)
#define TEXT_XMAX   (g_cw - 16)
#define STATUS_H    20
#define TEXT_YMAX   (g_ch - STATUS_H - 4)
#define LINE_H      16

#define BUFFER_SIZE     8192
#define FILENAME_MAXLEN 63
#define PROMPT_MAXLEN   40

#define MAX_WINS 4

/* TrueType: размер шрифта документа (кнопки 11/13/16 в тулбаре). */
static int font_size = 13;
static int last_buttons = 0;

static int text_line_height(void)
{
    if (axttf_ready())
    {
        int lh = axttf_line_height(font_size, 0);

        if (lh > 6)
            return lh;
    }

    return LINE_H;
}

static int text_advance(char c)
{
    if (axttf_ready())
    {
        const axttf_glyph *g = axttf_get_glyph((unsigned char)c, font_size);

        if (g)
            return g->adv;
    }

    return 8;
}

static void draw_char_ttf(char c, int x, int y, uint32_t color)
{
    const axttf_glyph *g = axttf_get_glyph((unsigned char)c, font_size);
    int base;

    if (!g)
        return;

    base = y + axttf_ascent(font_size, 0);

    for (int yy = 0; yy < g->h; yy++)
    {
        const unsigned char *row = g->aa + yy * g->w;

        for (int xx = 0; xx < g->w; xx++)
        {
            int a = row[xx];

            if (a)
                aq_px_alpha(x + g->xoff + xx, base + g->yoff + yy,
                            ((uint32_t)a << 24) | (color & 0x00FFFFFF));
        }
    }
}

/* ============================================================
 * The app's OWN menu bar (macOS model: menus belong to the
 * application).  While a Notes window is focused, the global
 * bar shows exactly this File / View menu, and every click
 * arrives here as AX_EV_MENU (ev.key = menu, ev.w = item).
 *
 *   [0] "File"  -- New Window, Close Window, Save, Open...
 *   [1] "View"  -- resolution presets (drives ax_resize)
 * ============================================================ */

/* The 8-bit font only covers ASCII -- no fancy ellipsis. */
static const char *notes_file_items[] = {
    "New Window",
    "Close Window",
    "Save",
    "Open..."
};

static const char *notes_view_items[] = {
    "360x280",
    "480x360",
    "640x480",
    "800x600"
};

static const ax_menu_def_t notes_menus[] = {
    { "File", notes_file_items, 4 },
    { "View", notes_view_items, 4 },
};

static const ax_menubar_def_t notes_menubar = {
    "Notes",
    notes_menus,
    2
};

/*
 * Resolution presets, parallel to notes_view_items[].
 * Index 0 == "360x280" (the default), etc.  ax_resize()
 * returns the new canvas pointer; the old one is invalidated.
 */
static const int NOTES_VIEW_W[] = { 360, 480, 640, 800 };
static const int NOTES_VIEW_H[] = { 280, 360, 480, 600 };

/* ============================================================
 * Per-window state (the macOS model: one process, many windows)
 * ============================================================ */

typedef struct {
    int      used;
    uint32_t *canvas;                       /* window handle  */
    int      cw, ch;                        /* per-window canvas dims */

    char     buffer[BUFFER_SIZE];           /* document       */
    int      blen;
    char     current_file[FILENAME_MAXLEN + 1];
    int      dirty;

    int      prompt_mode;                   /* Open prompt    */
    char     prompt_buf[PROMPT_MAXLEN + 1];
    int      prompt_len;

    char     msg[48];                       /* status bar     */
    int      need_render;
} note_win;

static note_win wins[MAX_WINS];

static const char *APP_TITLE = "Notes";

/* ============================================================
 * Tiny helpers (freestanding -- no libc)
 * ============================================================ */

static int str_len(const char *s)
{
    int n = 0;

    while (s[n])
        n++;

    return n;
}

static void copy_str(char *dst, const char *src, int maxlen)
{
    int i;

    if (!dst || !src || maxlen <= 0)
        return;

    for (i = 0; i < maxlen - 1 && src[i]; i++)
        dst[i] = src[i];

    dst[i] = '\0';
}

static void set_msg(note_win *w, const char *s)
{
    int i;

    for (i = 0; i < (int)sizeof(w->msg) - 1 && s[i]; i++)
        w->msg[i] = s[i];

    w->msg[i] = '\0';
}

/* Bind libax globals to this window's canvas. */
static void bind(const note_win *w)
{
    g_canvas = w->canvas;
    g_cw     = w->cw;
    g_ch     = w->ch;
}

/* ============================================================
 * Document helpers
 * ============================================================ */

static void doc_clear(note_win *w)
{
    w->blen  = 0;
    w->dirty = 0;

    w->buffer[0] = '\0';
}

static void set_filename(note_win *w, const char *name)
{
    copy_str(w->current_file, name, FILENAME_MAXLEN + 1);
}

/*
 * Load a file into a window.
 * Returns 0 on success, -1 if the file could not be opened.
 */
static int load_file(note_win *w, const char *name)
{
    int fd;
    int n;

    fd = open((void *)name, O_RDONLY, 0);

    if (fd < 0)
        return -1;

    n = read(fd, w->buffer, BUFFER_SIZE - 1);

    close(fd);

    if (n < 0)
        n = 0;

    w->blen       = n;
    w->buffer[w->blen] = '\0';

    set_filename(w, name);

    w->dirty = 0;

    return 0;
}

/*
 * Save the window's document to current_file.
 * Returns 0 on success, -1 on failure.
 */
static int save_file(note_win *w)
{
    int fd;
    int written = 0;

    if (w->current_file[0] == '\0')
        return -1;

    fd = open(w->current_file, O_WRONLY | O_CREAT | O_TRUNC, 0);

    if (fd < 0)
        return -1;

    while (written < w->blen)
    {
        int n = write(fd, &w->buffer[written], w->blen - written);

        if (n <= 0)
        {
            close(fd);
            return -1;
        }

        written += n;
    }

    close(fd);

    w->dirty = 0;

    return 0;
}

/* ============================================================
 * Commands (Ctrl+key)
 * ============================================================ */

static void cmd_new(note_win *w)
{
    doc_clear(w);
    set_filename(w, "untitled.txt");
    w->prompt_mode = 0;
    set_msg(w, "New document");
}

static void cmd_save(note_win *w)
{
    if (w->current_file[0] == '\0')
    {
        set_filename(w, "untitled.txt");
    }

    if (save_file(w) == 0)
    {
        set_msg(w, "Saved: ");
        copy_str(w->msg + str_len(w->msg), w->current_file,
                 (int)sizeof(w->msg) - str_len(w->msg));
    }
    else
    {
        set_msg(w, "Save failed!");
    }

    w->prompt_mode = 0;
}

static void cmd_open_start(note_win *w)
{
    w->prompt_mode = 1;
    w->prompt_len  = 0;
    w->prompt_buf[0] = '\0';
    set_msg(w, "Open file:");
}

static void cmd_open_confirm(note_win *w)
{
    char name[FILENAME_MAXLEN + 1];

    w->prompt_mode = 0;

    if (w->prompt_len == 0)
    {
        set_msg(w, "Open cancelled");
        return;
    }

    copy_str(name, w->prompt_buf, FILENAME_MAXLEN + 1);

    if (load_file(w, name) == 0)
    {
        set_msg(w, "Opened: ");
        copy_str(w->msg + str_len(w->msg), name,
                 (int)sizeof(w->msg) - str_len(w->msg));
    }
    else
    {
        set_msg(w, "Not found: ");
        copy_str(w->msg + str_len(w->msg), name,
                 (int)sizeof(w->msg) - str_len(w->msg));
    }
}

/* ============================================================
 * Rendering
 * ============================================================ */

/*
 * Render one text line of the document onto the paper area.
 */
static void draw_char_safe(char ch, int x, int y, unsigned col)
{
    unsigned char c = (unsigned char)ch;

    /* The font table covers ASCII 0..127; draw 32..126 only. */
    if (c < 32 || c > 126)
        return;

    ax_char(ch, x, y, col);
}

/*
 * Count rendered lines (word-wrap identical to draw pass).
 */
static int count_lines(const note_win *w)
{
    int x     = TEXT_X0;
    int lines = 1;
    int i;

    for (i = 0; i < w->blen; i++)
    {
        char c = w->buffer[i];

        if (c == '\n')
        {
            lines++;
            x = TEXT_X0;
            continue;
        }

        if ((unsigned char)c < 32)
            continue;                       /* control chars: invisible */

        x += 8;

        if (x > TEXT_XMAX)
        {
            lines++;
            x = TEXT_X0;
        }
    }

    return lines;
}

static void render(note_win *w)
{
    int x, y;
    int i;
    int total_lines;
    int max_lines;
    int skip;

    bind(w);

    /* window body, toolbar, white paper area (AquaKit) */
    aq_window_bg();
    aq_toolbar(0, 0, g_cw, TOOL_H);
    aq_content_edge(0, TOOL_H, g_cw, g_ch - TOOL_H - STATUS_H);

    /* document name + dirty marker, centered in the toolbar */
    {
        char title[48];
        int  pos = 0;

        copy_str(title, "Notes - ", sizeof(title));
        pos = str_len(title);

        copy_str(title + pos, w->current_file,
                 (int)sizeof(title) - pos - 2);

        pos = str_len(title);

        if (w->dirty && pos < (int)sizeof(title) - 2)
            title[pos++] = '*';

        title[pos] = '\0';

        int tw = ax_text_width(title);
        int tx = (g_cw - tw) / 2;
        if (tx < 92) tx = 92;   /* не наезжать на переключатель кегля */
        aq_text(title, tx, 8, w->dirty ? AQ_ACCENT : AQ_TOOL_TEXT);
    }

    /* Font size (TrueType): склеенный переключатель 11 / 13 / 16. */
    {
        static const char *fs_lbl[3] = { "11", "13", "16" };
        int active = (font_size <= 11) ? 0 : (font_size >= 16) ? 2 : 1;
        int sx = 8, sw = 26, sh = 18, sy = 3;

        aq_segmented(sx, sy, sw * 3, sh, 3, active);

        for (int si = 0; si < 3; si++)
        {
            int tw = aq_text_width(fs_lbl[si]);

            aq_text(fs_lbl[si], sx + si * sw + (sw - tw) / 2,
                    sy + (sh - 8) / 2,
                    si == active ? AQ_TEXT : AQ_TEXT_SECOND);
        }
    }

    /* Text area: draw the tail that fits (auto scroll to cursor). */
    int lh = text_line_height();

    total_lines = count_lines(w);
    max_lines   = (TEXT_YMAX - TEXT_Y0) / lh;

    if (max_lines < 1)
        max_lines = 1;

    skip = total_lines - max_lines;

    if (skip < 0)
        skip = 0;

    x = TEXT_X0;
    y = TEXT_Y0;

    for (i = 0; i < w->blen; i++)
    {
        char c = w->buffer[i];

        if (c == '\n')
        {
            if (skip > 0) { skip--; }
            else          { y += lh; }

            x = TEXT_X0;
            continue;
        }

        if ((unsigned char)c < 32)
            continue;

        int adv = text_advance(c);

        if (x > TEXT_X0 && x + adv > TEXT_XMAX)
        {
            if (skip > 0) { skip--; }
            else          { y += lh; }

            x = TEXT_X0;
        }

        if (skip == 0 && y + lh <= TEXT_YMAX)
        {
            if (axttf_ready())
                draw_char_ttf(c, x, y, AQ_TEXT);
            else
                draw_char_safe(c, x, y, AQ_TEXT);
        }

        x += adv;
    }

    /* Cursor (always at the end of the document). */
    if (!w->prompt_mode && y + lh <= TEXT_YMAX)
        aq_fill(x, y, 2, lh, AQ_ACCENT);

    /* Open prompt: Lion-style sheet with an inset text field. */
    if (w->prompt_mode)
    {
        int fw = g_cw - 90;
        int fx = (g_cw - fw) / 2;
        int fy = TOOL_H + (g_ch - TOOL_H - STATUS_H) / 2 - 24;

        aq_panel(fx - 10, fy - 26, fw + 20, 66);
        aq_text_bold("Open file:", fx, fy - 16, AQ_TEXT);
        aq_field(fx, fy, fw, AQ_FIELD_H, w->prompt_buf,
                 w->prompt_len, 1);
        aq_text("Enter = open    Esc = cancel",
                fx, fy + AQ_FIELD_H + 6, AQ_TEXT_SECOND);
    }

    /* Status bar. */
    aq_statusbar(g_ch - STATUS_H, STATUS_H);

    if (w->prompt_mode)
    {
        aq_text("Type the file name",
                8, g_ch - 14, AQ_TEXT_SECOND);
    }
    else
    {
        if (w->msg[0])
            aq_text(w->msg, 8, g_ch - 14, AQ_TEXT);
        else
            aq_text("Ctrl+S Save   Ctrl+O Open   Ctrl+N New",
                    8, g_ch - 14, AQ_TEXT_SECOND);
    }
}

/* ============================================================
 * Key handling
 * ============================================================ */

static void handle_key(note_win *w, unsigned key)
{
    if (w->prompt_mode)
    {
        /* Ctrl combinations stay active inside the prompt. */
        if (key == AX_KEY_CTRL_S)  { cmd_save(w);            return; }
        if (key == AX_KEY_CTRL_O)  { w->prompt_mode = 0; set_msg(w, "Open cancelled"); return; }
        if (key == AX_KEY_CTRL_N)  { cmd_new(w);             return; }

        if (key == '\n')                    { cmd_open_confirm(w);    return; }
        if (key == 27)                      { w->prompt_mode = 0; set_msg(w, "Open cancelled"); return; }

        if (key == '\b')
        {
            if (w->prompt_len > 0)
                w->prompt_buf[--w->prompt_len] = '\0';

            return;
        }

        if (key >= 32 && key < 127 && w->prompt_len < PROMPT_MAXLEN)
        {
            w->prompt_buf[w->prompt_len++] = (char)key;
            w->prompt_buf[w->prompt_len]   = '\0';
        }

        /* Anything else is ignored in prompt mode. */
        return;
    }

    if (key == AX_KEY_CTRL_S) { cmd_save(w);      return; }
    if (key == AX_KEY_CTRL_O) { cmd_open_start(w); return; }
    if (key == AX_KEY_CTRL_N) { cmd_new(w);        return; }

    if (key == '\b')
    {
        if (w->blen > 0)
        {
            w->blen--;
            w->buffer[w->blen] = '\0';
            w->dirty = 1;
        }

        return;
    }

    if (key == '\n' || key == '\t' || (key >= 32 && key < 127))
    {
        if (w->blen < BUFFER_SIZE - 1)
        {
            w->buffer[w->blen++] = (char)key;
            w->buffer[w->blen]   = '\0';
            w->dirty = 1;
        }
        else
        {
            set_msg(w, "Buffer full!");
        }

        return;
    }

    /* All other control codes are ignored. */
}

/* ============================================================
 * Window management (the macOS model)
 * ============================================================ */

/*
 * Open one more window in THIS process.
 * Returns the new note_win or 0 when the process is full.
 */
static note_win *open_window(void)
{
    int i;

    for (i = 0; i < MAX_WINS; i++)
    {
        note_win *w = &wins[i];

        if (w->used)
            continue;

        w->canvas = ax_surface(APP_TITLE, CW, CH);
        ax_win_focus(w->canvas);
        w->cw     = CW;
        w->ch     = CH;

        if (!w->canvas)
            return 0;

        w->used = 1;
        w->blen = 0;
        w->buffer[0] = '\0';
        w->dirty = 0;
        w->prompt_mode = 0;
        w->prompt_len = 0;
        w->prompt_buf[0] = '\0';
        w->msg[0] = '\0';
        set_filename(w, "untitled.txt");
        w->need_render = 1;

        return w;
    }

    return 0;
}

/*
 * Destroy one window (its content is already unsaved-lost;
 * the process keeps running while other windows remain).
 */
static void close_window(note_win *w)
{
    ax_win_close(w->canvas);

    w->used   = 0;
    w->canvas = 0;
}

/* Destroy every window and leave the process. */
static void quit_all(void)
{
    int i;

    for (i = 0; i < MAX_WINS; i++)
    {
        if (wins[i].used)
            close_window(&wins[i]);
    }
}

/* ============================================================
 * Entry point
 * ============================================================ */

/*
 * Kernel passes exec_args_t* in rdi:
 *
 *     typedef struct { int argc; char **argv; } exec_args_t;
 */
typedef struct exec_args
{
    int    argc;
    char **argv;
} exec_args_t;

void _start(void *raw_args)
{
    exec_args_t *args = (exec_args_t *)raw_args;

    note_win *main_win = open_window();

    if (!main_win)
    {
        for (;;)
            yield();
    }

    /*
     * Register the app's own menu bar (macOS model).  From now
     * on the global bar shows Notes' File menu while a Notes
     * window is focused, and menu clicks arrive as AX_EV_MENU.
     */
    ax_menu_set(&notes_menubar);

    /*
     * argv[1] -- file to open (from xsh / temu: "notes test.txt",
     * or from Files: double click on a text file).
     */
    if (args &&
        args->argc >= 2 &&
        args->argv &&
        args->argv[1] &&
        args->argv[1][0])
    {
        char name[FILENAME_MAXLEN + 1];

        copy_str(name, args->argv[1], FILENAME_MAXLEN + 1);

        if (load_file(main_win, name) == 0)
        {
            set_msg(main_win, "Opened: ");
            copy_str(main_win->msg + str_len(main_win->msg), name,
                     (int)sizeof(main_win->msg) - str_len(main_win->msg));
        }
        else
        {
            /*
             * File does not exist yet -- start an empty
             * document bound to that name; the file is
             * created on first Ctrl+S.
             */
            set_filename(main_win, name);
            set_msg(main_win, "New file: ");
            copy_str(main_win->msg + str_len(main_win->msg), name,
                     (int)sizeof(main_win->msg) - str_len(main_win->msg));
        }
    }

    render(main_win);
    ax_commit(main_win->canvas);

    /*
     * Main loop: pump every window of the process.
     * The process exits only when the LAST window is gone.
     */
    for (;;)
    {
        int live = 0;
        int i;

        for (i = 0; i < MAX_WINS; i++)
        {
            note_win *w = &wins[i];

            if (!w->used)
                continue;

            ax_event ev;

            while (ax_poll(w->canvas, &ev) > 0)
            {
                if (ev.type == AX_EV_CLOSE)
                {
                    /* This window only -- the process stays. */
                    close_window(w);
                    break;
                }

                if (ev.type == AX_EV_QUIT)
                {
                    /* Cmd+Q: destroy every window, then exit. */
                    quit_all();
                    return;
                }

                if (ev.type == AX_EV_NEWWIN)
                {
                    /* File > New Window: one more window here. */
                    note_win *nw = open_window();

                    if (nw)
                        set_msg(nw, "New window");
                    break;
                }

                if (ev.type == AX_EV_MENU)
                {
                    /*
                     * The app's own menu (registered below):
                     * ev.key = menu index, ev.w = item index.
                     * Acts on the window that had focus.
                     */
                    if (ev.key == 0)   /* File */
                    {
                        switch (ev.w)
                        {
                            case 0:                 /* New Window */
                            {
                                note_win *nw = open_window();

                                if (nw)
                                    set_msg(nw, "New window");
                                break;
                            }
                            case 1:                 /* Close Window */
                                close_window(w);
                                break;
                            case 2:                 /* Save */
                                cmd_save(w);
                                w->need_render = 1;
                                break;
                            case 3:                 /* Open... */
                                cmd_open_start(w);
                                w->need_render = 1;
                                break;
                        }
                    }
                    

                    break;
                }

                /*
                 * Kernel-initiated resize (e.g. maximize/restore
                 * from the title bar, or future drag-resize).
                 *
                 * CRITICAL: ev.w / ev.h are the NEW window dims,
                 * but the kernel's toggle_maximize() does NOT
                 * reallocate the canvas -- it only changes w->w/h.
                 * The canvas stays at the OLD (small) size, and
                 * the kernel's draw_surface() blits it SCALED
                 * (that's the blurry effect in fullscreen).
                 *
                 * To actually use the new resolution we have to
                 * call ax_resize() ourselves.  It returns the NEW
                 * canvas pointer (the OLD w->canvas is freed and
                 * INVALIDATED afterwards).  If we updated
                 * w->cw/w->ch without resizing, render() would
                 * write past the end of the small canvas and
                 * corrupt the kernel heap (that was the GPF).
                 *
                 * If ax_resize fails (OOM), keep the OLD canvas
                 * and OLD dims -- the kernel will keep blitting
                 * scaled, which is blurry but at least doesn't
                 * crash.
                 */
              
                if (ev.type == AX_EV_MOUSE)
                {
                    if (ev.buttons && !last_buttons &&
                        ev.my < TOOL_H &&
                        ev.mx >= 8 && ev.mx < 8 + 78)
                    {
                        static const int fs_tab[3] = { 11, 13, 16 };
                        int seg = (ev.mx - 8) / 26;

                        if (seg >= 0 && seg < 3)
                            font_size = fs_tab[seg];

                        w->need_render = 1;
                    }

                    last_buttons = (int)ev.buttons;
                }

                if (ev.type == AX_EV_KEY)
                {
                    handle_key(w, ev.key);
                    w->need_render = 1;
                }
            }

            if (!w->used)
                continue;   /* just closed -- skip render */

            live++;

            if (w->need_render)
            {
                w->need_render = 0;
                render(w);
                ax_commit(w->canvas);
            }
        }

        /*
         * No windows left -> the process exits.  In the macOS
         * model the window manager already reaped everything
         * the process owned.
         */
        if (live == 0)
            return;

        yield();
    }
}
