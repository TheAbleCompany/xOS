
#include "../lib/libax.h"
#include "../lib/axquartz.h"
#include "../lib/axaqua.h"
#include "../lib/unistd.h"
#include "../fs/fs.h"

#define CW 460
#define CH 300

#define MAX_FILES 64
#define NAME_MAX  127
#define PATH_MAX  256

typedef struct
{
    uint32_t ino;
    uint16_t reclen;
    uint8_t  type;
    char     name[1];
} __attribute__((packed)) ax_dirent;


/* ============================================================
 * FILE LIST
 * ============================================================ */

typedef struct {
    char name[NAME_MAX + 1];
    char path[PATH_MAX];
    int  is_dir;
    int  ptype;
} file_entry_t;

static file_entry_t files[MAX_FILES];

static int nfiles;

static int sel = -1;
static int last_sel = -1;

static int last_buttons;

static char status[48] = "Loading...";

/* Текущий каталог */
static char current_root[PATH_MAX] = "disk0";


/* ============================================================
 * WINDOW / GRID LAYOUT
 * ============================================================ */

#define TOOL_H        28
#define STATUS_H      20

#define SIDEBAR_W     132

#define SB_CAP_Y      (TOOL_H + 10)
#define SB_ITEM_Y0    (TOOL_H + 26)
#define SB_ITEM_H     26
#define SB_REFRESH_Y  (SB_ITEM_Y0 + 2 * SB_ITEM_H + 12)

#define GRID_X        (SIDEBAR_W + 10)
#define GRID_Y        (TOOL_H + 8)

#define SCROLL_W      12
#define SCROLL_MARGIN 4

#define GRID_RIGHT    (CW - SCROLL_W - SCROLL_MARGIN - 2)
#define GRID_BOTTOM   (CH - STATUS_H - 2)

#define GRID_W        (GRID_RIGHT - GRID_X)
#define GRID_H        (GRID_BOTTOM - GRID_Y)

#define COLS          3

#define ITEM_W        (GRID_W / COLS)
#define ITEM_H        78

#define VISIBLE_ROWS  (GRID_H / ITEM_H)


/* ============================================================
 * SCROLLBAR
 * ============================================================ */

#define SCROLL_X       (CW - SCROLL_W - SCROLL_MARGIN)

#define SCROLL_UP_Y    GRID_Y
#define SCROLL_BTN_H   16

#define SCROLL_TRACK_Y (SCROLL_UP_Y + SCROLL_BTN_H)

#define SCROLL_DOWN_Y  (GRID_BOTTOM - SCROLL_BTN_H)

#define SCROLL_TRACK_H \
    (SCROLL_DOWN_Y - SCROLL_TRACK_Y)


/* ============================================================
 * APPLICATION MENU
 * ============================================================ */

static const char *files_file_items[] = {
    "Refresh",
    "Delete Selected",
    "Close Window"
};

static const char *files_edit_items[] = { "Deselect" };
static const char *files_go_items[]   = { "Up", "Root" };
static const char *files_view_items[] = { "Icons", "List" };

static const ax_menu_def_t files_menus[] = {
    { "File", files_file_items, 3 },
    { "Edit", files_edit_items, 1 },
    { "Go",   files_go_items,   2 },
    { "View", files_view_items, 2 },
};

static const ax_menubar_def_t files_menubar = {
    "Files",
    files_menus,
    4
};


static int scroll_row = 0;

void reload_directory(void);

#define HIST_MAX 16

static char hist[HIST_MAX][PATH_MAX];
static int  hist_len = 0;
static int  hist_pos = -1;

#define VIEW_ICONS 0
#define VIEW_LIST  1

static int view_mode = VIEW_ICONS;

#define LIST_ROW_H 22

static int scrollbar_drag = 0;
static int drag_offset_y = 0;


/* ============================================================
 * HELPERS
 * ============================================================ */

static void copy_text(char *dst, const char *src, int cap)
{
    int i = 0;

    if (!dst || cap <= 0)
        return;

    if (src) {
        while (src[i] && i < cap - 1) {
            dst[i] = src[i];
            i++;
        }
    }

    dst[i] = 0;
}


static void set_status(const char *s)
{
    copy_text(status, s, sizeof(status));
}


/*
 * Собрать полный путь без молчаливого обрезания.
 *
 * Возвращает:
 *   0  — успешно
 *  -1  — путь не помещается в буфер
 */
