#include "../lib/libax.h"
#include "../lib/unistd.h"

/* ============================================================
 * The app's OWN menu bar (macOS model: menus belong to the
 * application).  Clicks arrive as AX_EV_MENU
 * (ev.key = menu index, ev.w = item index).
 * ============================================================ */

static const char *xsh_shell_items[] = { "Clear Screen", "Close Window" };

static const ax_menu_def_t xsh_menus[] = {
    { "Shell", xsh_shell_items, 2 },
};

static const ax_menubar_def_t xsh_menubar = {
    "XSH",
    xsh_menus,
    1
};
#define CW 480
#define CH 300
#define CHAR_W 8                          /* ширина символа шрифта, px */
#define TERM_COLS ((CW - 20) / CHAR_W)     /* колонок влезает в окно (= 57) */
#define MAX_LINES 160                      /* глубина scrollback */
#define LINE_LEN 80                        /* >= TERM_COLS + запас */

static char scrollback[MAX_LINES][LINE_LEN];
static int  sb_count = 0;
static char cmdline[LINE_LEN];
static int  cl_len = 0;


static void push_line(const char *s)
{
    if (sb_count >= MAX_LINES) {
        for (int i = 1; i < MAX_LINES; i++)
            for (int j = 0; j < LINE_LEN; j++)
                scrollback[i-1][j] = scrollback[i][j];
        sb_count = MAX_LINES - 1;
    }
    int j = 0;
    while (s[j] && j < LINE_LEN - 1) { scrollback[sb_count][j] = s[j]; j++; }
    scrollback[sb_count][j] = 0;
    sb_count++;
}
/* Дренируем master-часть pty окна и разбиваем по '\n' на строки scrollback.
 * Незавершённая (без \n) строка держится в pty_line и рисуется render()
 * "живьём" -- иначе, например, "What's your name?: " без \n не появится
 * на экране, пока не придёт следующая полная строка. */
static char pty_line[LINE_LEN];
static int  pty_line_len = 0;
static int  fg_pid = 0; /* 0 = ввод у шелла, иначе -- у дочернего процесса */

/* Текущая строка заполнилась до TERM_COLS: переносим её в scrollback.
 * По возможности переносим по последнему пробелу (word wrap): хвост
 * после пробела переезжает в начало новой строки. Если пробела нет --
 * жёсткий разрез по колонке. */
static void wrap_line(void)
{
    char tail[LINE_LEN];
    int cut = -1;
    int i, t;

    for (i = TERM_COLS - 1; i >= TERM_COLS / 3; i--) {
        if (pty_line[i] == ' ') { cut = i; break; }
    }

    if (cut > 0 && cut < pty_line_len) {
        t = 0;
        for (i = cut + 1; i < pty_line_len; i++)
            tail[t++] = pty_line[i];
        tail[t] = 0;

        pty_line[cut] = 0;             /* обрезаем по пробел */
        push_line(pty_line);

        for (i = 0; i < t; i++)        /* хвост -> начало новой строки */
            pty_line[i] = tail[i];
        pty_line_len = t;
    } else {
        pty_line[pty_line_len] = 0;
        push_line(pty_line);
        pty_line_len = 0;
    }
}

/* Дренируем master-часть pty окна и разбиваем по '\n' на строки scrollback.
 * Длинные строки (вывод run без '\n') автоматически переносятся по ширине
 * окна -- как в обычных терминалах. Незавершённая строка держится в
 * pty_line и рисуется render() "живьём". */
static void drain_pty(void)
{
    uint8_t buf[128];
    int n, guard = 32;   /* защитный предел итераций на кадр */
    while (guard-- > 0 && (n = ax_term_read(g_canvas, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n') {
                pty_line[pty_line_len] = 0;
                push_line(pty_line);
                pty_line_len = 0;
            } else if (c == '\b') {
                if (pty_line_len > 0) pty_line_len--;
            } else if (c == '\r') {
                /* CR игнорируем: перенос делает LF или wrap_line */
            } else if (c >= ' ') {
                if (pty_line_len >= TERM_COLS)
                    wrap_line();       /* строка заполнилась -- перенос */
                if (pty_line_len < LINE_LEN - 1)
                    pty_line[pty_line_len++] = c;
            }
            /* прочие управляющие символы (< 32) -- игнорируем */
        }
    }
    pty_line[pty_line_len] = 0;
}


static int streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void itoa_simple(int v, char *out)
{
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    char tmp[12]; int t = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    while (v) { tmp[t++] = '0' + v % 10; v /= 10; }
    int p = 0;
    if (neg) out[p++] = '-';
    while (t) out[p++] = tmp[--t];
    out[p] = 0;
}

// Парсинг аргументов: разбивает cmd на argv, возвращает argc
static int parse_args(char *cmd, char **argv, int max_args)
{
    int argc = 0;
    char *p = cmd;
    while (*p == ' ') p++;
    while (*p && argc < max_args) {
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = 0; p++; }
        while (*p == ' ') p++;
    }
    return argc;
}

