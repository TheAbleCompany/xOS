/* Хост-тест движка axttf.h: рендер строки в ASCII-арт из альфы. */
#define AXTTF_HOSTLOAD 1
#include "../lib/axttf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SCR_W 90
#define SCR_H 24

static unsigned char scr[SCR_H][SCR_W + 1];

int main(void)
{
    const char *s = "ШжЫЖфё Test 13";
    const axttf_glyph *g;
    int size = 13;
    int asc = axttf_ascent(size, 0);
    int pen = 2, base = asc + 2, i;

    printf("ready=%d mono=%d asc=%d lh=%d\n",
           axttf_ready(), axttf_mono_ready(),
           asc, axttf_line_height(size, 0));
    printf("width('%s') = %d px\n", s, axttf_text_width(s, size));
    printf("mono width  = %d px (cell %d)\n",
           axttf_text_width_mono(s), AXTTF_MONO_CELL);

    /* пропорциональная строка */
    {
        const char *p = s;
        unsigned prev = 0;

        while (*p) {
            unsigned cp = axttf_utf8_next(&p);

            g = axttf_get_glyph(cp, size);
            if (!g) { printf("no glyph for U+%X\n", cp); continue; }
            if (cp == 0x428)
                printf("glyph U+0428: w=%d h=%d xoff=%d yoff=%d adv=%d aa[0..4]=%d %d %d %d %d\n",
                       g->w, g->h, g->xoff, g->yoff, g->adv,
                       g->aa[0], g->aa[1], g->aa[2], g->aa[3], g->aa[4]);
            {
                int px = pen + g->xoff, py = base + g->yoff;
                int yy, xx;
                static int drawn;

                for (yy = 0; yy < g->h; yy++)
                    for (xx = 0; xx < g->w; xx++) {
                        int a = g->aa[yy * g->w + xx];
                        int sx = px + xx, sy = py + yy;

                        if (a > 40 && sx >= 0 && sx < SCR_W && sy >= 0 && sy < SCR_H) {
                            scr[sy][sx] = (unsigned char)(a > 160 ? '#' : (a > 90 ? '+' : '.'));
                            drawn++;
                        }
                    }
                if (cp == 0x428)
                    printf("draw Ш: px=%d py=%d drawn_total=%d\n", px, py, drawn);
            }
            pen += g->adv;
            prev = cp;
        }
    }

    /* прямой арт глифа Ш (пороги видно) */
    {
        const axttf_glyph *sh = axttf_get_glyph(0x428, size);
        int yy, xx;

        printf("Ш bitmap w=%d h=%d:\n", sh->w, sh->h);
        for (yy = 0; yy < sh->h; yy++) {
            for (xx = 0; xx < sh->w; xx++) {
                int a = sh->aa[yy * sh->w + xx];

                putchar(a > 200 ? '#' : a > 100 ? '+' : a > 30 ? '.' : ' ');
            }
            putchar('\n');
        }
    }

    for (i = 0; i < SCR_H; i++) {
        int j;

        putchar('|');
        for (j = 0; j < SCR_W; j++)
            putchar(scr[i][j] ? scr[i][j] : ' ');
        putchar('|');
        putchar('\n');
    }

    printf("\nscr ptr ok\n");

    /* моно-режим */
    printf("\nmono glyphs (cell %d):\n", AXTTF_MONO_CELL);
    {
        const char *p = s;

        while (*p) {
            unsigned cp = axttf_utf8_next(&p);

            g = axttf_get_glyph_mono(cp);
            if (!g) { printf("no mono glyph U+%X\n", cp); continue; }
            printf("U+%04X w=%d adv=%d xoff=%d\n", cp, g->w, g->adv, g->xoff);
        }
    }

    /* проверка математики (холодные пути движка) */
    printf("math: sqrt(2)=%.4f (2^0.5=%.4f) cos(0)=%.4f cos(pi)=%.4f\n",
           axtf_sqrtf(2.0f), 1.4142135f, axtf_cosf(0.0f), axtf_cosf(3.14159265f));
    return 0;
}