static int make_path(
    char *dst,
    int cap,
    const char *root,
    const char *name
) {
    int i = 0;

    if (!dst || cap <= 0 || !name)
        return -1;

    dst[0] = '\0';

    if (root && root[0]) {
        while (root[i]) {
            if (i >= cap - 1)
                return -1;

            dst[i] = root[i];
            i++;
        }

        if (i > 0 && dst[i - 1] != '/') {
            if (i >= cap - 1)
                return -1;

            dst[i++] = '/';
        }
    }

    int j = 0;

    while (name[j]) {
        if (i >= cap - 1) {
            dst[0] = '\0';
            return -1;
        }

        dst[i++] = name[j++];
    }

    dst[i] = '\0';
    return 0;
}


/*
 * Проверка: находится ли путь внутри initrd.
 */
static int is_initrd_path(const char *path)
{
    if (!path)
        return 0;

    return
        path[0] == 'i' &&
        path[1] == 'n' &&
        path[2] == 'i' &&
        path[3] == 't' &&
        path[4] == 'r' &&
        path[5] == 'd' &&
        (path[6] == '\0' || path[6] == '/');
}


/*
 * true, если текущий каталог -- подкаталог тома.
 */
static int can_go_up(void)
{
    for (int i = 0; current_root[i]; i++) {
        if (current_root[i] == '/')
            return 1;
    }

    return 0;
}


/*
 * Выход в родительский каталог.
 */
static int go_up(void)
{
    int last = -1;

    for (int i = 0; current_root[i]; i++) {
        if (current_root[i] == '/')
            last = i;
    }

    if (last <= 0)
        return 0;

    current_root[last] = 0;

    return 1;
}


static int str_eq(const char *a, const char *b)
{
    int i = 0;

    while (a[i] && b[i]) {
        if (a[i] != b[i])
            return 0;
        i++;
    }

    return a[i] == b[i];
}


static void nav_load(const char *path)
{
    copy_text(current_root, path, sizeof(current_root));

    sel = -1;
    last_sel = -1;
    scroll_row = 0;

    reload_directory();
}


static void hist_record(const char *path)
{
    if (hist_pos >= 0 && hist_pos < hist_len &&
        str_eq(hist[hist_pos], path))
        return;

    if (hist_pos + 1 < HIST_MAX) {
        hist_pos++;
    } else {
        for (int i = 1; i < HIST_MAX; i++)
            copy_text(hist[i - 1], hist[i], PATH_MAX);

        hist_pos = HIST_MAX - 1;
    }

    copy_text(hist[hist_pos], path, PATH_MAX);
    hist_len = hist_pos + 1;
}


static void nav_to(const char *path)
{
    if (!path || !path[0])
        return;

    hist_record(current_root);
    nav_load(path);
}


static void nav_back(void)
{
    if (hist_pos <= 0)
        return;

    hist_pos--;
    nav_load(hist[hist_pos]);
}


static void nav_forward(void)
{
    if (hist_pos + 1 >= hist_len)
        return;

    hist_pos++;
    nav_load(hist[hist_pos]);
}


static void go_up_nav(void)
{
    char path[PATH_MAX];
    int last = -1;

    copy_text(path, current_root, sizeof(path));

    for (int i = 0; path[i]; i++)
        if (path[i] == '/')
            last = i;

    if (last <= 0) {
        set_status("Already at volume root");
        return;
    }

    path[last] = 0;
    nav_to(path);
}


static void go_root(void)
{
    nav_to(current_root[0] == 'd' ? "disk0" : "initrd");
}


static int view_rows_per_page(void)
{
    return view_mode == VIEW_LIST ? GRID_H / LIST_ROW_H : VISIBLE_ROWS;
}


static void set_view(int mode)
{
    if (mode == view_mode)
        return;

    view_mode = mode;

    sel = -1;
    last_sel = -1;
    scroll_row = 0;
}


static int total_rows(void)
{
    return view_mode == VIEW_LIST
               ? nfiles
               : (nfiles + COLS - 1) / COLS;
}


static int max_scroll_row(void)
{
    int rows = total_rows();
    int max = rows - view_rows_per_page();

    if (max < 0)
        max = 0;

    return max;
}


static void clamp_scroll(void)
{
    int max = max_scroll_row();

    if (scroll_row < 0)
        scroll_row = 0;

    if (scroll_row > max)
        scroll_row = max;
}


/* ============================================================
 * DIRECTORY CHECK
 * ============================================================ */

static int probe_directory(const char *path)
{
    char probe[256];

    int fd = open((void *)path, O_RDONLY, 0);

    if (fd < 0)
        return 0;

    int n = getdents(fd, probe, sizeof(probe));

    close(fd);

    return n >= 0;
}


/* ============================================================
 * DELETE FILE
 * ============================================================ */

