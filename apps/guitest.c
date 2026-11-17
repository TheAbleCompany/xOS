/*
 * guitest.c -- диагностический клиент WindowServer (v2).
 * Ходит ТОЛЬКО через libax API (как files/notes), печатая
 * в serial каждый шаг.  Диагностика потери связи живёт в
 * ax_poll (libax.h, AXDIAG).
 */

#include "../lib/libax.h"
#include "../lib/unistd.h"

/* --- собственный serial-вывод (без common.o) --- */
static inline void gt_outb(unsigned short port, unsigned char val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void gt_putc(char c) { gt_outb(0x3F8, (unsigned char)c); }

static void gt_puts(const char *s) { while (*s) gt_putc(*s++); }

static void gt_hex(uint64_t v)
{
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int d = (int)((v >> (60 - i * 4)) & 0xF);
        buf[2 + i] = d < 10 ? ('0' + d) : ('A' + d - 10);
    }
    buf[18] = 0;
    gt_puts(buf);
}

/* маркер в .data: проверка целостности своего образа */
static volatile uint32_t gt_marker = 0x12345678;

#define CW 320
#define CH 200

void _start(void)
{
    gt_puts("GT: start\n");

    /* r11 round-trip across int 0x80 */
    {
        volatile uint64_t magic_in = 0xDEADBEEFCAFE1234ull;
        volatile uint64_t magic_out = 0;
        __asm__ volatile(
            "movq %1, %%r11\n\t"
            "int $0x80\n\t"
            "movq %%r11, %0\n\t"
            : "=m"(magic_out)
            : "m"(magic_in), "a"((long)151)
            : "memory");
        gt_puts("GT: r11 test ");
        gt_puts(magic_out == magic_in ? "[OK]\n" : "[CLOBBERED!]\n");
    }

    gt_puts("GT: marker="); gt_hex(gt_marker);
    gt_puts(&gt_marker == (volatile uint32_t *)0 ? " ??\n" : "\n");

    int pid = ax_getpid();
    gt_puts("GT: getpid="); gt_hex((uint64_t)pid); gt_puts("\n");

    /* --- через libax: connect+surface --- */
    uint32_t *cv = ax_surface("GuiTest", CW, CH);
    gt_puts("GT: surface=");
    gt_hex((uint64_t)(uintptr_t)cv);
    gt_puts(" w="); gt_hex((uint64_t)g_cw);
    gt_puts(" h="); gt_hex((uint64_t)g_ch);
    gt_puts(" marker="); gt_hex(gt_marker);
    gt_puts("\n");

    if (!cv) {
        gt_puts("GT: surface FAILED -- spin\n");
        for (;;) yield();
    }

    /* рисуем красный + белую рамку */
    for (int i = 0; i < g_cw * g_ch; i++)
        cv[i] = AX_ARGB(0xF0, 120, 30, 30);
    for (int x = 0; x < g_cw; x++)
        for (int j = 0; j < 4; j++) {
            cv[j * g_cw + x] = 0xFFFFFFFF;
            cv[(g_ch - 1 - j) * g_cw + x] = 0xFFFFFFFF;
        }

    gt_puts("GT: drawn px0="); gt_hex((uint64_t)cv[0]); gt_puts("\n");

    ax_commit(cv);
    gt_puts("GT: commit sent, marker="); gt_hex(gt_marker); gt_puts("\n");

    /* --- жизненный цикл --- */
    long beats = 0;
    ax_event ev;
    for (;;) {
        int prc = ax_poll(cv, &ev);
        if (prc < 0) {
            gt_puts("GT: poll<0 -- spin, marker="); gt_hex(gt_marker);
            gt_puts("\n");
            for (;;) yield();
        }
        if (prc == 1) {
            gt_puts("GT: EV type="); gt_hex((uint64_t)ev.type);
            gt_puts(" key=");        gt_hex((uint64_t)ev.key);
            gt_puts("\n");
            if (ev.type == 3 || ev.type == 6)
                return;
        }
        beats++;
        if ((beats & 0xFFFFF) == 3) {
            gt_puts("GT: alive, marker="); gt_hex(gt_marker);
            gt_puts("\n");
        }
        yield();
    }
}
