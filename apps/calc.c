/*
 * ============================================================
 * Calculator -- multi-window app for ArtyomX64
 * ============================================================
 *
 * macOS process model: ONE process owns MANY windows.
 *
 *   * Every window is an independent calculator with its own
 *     accumulator / operand / display.
 *
 *   * The app registers its OWN global menu bar (macOS model:
 *     menus belong to the application, the shell only renders
 *     them for the frontmost app):
 *
 *         File: New Window / Close Window
 *         Calc: Clear / Negate
 *
 *     Clicking an item delivers AX_EV_MENU to the focused
 *     window (ev.key = menu index, ev.w = item index).
 *
 *   * App menu > Quit sends AX_EV_QUIT to every window --
 *     the app destroys all windows and exits (Cmd+Q).
 *
 * Keyboard: 0-9, + - * / = , C/c -- same as clicking buttons.
 * ============================================================
 */

#include "../lib/libax.h"
#include "../lib/axquartz.h"
#include "../lib/axaqua.h"
#include "../lib/unistd.h"

#define CW 300
#define CH 232

#define MAX_WINS 4

/*
 * Aqua layout (rendered with the shared AquaKit):
 *   toolbar strip, inset white display, 4x4 Aqua button grid
 *   with the blue default "=" button.
 */
#define DISP_X      14
#define DISP_Y      28
#define DISP_H      36
#define GRID_X0     14
#define GRID_Y0     74
#define BTN_GAP     6
#define BTN_H       32

/* ============================================================
 * Per-window state (one process, many windows)
 * ============================================================ */

typedef struct {
    int      used;
    uint32_t *canvas;
    long     rx;              /* operand being typed          */
    long     acc;             /* accumulator                  */
    int      opn;             /* pending operation char       */
    char     disp[24];
    int      dlen;
    int      pressed;         /* index 0..15 of the pressed key, -1 = none */
} calc_win;

static calc_win wins[MAX_WINS];

/* ============================================================
 * The app's OWN menu bar
 * ============================================================ */

static const char *calc_file_items[] = { "New Window", "Close Window" };
static const char *calc_menu_items[] = { "Clear", "Negate" };

static const ax_menu_def_t calc_menus[] = {
    { "File", calc_file_items, 2 },
    { "Calc", calc_menu_items, 2 },
};

static const ax_menubar_def_t calc_menubar = {
    "Calculator",
    calc_menus,
    2
};

/* ============================================================
 * Tiny helpers
 * ============================================================ */

static void putnum(calc_win *w, long n)
{
    w->dlen = 0;
    if (n == 0) { w->disp[w->dlen++] = '0'; w->disp[w->dlen] = 0; return; }
    char tmp[24]; int t = 0;
    int neg = n < 0; if (neg) n = -n;
    while (n) { tmp[t++] = '0' + n % 10; n /= 10; }
    if (neg) w->disp[w->dlen++] = '-';
    while (t) w->disp[w->dlen++] = tmp[--t];
    w->disp[w->dlen] = 0;
}

static const char *keys[4][4] = {
    {"7","8","9","/"},
    {"4","5","6","*"},
    {"1","2","3","-"},
    {"0","C","=","+"},
};

/* Bind libax globals to this window's canvas. */
static void bind(const calc_win *w)
{
    g_canvas = w->canvas;
    g_cw     = CW;
    g_ch     = CH;
}

/* ============================================================
 * Rendering (AquaKit -- unified Lion design)
 * ============================================================ */

/* key index 0..15 -> character */
static char key_char(int idx)
{
    return keys[idx / 4][idx % 4][0];
}