/*
 * Удаление выбранного обычного файла.
 *
 * Используется syscall №10:
 *
 *     unlink(path)
 *
 * Каталоги пока не удаляются.
 * initrd защищён от изменений.
 */
static int delete_selected(void)
{
    if (sel < 0 || sel >= nfiles) {
        set_status("Nothing selected");
        return -1;
    }

    if (files[sel].is_dir) {
        set_status("Cannot delete directory");
        return -1;
    }

    if (is_initrd_path(current_root)) {
        set_status("INITRD IS READ ONLY");
        return -1;
    }

    if (!files[sel].path[0]) {
        set_status("Invalid file path");
        return -1;
    }

    /*
     * unlink() реализован в unistd.h:
     * syscall number = 10.
     */
    int result = unlink((void *)files[sel].path);

    if (result < 0) {
        set_status("DELETE FAILED");
        return -1;
    }

    sel = -1;
    last_sel = -1;

    reload_directory();

    set_status("File deleted");

    return 0;
}


/* ============================================================
 * LOAD DIRECTORY
 * ============================================================ */

void reload_directory(void)
{
    int fd;

    nfiles = 0;

    fd = open((void *)current_root, O_RDONLY, 0);

    if (fd < 0) {
        set_status(
            current_root[0] == 'd'
                ? "NO DISK0 MOUNTED"
                : "OPEN FAILED"
        );

        return;
    }

    static char buf[512];

    for (;;) {

        int n = getdents(fd, buf, sizeof(buf));

        if (n < 0) {
            set_status("GETDENTS FAILED");
            break;
        }

        if (n == 0)
            break;

        int off = 0;

        while (off < n && nfiles < MAX_FILES) {

            ax_dirent *d = (ax_dirent *)(buf + off);

            if (d->reclen < 8 || off + d->reclen > n) {
                set_status("BAD DIRENT");
                close(fd);
                clamp_scroll();
                return;
            }

            if (d->name[0]) {

                copy_text(
                    files[nfiles].name,
                    d->name,
                    sizeof(files[nfiles].name)
                );

                if (make_path(
                        files[nfiles].path,
                        sizeof(files[nfiles].path),
                        current_root,
                        d->name
                    ) < 0) {

                    files[nfiles].path[0] = '\0';
                    files[nfiles].is_dir = 0;
                    files[nfiles].ptype = -1;

                    nfiles++;
                    off += d->reclen;
                    continue;
                }

                files[nfiles].is_dir =
                    probe_directory(files[nfiles].path);

                files[nfiles].ptype =
                    files[nfiles].is_dir
                        ? -1
                        : fptype(files[nfiles].path);

                nfiles++;
            }

            off += d->reclen;
        }

        if (nfiles >= MAX_FILES)
            break;
    }

    close(fd);

    clamp_scroll();

    if (nfiles > 0)
        set_status("OK");
    else
        set_status("EMPTY");
}


/* ============================================================
 * SCROLLBAR CALCULATIONS
 * ============================================================ */

static int scrollbar_thumb_height(void)
{
    int rows = total_rows();
    int h;

    if (rows <= view_rows_per_page())
        return SCROLL_TRACK_H;

    h = (SCROLL_TRACK_H * view_rows_per_page()) / rows;

    if (h < 16)
        h = 16;

    if (h > SCROLL_TRACK_H)
        h = SCROLL_TRACK_H;

    return h;
}


static int scrollbar_thumb_y(void)
{
    int max = max_scroll_row();
    int thumb_h = scrollbar_thumb_height();
    int range = SCROLL_TRACK_H - thumb_h;

    if (max <= 0 || range <= 0)
        return SCROLL_TRACK_Y;

    return SCROLL_TRACK_Y + (range * scroll_row) / max;
}


static void scroll_from_thumb_y(int thumb_y)
{
    int max = max_scroll_row();
    int thumb_h = scrollbar_thumb_height();
    int range = SCROLL_TRACK_H - thumb_h;

    if (max <= 0 || range <= 0)
        return;

    if (thumb_y < SCROLL_TRACK_Y)
        thumb_y = SCROLL_TRACK_Y;

    if (thumb_y > SCROLL_TRACK_Y + range)
        thumb_y = SCROLL_TRACK_Y + range;

    int pos = thumb_y - SCROLL_TRACK_Y;

    scroll_row = (pos * max + range / 2) / range;

    clamp_scroll();
}


/* ============================================================
 * GRID LABEL
 * ============================================================ */

