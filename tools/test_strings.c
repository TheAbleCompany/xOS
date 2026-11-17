/* Хост-тест: рендер строк menubar/часов, поиск пропадающих глифов. */
#define AXTTF_HOSTLOAD 1
#include "../lib/axttf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void dump(const char *s, int size)
{
    printf("=== \"%s\" @ %d px ===\n", s, size);
    while (*s) {
        unsigned cp;
        const axttf_glyph *g = 0;
        const char *before = s;

        cp = axttf_utf8_next(&s);
        g = axttf_get_glyph(cp, size);
        if (!g)
            printf("  U+%04X '%c': NO GLYPH\n", cp, cp < 128 ? (char)cp : '?');
        else
            printf("  U+%04X '%c': w=%d h=%d adv=%d xoff=%d yoff=%d aa0=%d\n",
                   cp, cp < 128 ? (char)cp : '?',
                   g->w, g->h, g->adv, g->xoff, g->yoff,
                   g->w && g->h ? g->aa[0] : -1);
        if (s == before) { printf("  (stuck) break\n"); break; }
    }
}

int main(void)
{
    printf("ready=%d\n", axttf_ready());
    dump("Desktop", 13);
    dump("File", 13);
    dump("Edit", 13);
    dump("View", 13);
    dump("Nov 12", 13);
    dump("14:35", 13);
    dump("SMTWTFS", 13);
    dump("100%", 13);
    return 0;
}
