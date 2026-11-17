/*
 * shim_ax.c -- host implementation of the ArtyomX64 ax_* API.
 * See shim_ax.h for the script format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shim_ax.h"

int       g_cw = 0, g_ch = 0;
uint32_t *g_canvas = 0;

#define MAX_QUEUED 64
static ax_event ev_queue[MAX_QUEUED];
static int      ev_head = 0, ev_tail = 0;

static void push_ev(const ax_event *e)
{
    int next = (ev_head + 1) % MAX_QUEUED;

    if (next == ev_tail)
        return;                              /* drop if full */

    ev_queue[ev_head] = *e;
    ev_head = next;
}

/* ---------------- script ---------------- */

typedef enum { SC_WAIT, SC_KEY, SC_MENU, SC_DUMP, SC_QUIT } sc_kind_t;

typedef struct {
    sc_kind_t kind;
    long      at_commit;                     /* fire when commits >= this */
    int       fired;
    int       a, b;                          /* menu m/i          */
    char      ch;                            /* key               */
    char      tag[64];                       /* dump file tag     */
} sc_item_t;

static sc_item_t script[512];
static int       script_n = 0;
static long      commits  = 0;
static long      stall_seen = -1;
static long      stall_count = 0;

/* progress bookkeeping shared by ax_commit() and yield() */
static void stall_note(void)
{
    if (commits == stall_seen)
        stall_count++;
    else {
        stall_seen = commits;
        stall_count = 0;
    }
}

static void load_script(void)
{
    const char *path = getenv("HOV_SCRIPT");

    if (!path) {
        fprintf(stderr, "shim: HOV_SCRIPT not set; interactive-less idle\n");
        return;
    }

    FILE *fp = fopen(path, "r");

    if (!fp) {
        perror("shim: open script");
        exit(2);
    }

    char line[256];
    long at_commit = 1;     /* 1-based commit counter (loading = 1) */

    while (fgets(line, sizeof line, fp)) {
        char cmd[64], arg1[64], arg2[64];

        if (line[0] == '#' || line[0] == '\n')
            continue;

        int n = sscanf(line, "%63s %63s %63s", cmd, arg1, arg2);

        if (n <= 0)
            continue;

        if (!strcmp(cmd, "wait")) {
            at_commit += atoi(arg1);
        } else if (!strcmp(cmd, "key")) {
            sc_item_t *it = &script[script_n++];

            it->kind = SC_KEY;
            it->at_commit = at_commit;
            it->ch   = arg1[0];
        } else if (!strcmp(cmd, "menu")) {
            sc_item_t *it = &script[script_n++];

            it->kind = SC_MENU;
            it->at_commit = at_commit;
            it->a    = atoi(arg1);
            it->b    = atoi(arg2);
        } else if (!strcmp(cmd, "dump")) {
            sc_item_t *it = &script[script_n++];

            it->kind = SC_DUMP;
            it->at_commit = at_commit;
            snprintf(it->tag, sizeof it->tag, "%s", arg1);
        } else if (!strcmp(cmd, "quit")) {
            sc_item_t *it = &script[script_n++];

            it->kind = SC_QUIT;
            it->at_commit = at_commit;
        } else {
            fprintf(stderr, "shim: unknown script cmd '%s'\n", cmd);
        }
    }

    fclose(fp);

    for (int i = 0; i < script_n; i++)
        fprintf(stderr, "shim: item %d kind=%d at=%ld ch=%c tag=%s\n",
                i, script[i].kind, script[i].at_commit,
                script[i].ch ? script[i].ch : '?', script[i].tag);

}