static void draw_grid_label(
    const char *name,
    int x,
    int y,
    unsigned color
) {
    char buf[NAME_MAX + 8];

    int max_cp = (ITEM_W - 8) / 8;
    int used = 0;
    int ncp = 0;

    if (!name)
        return;

    if (max_cp < 6)
        max_cp = 6;

    while (name[used] && ncp < max_cp - 3) {

        unsigned char c = (unsigned char)name[used];

        int len = 1;

        if      ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;

        int copied = 0;

        while (copied < len &&
               name[used + copied] &&
               used + copied < NAME_MAX &&
               used + copied < (int)sizeof(buf) - 4) {

            buf[used + copied] = name[used + copied];
            copied++;
        }

        if (copied != len)
            break;

        used += len;
        ncp++;
    }

    if (name[used]) {
        buf[used++] = '.';
        buf[used++] = '.';
        buf[used++] = '.';
    }

    buf[used] = 0;

    aq_text(buf, x, y, color);
}


/* ============================================================
 * TOOLBAR GLYPHS
 * ============================================================ */

static void draw_refresh_glyph(int cx, int cy, uint32_t col)
{
    static const int px[13] = { 0, 3, 5, 6, 5, 3, 0, -3, -5, -6, -5, -3, 0 };
    static const int py[13] = { -6, -5, -3, 0, 3, 5, 6, 5, 3, 0, -3, -5, -6 };

    for (int i = 0; i < 12; i++) {
        if (i <= 2)   /* разрыв под стрелку */
            continue;
        aq_line(cx + px[i], cy + py[i],
                cx + px[i + 1], cy + py[i + 1], col);
    }

    /* стрелка на конце дуги */
    aq_line(cx + px[2], cy + py[2], cx + px[2] + 3, cy + py[2] - 2, col);
    aq_line(cx + px[2], cy + py[2], cx + px[2] + 4, cy + py[2] + 1, col);
}


static void draw_nav_glyph(int cx, int cy, int forward, uint32_t col)
{
    if (!forward) {
        aq_line(cx + 2, cy - 5, cx - 2, cy, col);
        aq_line(cx - 2, cy, cx + 2, cy + 5, col);
    } else {
        aq_line(cx - 2, cy - 5, cx + 2, cy, col);
        aq_line(cx + 2, cy, cx - 2, cy + 5, col);
    }
}


static void draw_grid_glyph(int cx, int cy, uint32_t col)
{
    aq_fill(cx - 7, cy - 6, 5, 5, col);
    aq_fill(cx + 2, cy - 6, 5, 5, col);
    aq_fill(cx - 7, cy + 1, 5, 5, col);
    aq_fill(cx + 2, cy + 1, 5, 5, col);
}


static void draw_list_glyph(int cx, int cy, uint32_t col)
{
    for (int i = 0; i < 3; i++) {
        aq_fill(cx - 7, cy - 6 + i * 5, 3, 3, col);
        aq_fill(cx - 2, cy - 5 + i * 5, 9, 2, col);
    }
}


/* ============================================================
 * RENDER
 * ============================================================ */

