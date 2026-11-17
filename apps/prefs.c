#include "../lib/libax.h"
#include "../lib/axquartz.h"
#include "../lib/axaqua.h"
#include "../lib/unistd.h"
#define CW 380
#define CH 240

/*
 * Aqua layout: toolbar + grouped white cards, accent swatches
 * rendered as glossy Aqua tiles (AquaKit).
 */
#define TOOL_H   28
#define CARD_X   16
#define CARD_W   (CW - CARD_X * 2)

/* ============================================================
 * The app's OWN menu bar (macOS model: menus belong to the
 * application).  Clicks arrive as AX_EV_MENU
 * (ev.key = menu index, ev.w = item index).
 * ============================================================ */

static const char *prefs_file_items[] = { "Close Window" };

static const ax_menu_def_t prefs_menus[] = {
    { "File", prefs_file_items, 1 },
};

static const ax_menubar_def_t prefs_menubar = {
    "System Settings",
    prefs_menus,
    1
};

static void render(void)
{
    aq_window_bg();
    aq_toolbar(0, 0, CW, TOOL_H);
    aq_text_bold("General", 12, 10, AQ_TOOL_TEXT);

    ax_screen_t sc;
    ax_screen(&sc);

    char res[32];
    int p = 0;
    int w = sc.width, h = sc.height;
    char tw[8];
    int t = 0;
    if (w == 0) w = 1;
    while (w) { tw[t++] = '0' + w % 10; w /= 10; }
    while (t) res[p++] = tw[--t];
    res[p++] = 'x';
    t = 0;
    if (h == 0) h = 1;
    while (h) { tw[t++] = '0' + h % 10; h /= 10; }
    while (t) res[p++] = tw[--t];
    res[p] = 0;

    /* --- About card --- */
    {
        int cy = TOOL_H + 10;
        aq_panel(CARD_X, cy, CARD_W, 62);

        aq_icon_about(CARD_X + 12, cy + 13, 36);
        aq_text_bold("ArtyomXOS", CARD_X + 60, cy + 12, AQ_TEXT);
        aq_text("Version 1.0  -  Lion Edition",
                CARD_X + 60, cy + 28, AQ_TEXT_SECOND);
        aq_text("64-bit UNIX compatible",
                CARD_X + 60, cy + 44, AQ_TEXT_TERT);
    }

    /* --- Display card --- */
    {
        int cy = TOOL_H + 84;
        aq_panel(CARD_X, cy, CARD_W, 44);

        aq_text_bold("Display", CARD_X + 12, cy + 8, AQ_TEXT);
        aq_text("Resolution", CARD_X + 12, cy + 26, AQ_TEXT_SECOND);
        aq_field(CARD_X + 110, cy + 12, 120, AQ_FIELD_H, res, -1, 0);
    }

    /* --- Accent card --- */
    {
        int cy = TOOL_H + 140;
        aq_panel(CARD_X, cy, CARD_W, 56);

        aq_text_bold("Accent", CARD_X + 12, cy + 8, AQ_TEXT);
        aq_text("Highlight color", CARD_X + 12, cy + 26, AQ_TEXT_SECOND);

        uint32_t accents[] = {
            AX_RGB(10, 132, 255), AX_RGB(255, 69, 58), AX_RGB(48, 209, 88),
            AX_RGB(255, 159, 10), AX_RGB(191, 90, 242)
        };
        for (int i = 0; i < 5; i++) {
            int sx = CARD_X + 130 + i * 36;
            int sy = cy + 22;
            aq_circle_aa(sx + 11, sy + 11, 11, accents[i]);
            aq_gloss_circle(sx + 11, sy + 11, 9,
                            aq_tint(accents[i], 90),
                            aq_shade(accents[i], 40),
                            aq_shade(accents[i], 120));
        }
    }

    aq_text("ArtyomX project (c) 2026",
            CARD_X, CH - 14, AQ_TEXT_TERT);
}

void _start(void)
{
    g_canvas = ax_surface("System Settings", CW, CH);
    ax_canvas_dims(CW, CH);
    if (!g_canvas) { for(;;) yield(); }

    /* Register the app's OWN menu bar (macOS model). */
    ax_menu_set(&prefs_menubar);

    render();
            ax_commit(g_canvas);
    ax_event ev;
    int tick = 0;
    for (;;) {
        while (ax_poll(g_canvas, &ev)>0) {
            if (ev.type == AX_EV_CLOSE) return;
            if (ev.type == AX_EV_QUIT) return;      /* Cmd+Q */
            if (ev.type == AX_EV_MENU && ev.key == 0) {
                switch (ev.w) {
                    case 0: /* File > Close Window */
                        ax_win_close(g_canvas);
                        return;
                }
            }
        }
        if ((tick++ & 0x3FFFF) == 0) {
            render();
            ax_commit(g_canvas);
        }
        //__asm__ volatile("pause");
        yield();
    }
}