static void run_cmd(char *cmd)
{
    if (cmd[0] == 0) return;

    char *argv[8];
    int argc = parse_args(cmd, argv, 8);
    if (argc == 0) return;

    if (streq(argv[0], "help")) {
        push_line("cmds: help ver clear time echo exec");
    } else if (streq(argv[0], "ver")) {
        push_line("ArtyomXOS 1.0 x86_64 AXShell");
    } else if (streq(argv[0], "clear")) {
        sb_count = 0;
    } else if (streq(argv[0], "time")) {
        ax_time_t t; ax_time(&t);
        char buf[32];
        buf[0] = '0' + t.hour / 10; buf[1] = '0' + t.hour % 10; buf[2] = ':';
        buf[3] = '0' + t.minute / 10; buf[4] = '0' + t.minute % 10; buf[5] = ':';
        buf[6] = '0' + t.second / 10; buf[7] = '0' + t.second % 10; buf[8] = 0;
        push_line(buf);
    } else if (streq(argv[0], "echo")) {
        char buf[LINE_LEN];
        int p = 0;
        for (int i = 1; i < argc; i++) {
            if (i > 1 && p < LINE_LEN - 1) buf[p++] = ' ';
            for (int j = 0; argv[i][j] && p < LINE_LEN - 1; j++)
                buf[p++] = argv[i][j];
        }
        buf[p] = 0;
        push_line(buf);
    } else if (streq(argv[0], "exec")) {
        if (fg_pid) {
            push_line("xsh: busy, wait for child");
        } else if (argc < 2) {
            push_line("usage: exec <path> [args...]");
        } else {
            /* Передаём аргументы: exec lua /comprehensive.lua ->
               exec("lua", 2, {"lua", "/comprehensive.lua"}) */
            int pid = exec(argv[1], argc - 1, &argv[1]);
            if (pid < 0) {
                push_line("exec: failed");
            } else {
                fg_pid = pid;
                pty_line_len = 0;
                pty_line[0] = 0;
            }
        }
    } else {
        push_line("xsh: command not found");
    }
}
static void render(void)
{
    ax_fill(AX_ARGB(0xF2,10,12,16));
    ax_rect(0, 0, CW, 22, AX_RGB(28,30,38));
    ax_text("XSH Shell", 10, 7, AX_RGB(100,200,255));

    /* Рисуем ХВОСТ scrollback (последние строки), чтобы после переноса
       длинного вывода на экране всегда было видно свежее + живую строку. */
    int rows = (CH - 50) / 10;                       /* строк влезает */
    int start = sb_count > rows - 1 ? sb_count - (rows - 1) : 0;
    int y = 30;
    for (int i = start; i < sb_count; i++, y += 10) {
        ax_text(scrollback[i], 10, y, AX_RGB(200,220,200));
    }
    if (fg_pid) {
        ax_text(pty_line, 10, y, AX_RGB(200,220,200));
        ax_rect(10 + pty_line_len * 8, y, 6, 8, AX_RGB(255,180,80));
    } else {
        ax_text("$ ", 10, y, AX_RGB(100,200,255));
        ax_text(cmdline, 26, y, AX_RGB(220,240,220));
        ax_rect(26 + cl_len * 8, y, 6, 8, AX_RGB(100,200,255));
    }
}

void _start(void)
{
    g_canvas = ax_surface("XSH", CW, CH);
    ax_canvas_dims(CW, CH);
    if (!g_canvas) { for(;;) yield(); }

    /* Register the app's OWN menu bar (macOS model). */
    ax_menu_set(&xsh_menubar);

    push_line("XSH 0.0.3 - type 'help'");
    render();
    ax_commit(g_canvas);

    ax_event ev;
    for (;;) {
        while (ax_poll(g_canvas, &ev)>0) {
            if (ev.type == AX_EV_CLOSE) return;
            if (ev.type == AX_EV_QUIT) return;    /* Cmd+Q */
            if (ev.type == AX_EV_MENU && ev.key == 0) {
                switch (ev.w) {
                    case 0:                       /* Shell > Clear Screen */
                        sb_count = 0;
                        pty_line_len = 0;
                        pty_line[0] = 0;
                        cl_len = 0;
                        cmdline[0] = 0;
                        break;
                    case 1:                       /* Shell > Close Window */
                        ax_win_close(g_canvas);
                        return;
                }
                render();
                ax_commit(g_canvas);
                continue;
            }
            if (ev.type == AX_EV_KEY) {
                if (!fg_pid) {
                    if (ev.key == '\n') {
                        cmdline[cl_len] = 0;
                        char echo[LINE_LEN];
                        echo[0] = '$'; echo[1] = ' ';
                        int p = 2;
                        for (int i = 0; i < cl_len && p < LINE_LEN - 1; i++)
                            echo[p++] = cmdline[i];
                        echo[p] = 0;
                        push_line(echo);
                        run_cmd(cmdline);
                        cl_len = 0;
                        cmdline[0] = 0;
                    } else if (ev.key == '\b') {
                        if (cl_len > 0) cl_len--;
                        cmdline[cl_len] = 0;
                    } else if (cl_len < LINE_LEN - 1) {
                        cmdline[cl_len++] = (char)ev.key;
                        cmdline[cl_len] = 0;
                    }
                } else {
                    /* фокус ввода у дочернего процесса -- отдаём
                       нажатие в master pty, echo/backspace/Enter
                       обработает line discipline в dev/pty.c сама */
                    uint8_t kb = (uint8_t)ev.key;
                    ax_term_write(g_canvas, &kb, 1);
                }
            }
        }

        drain_pty();

        if (fg_pid && !ax_task_alive(fg_pid)) {
            fg_pid = 0;   /* процесс завершился -- возвращаем ввод шеллу */
            if (pty_line_len > 0) {
                pty_line[pty_line_len] = 0;
                push_line(pty_line);
                pty_line_len = 0;
                pty_line[0] = 0;
            }
        }

        render();
        ax_commit(g_canvas);

        yield();
    }
}