void render(void)
{
    aq_window_bg();
    aq_toolbar(0, 0, CW, TOOL_H);
    aq_statusbar(CH - STATUS_H, STATUS_H);

    /* TOOLBAR */

    /* [‹|›]  [icons|list]  (refresh)   path... */

    aq_segmented(8, 4, 52, 20, 2, -1);
    aq_segmented(68, 4, 52, 20, 2, view_mode == VIEW_LIST ? 1 : 0);
    aq_button(128, 4, 26, 20, 0, AQ_BTN_NORMAL, AQ_BTN_DISABLED);

    draw_nav_glyph(8 + 13, 4 + 10, 0,
                   hist_pos > 0 ? AQ_TEXT : AQ_TEXT_DISABLED);
    draw_nav_glyph(8 + 26 + 13, 4 + 10, 1,
                   hist_pos + 1 < hist_len ? AQ_TEXT : AQ_TEXT_DISABLED);

    draw_grid_glyph(68 + 13, 4 + 10,
                    view_mode == VIEW_ICONS ? AQ_TEXT : AQ_TEXT_DISABLED);
    draw_list_glyph(68 + 26 + 13, 4 + 10,
                    view_mode == VIEW_LIST ? AQ_TEXT : AQ_TEXT_DISABLED);

    draw_refresh_glyph(128 + 13, 4 + 10, AQ_TEXT);

    if (!can_go_up()) {

        const char *loc =
            current_root[0] == 'd'
                ? "disk0 (real disk)"
                : "initrd filesystem";

        aq_text_bold(loc, 160, 10, AQ_TOOL_TEXT);

    } else {

        char loc[64];

        int maxc = (CW - 160 - 6) / 8;
        int i = 0;

        while (current_root[i] && i < maxc) {
            loc[i] = current_root[i];
            i++;
        }

        if (current_root[i]) {

            while (i > maxc - 3)
                i--;

            loc[i++] = '.';
            loc[i++] = '.';
            loc[i++] = '.';
        }

        loc[i] = 0;

        aq_text_bold(loc, 160, 10, AQ_TOOL_TEXT);
    }


    /* SIDEBAR */

    aq_sidebar(
        0,
        TOOL_H,
        SIDEBAR_W,
        CH - TOOL_H - STATUS_H
    );

    aq_sidebar_caption(0, SB_CAP_Y, "FAVORITES");

    aq_sidebar_item(
        0,
        SB_ITEM_Y0,
        SIDEBAR_W,
        SB_ITEM_H,
        "initrd",
        current_root[0] == 'i',
        AX_RGB(96, 166, 250)
    );

    aq_sidebar_item(
        0,
        SB_ITEM_Y0 + SB_ITEM_H,
        SIDEBAR_W,
        SB_ITEM_H,
        "disk0",
        current_root[0] == 'd',
        AX_RGB(150, 152, 162)
    );

    aq_sidebar_caption(0, SB_REFRESH_Y - 8, "ACTIONS");

    aq_sidebar_item(
        0,
        SB_REFRESH_Y,
        SIDEBAR_W,
        SB_ITEM_H,
        "Refresh",
        0,
        AX_RGB(48, 209, 88)
    );


    /* FILE GRID */

    if (view_mode == VIEW_LIST) {

        int rows = view_rows_per_page();

        for (int r = 0; r < rows; r++) {

            int i = scroll_row + r;

            if (i >= nfiles)
                break;

            int iy = GRID_Y + r * LIST_ROW_H;

            if (iy + LIST_ROW_H > GRID_BOTTOM)
                break;

            if (i == sel)
                aq_select_rect(GRID_X - 2, iy, GRID_W + 4, LIST_ROW_H - 2);

            int icon_s = 14;
            int icon_x = GRID_X + 3;
            int icon_y = iy + (LIST_ROW_H - icon_s) / 2;

            if (files[i].is_dir)
                aq_icon_folder(icon_x, icon_y, icon_s);
            else
                aq_icon_doc(icon_x, icon_y, icon_s);

            aq_text(files[i].name, GRID_X + 24,
                    iy + (LIST_ROW_H - 8) / 2,
                    (i == sel) ? AQ_TEXT_ON_ACCENT : AQ_TEXT);

            const char *kind = files[i].is_dir ? "Folder" : "File";

            aq_text(kind,
                    GRID_RIGHT - 6 - aq_text_width(kind),
                    iy + (LIST_ROW_H - 8) / 2,
                    AQ_TEXT_SECOND);
        }

    } else {

    for (int i = 0; i < nfiles; i++) {

        int col = i % COLS;
        int row = i / COLS;

        int visible_row = row - scroll_row;

        if (visible_row < 0 || visible_row >= view_rows_per_page())
            continue;

        int ix = GRID_X + col * ITEM_W;
        int iy = GRID_Y + visible_row * ITEM_H;

        if (iy >= GRID_BOTTOM)
            continue;

        if (i == sel)
            aq_select_rect(
                ix + 3,
                iy + 2,
                ITEM_W - 8,
                ITEM_H - 6
            );

        int dir = files[i].is_dir;

        int icon_s = 44;
        int icon_x = ix + (ITEM_W - icon_s) / 2;
        int icon_y = iy + 6;

        if (dir) {

            aq_icon_folder(icon_x, icon_y, icon_s);

        } else {

            const char *nm = files[i].name;

            int nl = 0;

            while (nm[nl])
                nl++;

            int is_bin =
                nl >= 4 &&
                nm[nl - 4] == '.' &&
                nm[nl - 3] == 'b' &&
                nm[nl - 2] == 'i' &&
                nm[nl - 1] == 'n';

            if (files[i].ptype == PROGRAM_CONSOLE)
                aq_icon_terminal(icon_x, icon_y, icon_s);
            else if (is_bin)
                aq_icon_exec(icon_x, icon_y, icon_s);
            else
                aq_icon_doc(icon_x, icon_y, icon_s);
        }

        draw_grid_label(
            files[i].name,
            ix + 4,
            iy + ITEM_H - 20,
            (i == sel) ? AQ_TEXT_ON_ACCENT : AQ_TEXT
        );
    }


    /* SELECTED LABEL */

    if (sel >= 0 && sel < nfiles) {

        int col = sel % COLS;
        int row = sel / COLS;

        int visible_row = row - scroll_row;

        if (visible_row >= 0 && visible_row < view_rows_per_page()) {

            int ix = GRID_X + col * ITEM_W;
            int iy = GRID_Y + visible_row * ITEM_H;

            if (iy < GRID_BOTTOM) {

                draw_grid_label(
                    files[sel].name,
                    ix + 4,
                    iy + ITEM_H - 20,
                    AQ_TEXT_ON_ACCENT
                );
            }
        }
    }


    }

    /* SCROLLBAR */

    if (total_rows() > view_rows_per_page()) {

        int thumb_h = scrollbar_thumb_height();
        int thumb_y = scrollbar_thumb_y();

        aq_scroll_arrow(
            SCROLL_X + SCROLL_W / 2,
            SCROLL_UP_Y + SCROLL_BTN_H / 2,
            1,
            AQ_TEXT_SECOND
        );

        aq_scroll_arrow(
            SCROLL_X + SCROLL_W / 2,
            SCROLL_DOWN_Y + SCROLL_BTN_H / 2,
            0,
            AQ_TEXT_SECOND
        );

        aq_scrollbar(
            SCROLL_X,
            SCROLL_TRACK_Y,
            SCROLL_W,
            SCROLL_TRACK_H,
            thumb_y,
            thumb_h,
            0,
            scrollbar_drag
        );
    }


    /* STATUS BAR */

    {
        char line[48];

        int p2 = 0;

        const char *pre = "Status: ";

        for (int k = 0;
             pre[k] && p2 < (int)sizeof(line) - 1;
             k++) {
            line[p2++] = pre[k];
        }

        for (int k = 0;
             status[k] && p2 < (int)sizeof(line) - 1;
             k++) {
            line[p2++] = status[k];
        }

        line[p2] = 0;

        aq_text(line, 8, CH - 14, AQ_TEXT_SECOND);


        /* ITEM COUNT */

        char cnt[16];

        int v = nfiles;
        int t3 = 0;

        if (v == 0)
            cnt[t3++] = '0';

        char tmp3[8];

        while (v) {
            tmp3[t3++] = '0' + v % 10;
            v /= 10;
        }

        int q = 0;

        while (t3)
            cnt[q++] = tmp3[--t3];

        cnt[q] = 0;

        aq_text(
            cnt,
            CW - 8 - aq_text_width(cnt),
            CH - 14,
            AQ_TEXT_SECOND
        );
    }
}