static void render(calc_win *w)
{
    bind(w);

    /* window body + toolbar strip */
    aq_window_bg();
    aq_toolbar(0, 0, g_cw, 24);
    aq_text_bold("Calculator", 10, 8, AQ_TOOL_TEXT);

    /* display: inset white field, right-aligned dark digits */
    aq_rounded_fill(DISP_X, DISP_Y, g_cw - DISP_X * 2, DISP_H, 5,
                    AX_RGB(250, 250, 252));
    aq_rounded_outline(DISP_X, DISP_Y, g_cw - DISP_X * 2, DISP_H, 5,
                       AQ_FIELD_RIM);
    aq_fill_alpha(DISP_X + 2, DISP_Y + 1, g_cw - DISP_X * 2 - 4, 1,
                  AQ_FIELD_INNER);

    /* operator cue on the left of the display */
    if (w->opn) {
        char op[2] = { (char)w->opn, 0 };
        aq_text(op, DISP_X + 8, DISP_Y + 14, AQ_TEXT_SECOND);
    }

    {
        /* aq_text_scaled(..., 2) рисует кеглем 13*2 -- ширина ~ 2x */
        int tw = aq_text_width(w->disp) * 2;
        int tx = g_cw - DISP_X - 8 - tw;
        if (tx < DISP_X + 24) tx = DISP_X + 24;
        aq_text_scaled(w->disp, tx, DISP_Y + 10, AQ_TEXT, 2);
    }

    /* 4x4 Aqua button grid */
    int bw = (g_cw - GRID_X0 * 2) / 4;

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int idx  = r * 4 + c;
            int bx   = GRID_X0 + c * bw;
            int by   = GRID_Y0 + r * (BTN_H + BTN_GAP);
            int bwid = bw - BTN_GAP;
            char ch  = key_char(idx);

            aq_btn_kind kind = (ch == '=') ? AQ_BTN_DEFAULT : AQ_BTN_NORMAL;
            aq_btn_state st = (w->pressed == idx) ? AQ_BTN_DOWN : AQ_BTN_UP;

            char label[2] = { ch, 0 };
            aq_button(bx, by, bwid, BTN_H, label, kind, st);
        }
}

/* ============================================================
 * Calculator logic
 * ============================================================ */

static void press(calc_win *w, char c)
{
    if (c >= '0' && c <= '9') {
        w->rx = w->rx * 10 + (c - '0');
        putnum(w, w->rx);
    } else if (c == 'C') {
        w->rx = 0; w->acc = 0; w->opn = 0; putnum(w, 0);
    } else if (c == '=') {
        long res = w->acc;
        switch (w->opn) {
            case '+': res = w->acc + w->rx; break;
            case '-': res = w->acc - w->rx; break;
            case '*': res = w->acc * w->rx; break;
            case '/': res = w->rx ? w->acc / w->rx : 0; break;
            default: res = w->rx; break;
        }
        putnum(w, res); w->acc = res; w->rx = 0; w->opn = 0;
    } else if (c == 'N') {
        /* menu-only command: negate the displayed value */
        if (w->rx) { w->rx = -w->rx; putnum(w, w->rx); }
        else       { w->acc = -w->acc; putnum(w, w->acc); }
    } else {
        if (w->opn) {
            switch (w->opn) {
                case '+': w->acc += w->rx; break;
                case '-': w->acc -= w->rx; break;
                case '*': w->acc *= w->rx; break;
                case '/': w->acc = w->rx ? w->acc / w->rx : 0; break;
            }
        } else w->acc = w->rx;
        putnum(w, w->acc);
        w->rx = 0; w->opn = c;
    }
}

/* ============================================================
 * Window management (the macOS model)
 * ============================================================ */

static calc_win *open_window(void)
{
    for (int i = 0; i < MAX_WINS; i++) {
        calc_win *w = &wins[i];

        if (w->used)
            continue;

        w->canvas = ax_surface("Calculator", CW, CH);
        if (!w->canvas)
            return 0;
        ax_win_focus(w->canvas);

        w->used = 1;
        w->rx = 0; w->acc = 0; w->opn = 0;
        w->pressed = -1;
        putnum(w, 0);

        return w;
    }

    return 0;
}

static void close_window(calc_win *w)
{
    ax_win_close(w->canvas);
    w->used   = 0;
    w->canvas = 0;
}