static void run_due_actions(void)
{
    /* fire every action whose commit index has been reached */
    for (int i = 0; i < script_n; i++) {
        sc_item_t *it = &script[i];

        if (it->fired)
            continue;

        if (commits < it->at_commit &&
            !(commits > 1 && stall_count >= 20000))
            continue;       /* due, or event-wait stall after start */

        it->fired = 1;

        switch (it->kind) {
            case SC_KEY: {
                ax_event e;

                memset(&e, 0, sizeof e);
                e.type = AX_EV_KEY;
                e.key  = (uint32_t)(unsigned char)it->ch;
                push_ev(&e);
                break;
            }

            case SC_MENU: {
                ax_event e;

                memset(&e, 0, sizeof e);
                e.type = AX_EV_MENU;
                e.key  = (uint32_t)it->a;
                e.w    = (uint32_t)it->b;
                push_ev(&e);
                break;
            }

            case SC_DUMP: {
                char path[256];

                snprintf(path, sizeof path, "%s.ppm", it->tag);

                FILE *fp = fopen(path, "wb");

                if (fp) {
                    fprintf(fp, "P6\n%d %d\n255\n", g_cw, g_ch);

                    for (int p = 0; p < g_cw * g_ch; p++) {
                        uint8_t rgb[3] = {
                            (uint8_t)(g_canvas[p] >> 16),
                            (uint8_t)(g_canvas[p] >> 8),
                            (uint8_t)(g_canvas[p])
                        };

                        fwrite(rgb, 1, 3, fp);
                    }

                    fclose(fp);
                    fprintf(stderr, "shim: dumped %s (commit %ld)\n",
                            path, commits);
                }

                break;
            }

            case SC_QUIT:
                fprintf(stderr, "shim: script quit (commit %ld)\n",
                        commits);
                exit(0);

            case SC_WAIT:
                break;
        }
    }
}

/* ---------------- fake RTC ---------------- */

static void fake_clock(ax_time_t *t)
{
    /* 20 commits = 1 fake second */
    long total_sec = commits / 20;

    t->year   = 2026;
    t->month  = 9;
    t->day    = 11;
    t->hour   = 12;
    t->minute = (int32_t)((total_sec / 60) % 60);
    t->second = (int32_t)(total_sec % 60);
}

/* ---------------- API ---------------- */

uint32_t *ax_surface(const char *title, int w, int h)
{
    (void)title;

    load_script();

    g_canvas = (uint32_t *)malloc((size_t)w * h * 4);

    if (!g_canvas)
        exit(3);

    for (int i = 0; i < w * h; i++)
        g_canvas[i] = AX_RGB(20, 20, 24);

    g_cw = w;
    g_ch = h;

    return g_canvas;
}

int ax_poll(uint32_t *canvas, ax_event *ev)
{
    (void)canvas;

    if (ev_head == ev_tail)
        return 0;

    *ev = ev_queue[ev_tail];
    ev_tail = (ev_tail + 1) % MAX_QUEUED;

    return 1;
}

void ax_commit(uint32_t *canvas)
{
    (void)canvas;
    commits++;
    stall_note();

    if (getenv("HOV_DUMP_ALL")) {
        char path[64];

        snprintf(path, sizeof path, "all_%05ld.ppm", commits);

        FILE *fp = fopen(path, "wb");

        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", g_cw, g_ch);

            for (int p = 0; p < g_cw * g_ch; p++) {
                uint8_t rgb[3] = {
                    (uint8_t)(g_canvas[p] >> 16),
                    (uint8_t)(g_canvas[p] >> 8),
                    (uint8_t)(g_canvas[p])
                };

                fwrite(rgb, 1, 3, fp);
            }

            fclose(fp);
        }
    }

    run_due_actions();
}

int ax_menu_set(const ax_menubar_def_t *def)
{
    fprintf(stderr, "shim: menu_set app='%s' menus=%u\n",
            def->app_name, def->menu_count);
    return 0;
}

int ax_win_close(uint32_t *canvas)
{
    (void)canvas;

    fprintf(stderr, "shim: win_close (commit %ld)\n", commits);

    exit(0);
}

int ax_win_focus(uint32_t *canvas)
{
    (void)canvas;

    return 0;
}

void ax_time(ax_time_t *t)
{
    fake_clock(t);
}

void ax_screen(ax_screen_t *s)
{
    s->width = 1024;
    s->height = 768;
}

void yield(void)
{
    /*
     * host: no scheduler.  Pump script actions so keys injected while
     * the game sits in an event-wait loop (game over / win screens)
     * still get delivered; otherwise the shim would deadlock.
     *
     * The game only calls yield() when it stops committing frames
     * (event wait) -- count consecutive no-progress yields and let
     * pending actions fire once the stall is obvious.
     */
    stall_note();
    run_due_actions();
}

/* ---------------- entry (the game defines game_main) ---------------- */

int game_main(void);

int main(void)
{
    game_main();

    return 0;
}