/* ============================================================
 * ELF DETECTION
 * ============================================================ */

static int is_elf_file(const char *path)
{
    unsigned char hdr[4];

    int fd = open((void *)path, O_RDONLY, 0);

    if (fd < 0)
        return 0;

    int n = read(fd, hdr, 4);

    close(fd);

    if (n < 4)
        return 0;

    return
        hdr[0] == 0x7F &&
        hdr[1] == 'E' &&
        hdr[2] == 'L' &&
        hdr[3] == 'F';
}


/* ============================================================
 * OPEN SELECTED FILE
 * ============================================================ */

static void open_selected(void)
{
    if (sel < 0 || sel >= nfiles)
        return;

    if (!files[sel].path[0]) {
        set_status("Invalid path");
        return;
    }


    /* DIRECTORY */

    if (files[sel].is_dir) {

        char path[PATH_MAX];

        if (make_path(
                path,
                sizeof(path),
                current_root,
                files[sel].name
            ) < 0) {

            set_status("Path too long");
            return;
        }

        if (!probe_directory(path)) {
            set_status("Cannot open directory");
            return;
        }

        nav_to(path);

        return;
    }


    /* ELF EXECUTABLE */

    if (is_elf_file(files[sel].path)) {

        if (files[sel].ptype == PROGRAM_CONSOLE) {

            char *targv[2];

            targv[0] = "temu";
            targv[1] = files[sel].path;

            int tpid = exec("temu", 2, targv);

            if (tpid >= 0)
                set_status("Console app in terminal");
            else
                set_status("Cannot open terminal");

            return;
        }

        int pid = exec(
            files[sel].path,
            0,
            (char **)0
        );

        if (pid >= 0)
            set_status("Application started");
        else
            set_status("Not executable or launch failed");

        return;
    }


    /* DOCUMENT */

    {
        char *argv[2];

        argv[0] = "notes";
        argv[1] = files[sel].path;

        int pid = exec("notes", 2, argv);

        if (pid >= 0)
            set_status("Opened in Notes");
        else
            set_status("Cannot open file");
    }
}


/* ============================================================
 * MAIN
 * ============================================================ */