static void quit_all(void)
{
    for (int i = 0; i < MAX_WINS; i++)
        if (wins[i].used)
            close_window(&wins[i]);
}

/* ============================================================
 * Entry point
 * ============================================================ */

static int mouse_was_down = 0;

void _start(void)
{
    /* Register the app's OWN menu bar (macOS model). */
    ax_menu_set(&calc_menubar);

    calc_win *main_win = open_window();
    if (!main_win) { for(;;) yield(); }

    render(main_win);
    ax_commit(main_win->canvas);

    ax_event ev;

    for (;;) {
        int live = 0;

        for (int i = 0; i < MAX_WINS; i++) {
            calc_win *w = &wins[i];

            if (!w->used)
                continue;

            int need_render = 0;

            while (ax_poll(w->canvas, &ev) > 0) {

                if (ev.type == AX_EV_CLOSE) {
                    /* This window only -- the process stays. */
                    close_window(w);
                    break;
                }

                if (ev.type == AX_EV_QUIT) {
                    /* Cmd+Q: destroy every window, then exit. */
                    quit_all();
                    return;
                }

                if (ev.type == AX_EV_NEWWIN) {
                    /* File > New Window: one more window here. */
                    calc_win *nw = open_window();
                    if (nw) {
                        render(nw);
                        ax_commit(nw->canvas);
                    }
                    continue;
                }

                if (ev.type == AX_EV_MENU) {
                    /* ev.key = menu, ev.w = item */
                    if (ev.key == 0) {          /* File */
                        switch (ev.w) {
                            case 0: {           /* New Window */
                                calc_win *nw = open_window();
                                if (nw) {
                                    render(nw);
                                    ax_commit(nw->canvas);
                                }
                                break;
                            }
                            case 1:             /* Close Window */
                                close_window(w);
                                break;
                        }
                    } else if (ev.key == 1) {   /* Calc */
                        switch (ev.w) {
                            case 0: press(w, 'C'); need_render = 1; break;
                            case 1: press(w, 'N'); need_render = 1; break;
                        }
                    }
                    continue;
                }

                if (ev.type == AX_EV_KEY) {
                    if (ev.key >= '0' && ev.key <= '9') { press(w, (char)ev.key); need_render = 1; }
                    else if (ev.key == '+' || ev.key == '-' ||
                             ev.key == '*' || ev.key == '/' ||
                             ev.key == '=')        { press(w, (char)ev.key); need_render = 1; }
                    else if (ev.key == 'c' || ev.key == 'C') { press(w, 'C'); need_render = 1; }
                    continue;
                }

                if (ev.type == AX_EV_MOUSE) {

                    int down = ev.buttons & 1;

                    /*
                     * Aqua press feedback: highlight the key under
                     * the cursor while the button is held, fire on
                     * the press edge (released -> pressed).
                     */
                    int bw2 = (CW - GRID_X0 * 2) / 4;
                    int hit = -1;
                    for (int k = 0; k < 16; k++) {
                        int bx = GRID_X0 + (k % 4) * bw2;
                        int by = GRID_Y0 + (k / 4) * (BTN_H + BTN_GAP);
                        if (aq_button_hit(bx, by, bw2 - BTN_GAP, BTN_H,
                                          ev.mx, ev.my)) {
                            hit = k;
                            break;
                        }
                    }

                    if (down && !mouse_was_down) {
                        w->pressed = hit;
                        if (hit >= 0) {
                            press(w, key_char(hit));
                            need_render = 1;
                        }
                    } else if (!down && mouse_was_down) {
                        w->pressed = -1;
                        need_render = 1;
                    }

                    mouse_was_down = down;
                }
            }

            if (!w->used)
                continue;   /* just closed -- skip render */

            live++;

            if (need_render) {
                render(w);
                ax_commit(w->canvas);
            }
        }

        /* No windows left -> the process exits. */
        if (live == 0)
            return;

        yield();
    }
}