void _start(void)
{
    g_canvas = ax_surface("Files", CW, CH);

    ax_canvas_dims(CW, CH);

    if (!g_canvas) {
        for (;;)
            yield();
    }

    ax_menu_set(&files_menubar);

    reload_directory();

    hist_pos = 0;
    hist_len = 1;
    copy_text(hist[0], current_root, PATH_MAX);

    render();
    ax_commit(g_canvas);

    ax_event ev;

    for (;;) {

        while (ax_poll(g_canvas, &ev)) {

            /* WINDOW CLOSE */

            if (ev.type == AX_EV_CLOSE)
                return;

            if (ev.type == AX_EV_QUIT)
                return;


            /* APPLICATION MENU */

            if (ev.type == AX_EV_MENU) {

                if (ev.key == 0) {              /* File */

                    switch (ev.w) {

                        case 0: /* Refresh */
                            reload_directory();
                            sel = -1;
                            last_sel = -1;
                            break;

                        case 1: /* Delete Selected */
                            delete_selected();
                            break;

                        case 2: /* Close Window */
                            ax_win_close(g_canvas);
                            return;
                    }

                } else if (ev.key == 1) {       /* Edit */

                    if (ev.w == 0) {            /* Deselect */
                        sel = -1;
                        last_sel = -1;
                        set_status("Deselected");
                    }

                } else if (ev.key == 2) {       /* Go */

                    if (ev.w == 0) {            /* Up */
                        go_up_nav();
                    } else if (ev.w == 1) {     /* Root */
                        go_root();
                    }

                } else if (ev.key == 3) {       /* View */

                    if (ev.w == 0) set_view(VIEW_ICONS);
                    else if (ev.w == 1) set_view(VIEW_LIST);
                }

                render();
                ax_commit(g_canvas);
                continue;
            }


            if (ev.type != AX_EV_MOUSE)
                continue;


            int new_click = ev.buttons && !last_buttons;


            /* ==================================================
             * DRAG SCROLLBAR
             * ================================================== */

            if (scrollbar_drag) {

                if (!ev.buttons) {

                    scrollbar_drag = 0;

                } else {

                    int new_thumb_y = ev.my - drag_offset_y;

                    scroll_from_thumb_y(new_thumb_y);

                    last_buttons = (int)ev.buttons;

                    render();
                    ax_commit(g_canvas);

                    continue;
                }
            }


            /* ==================================================
             * SCROLLBAR
             * ================================================== */

            if (total_rows() > view_rows_per_page()) {

                int thumb_y = scrollbar_thumb_y();
                int thumb_h = scrollbar_thumb_height();


                /* START DRAG */

                if (new_click &&
                    ev.mx >= SCROLL_X &&
                    ev.mx < SCROLL_X + SCROLL_W &&
                    ev.my >= thumb_y &&
                    ev.my < thumb_y + thumb_h) {

                    scrollbar_drag = 1;
                    drag_offset_y = ev.my - thumb_y;

                    last_buttons = (int)ev.buttons;

                    continue;
                }


                /* SCROLL UP */

                if (new_click &&
                    ev.mx >= SCROLL_X &&
                    ev.mx < SCROLL_X + SCROLL_W &&
                    ev.my >= SCROLL_UP_Y &&
                    ev.my < SCROLL_UP_Y + SCROLL_BTN_H) {

                    scroll_row--;

                    clamp_scroll();

                    last_buttons = (int)ev.buttons;

                    render();
                    ax_commit(g_canvas);

                    continue;
                }


                /* SCROLL DOWN */

                if (new_click &&
                    ev.mx >= SCROLL_X &&
                    ev.mx < SCROLL_X + SCROLL_W &&
                    ev.my >= SCROLL_DOWN_Y &&
                    ev.my < SCROLL_DOWN_Y + SCROLL_BTN_H) {

                    scroll_row++;

                    clamp_scroll();

                    last_buttons = (int)ev.buttons;

                    render();
                    ax_commit(g_canvas);

                    continue;
                }


                /* CLICK ON TRACK */

                if (new_click &&
                    ev.mx >= SCROLL_X &&
                    ev.mx < SCROLL_X + SCROLL_W &&
                    ev.my >= SCROLL_TRACK_Y &&
                    ev.my < SCROLL_TRACK_Y + SCROLL_TRACK_H) {

                    int new_thumb_y = ev.my - thumb_h / 2;

                    scroll_from_thumb_y(new_thumb_y);

                    last_buttons = (int)ev.buttons;

                    render();
                    ax_commit(g_canvas);

                    continue;
                }
            }


            /* ==================================================
             * FILESYSTEM SWITCH: INITRD
             * ================================================== */

            if (new_click &&
                ev.mx >= 0 &&
                ev.mx < SIDEBAR_W &&
                ev.my >= SB_ITEM_Y0 &&
                ev.my < SB_ITEM_Y0 + SB_ITEM_H) {

                nav_to("initrd");

                last_buttons = (int)ev.buttons;

                render();
                ax_commit(g_canvas);

                continue;
            }


            /* ==================================================
             * FILESYSTEM SWITCH: DISK0
             * ================================================== */

            if (new_click &&
                ev.mx >= 0 &&
                ev.mx < SIDEBAR_W &&
                ev.my >= SB_ITEM_Y0 + SB_ITEM_H &&
                ev.my < SB_ITEM_Y0 + 2 * SB_ITEM_H) {

                nav_to("disk0");

                last_buttons = (int)ev.buttons;

                render();
                ax_commit(g_canvas);

                continue;
            }


            /* ==================================================
             * REFRESH BUTTON
             * ================================================== */

            if (new_click &&
                ev.mx >= 0 &&
                ev.mx < SIDEBAR_W &&
                ev.my >= SB_REFRESH_Y &&
                ev.my < SB_REFRESH_Y + SB_ITEM_H) {

                reload_directory();

                sel = -1;
                last_sel = -1;

                last_buttons = (int)ev.buttons;

                render();
                ax_commit(g_canvas);

                continue;
            }


            /* ==================================================
             * BACK BUTTON
             * ================================================== */

            if (new_click &&
                ev.mx >= 8 &&
                ev.mx < 8 + 26 &&
                ev.my >= 4 &&
                ev.my < 4 + 20) {

                nav_back();

                last_buttons = (int)ev.buttons;

                render();
                ax_commit(g_canvas);

                continue;
            }


            /* ==================================================
             * FORWARD BUTTON
             * ================================================== */

            if (new_click &&
                ev.mx >= 34 &&
                ev.mx < 34 + 26 &&
                ev.my >= 4 &&
                ev.my < 4 + 20) {

                nav_forward();

                last_buttons = (int)ev.buttons;

                render();
                ax_commit(g_canvas);

                continue;
            }


            /* ==================================================
             * REFRESH BUTTON
             * ================================================== */

            if (new_click &&
                ev.mx >= 128 &&
                ev.mx < 128 + 26 &&
                ev.my >= 4 &&
                ev.my < 4 + 20) {

                reload_directory();

                sel = -1;
                last_sel = -1;

                last_buttons = (int)ev.buttons;

                render();
                ax_commit(g_canvas);

                continue;
            }


            /* ==================================================
             * VIEW SWITCH (icons / list)
             * ================================================== */

            if (new_click && ev.my >= 4 && ev.my < 4 + 20 &&
                ev.mx >= 68 && ev.mx < 68 + 26) {

                set_view(VIEW_ICONS);

                last_buttons = (int)ev.buttons;

                render();
                ax_commit(g_canvas);

                continue;
            }

            if (new_click && ev.my >= 4 && ev.my < 4 + 20 &&
                ev.mx >= 94 && ev.mx < 94 + 26) {

                set_view(VIEW_LIST);

                last_buttons = (int)ev.buttons;

                render();
                ax_commit(g_canvas);

                continue;
            }


            /* ==================================================
             * FIND FILE UNDER CURSOR
             * ================================================== */

            int hit = -1;

            if (view_mode == VIEW_LIST) {

                if (ev.mx >= GRID_X &&
                    ev.my >= GRID_Y &&
                    ev.my < GRID_BOTTOM) {

                    int r = (ev.my - GRID_Y) / LIST_ROW_H;
                    int idx = scroll_row + r;

                    if (idx < nfiles)
                        hit = idx;
                }

            } else {

            for (int i = 0; i < nfiles; i++) {

                int col = i % COLS;
                int row = i / COLS;

                int visible_row = row - scroll_row;

                if (visible_row < 0)
                    continue;

                if (visible_row >= view_rows_per_page())
                    continue;

                int ix = GRID_X + col * ITEM_W;
                int iy = GRID_Y + visible_row * ITEM_H;

                if (ev.mx >= ix &&
                    ev.mx < ix + ITEM_W &&
                    ev.my >= iy &&
                    ev.my < iy + ITEM_H) {

                    hit = i;
                    break;
                }
            }


            }


            /* ==================================================
             * FILE CLICK
             * ================================================== */

            if (hit >= 0 && new_click) {

                if (last_sel == hit) {

                    sel = hit;

                    open_selected();

                    last_sel = -1;

                } else {

                    sel = hit;
                    last_sel = hit;

                    set_status("Selected - click again to open");
                }
            }


            last_buttons = (int)ev.buttons;

            render();
            ax_commit(g_canvas);
        }

        yield();
    }
}
