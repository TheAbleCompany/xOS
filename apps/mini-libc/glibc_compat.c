/*
 * glibc_compat.c — слой совместимости mini-libc с glibc.
 *
 * Проблема: run.c компилируется обычным gcc, то есть с ЗАГОЛОВКАМИ glibc
 * (без -nostdinc). Заголовки glibc ещё на этапе компиляции подменяют часть
 * имён на glibc-специфичные символы, которых нет в mini-libc:
 *
 *   1) stdio.h (при включённом __USE_ISOC99) объявляет sscanf с
 *      __asm__("__isoc99_sscanf"), поэтому run.o ссылается на
 *      __isoc99_sscanf, а не на sscanf;
 *
 *   2) ctype.h определяет isprint/isspace/isdigit и т.д. как табличные
 *      макросы вида
 *          #define isspace(c) ((*__ctype_b_loc())[(int)(c)] & _ISspace)
 *      поэтому run.o ссылается на __ctype_b_loc.
 *
 * -fno-builtin тут не помогает: это не встроенные функции gcc, а текстовые
 * подмены в самих заголовках.
 *
 * Файл полностью самодостаточен: не зависит от остального кода mini-libc и
 * определяет только «приватные» имена glibc, поэтому не конфликтует с уже
 * реализованными функциями libc (sscanf, tolower и т.п. не трогаем).
 */

#include <stdarg.h>
#include <stddef.h>

/* =================================================================== */
/* 1. __ctype_b_loc / __ctype_tolower_loc / __ctype_toupper_loc         */
/* =================================================================== */

/* Биты классификации — в точности как в glibc (C locale) */
#define G_ISupper  0x0100u
#define G_ISlower  0x0200u
#define G_ISalpha  0x0400u
#define G_ISdigit  0x0800u
#define G_ISxdigit 0x1000u
#define G_ISspace  0x2000u
#define G_ISprint  0x4000u
#define G_ISgraph  0x8000u
#define G_ISblank  0x0001u
#define G_IScntrl  0x0002u
#define G_ISpunct  0x0004u
#define G_ISalnum  0x0008u

static unsigned short g_ctype_tab[384];        /* индексы: -128 .. 255 */
static unsigned short *g_ctype_ptr = g_ctype_tab + 128;
static int g_ctype_ready = 0;

const unsigned short **__ctype_b_loc(void);
const unsigned short **__ctype_b_loc(void)
{
    if (!g_ctype_ready) {
        int c;
        for (c = 0; c < 256; c++) {
            unsigned f = 0;
            int up = (c >= 'A' && c <= 'Z');
            int lo = (c >= 'a' && c <= 'z');
            int dg = (c >= '0' && c <= '9');

            if (up) f |= G_ISupper;
            if (lo) f |= G_ISlower;
            if (up || lo)        f |= G_ISalpha;
            if (up || lo || dg)  f |= G_ISalnum;
            if (dg)              f |= G_ISdigit;
            if (dg || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
                f |= G_ISxdigit;

            if (c == ' ' || c == '\t' || c == '\n' ||
                c == '\v' || c == '\f' || c == '\r')
                f |= G_ISspace;
            if (c == ' ' || c == '\t')
                f |= G_ISblank;

            if (c < 0x20 || c == 0x7f)
                f |= G_IScntrl;

            if (f & (G_ISalpha | G_ISdigit))
                f |= G_ISgraph | G_ISprint;
            else if (c == ' ')
                f |= G_ISprint;
            else if (c > 0x20 && c < 0x7f)
                f |= G_ISpunct | G_ISgraph | G_ISprint;

            g_ctype_tab[128 + c] = (unsigned short)f;
        }
        /* зона отрицательных индексов (EOF и -128..-1) — зеркало 128..255 */
        for (c = 0; c < 128; c++)
            g_ctype_tab[c] = g_ctype_tab[c + 256];
        g_ctype_ready = 1;
    }
    return (const unsigned short **)&g_ctype_ptr;
}

/* На случай использования tolower/toupper: заголовки glibc превращают их
 * в табличные макросы через __ctype_tolower_loc/__ctype_toupper_loc.     */

static int g_tolower_tab[384];
static int *g_tolower_ptr = g_tolower_tab + 128;
static int g_tolower_ready = 0;

const int **__ctype_tolower_loc(void);
const int **__ctype_tolower_loc(void)
{
    if (!g_tolower_ready) {
        int c;
        for (c = 0; c < 256; c++)
            g_tolower_tab[128 + c] =
                (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
        for (c = 0; c < 128; c++)
            g_tolower_tab[c] = g_tolower_tab[c + 256];
        g_tolower_ready = 1;
    }
    return (const int **)&g_tolower_ptr;
}

static int g_toupper_tab[384];
static int *g_toupper_ptr = g_toupper_tab + 128;
static int g_toupper_ready = 0;

const int **__ctype_toupper_loc(void);
const int **__ctype_toupper_loc(void)
{
    if (!g_toupper_ready) {
        int c;
        for (c = 0; c < 256; c++)
            g_toupper_tab[128 + c] =
                (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
        for (c = 0; c < 128; c++)
            g_toupper_tab[c] = g_toupper_tab[c + 256];
        g_toupper_ready = 1;
    }
    return (const int **)&g_toupper_ptr;
}

/* =================================================================== */
/* 2. __isoc99_sscanf                                                   */
/*    Самодостаточная реализация scanf по строке. Не использует и не    */
/*    переопределяет sscanf/vsscanf из mini-libc, поэтому конфликтов    */
/*    символов нет ни при каком состоянии библиотеки.                   */
/* =================================================================== */

static int sc_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\v' || c == '\f' || c == '\r';
}

static int sc_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Разбор целого числа. base: 8/10/16 или 0 (автоопределение, для %i).
 * maxw: ограничение ширины (0 = без ограничения). Возвращает 1 при успехе
 * и сдвигает *pp; при неудаче откатывает *pp на исходную позицию.
 */
static int sc_scan_int(const char **pp, int base, int maxw,
                       unsigned long long *out, int *neg)
{
    const char *p = *pp;
    const char *start = *pp;
    unsigned long long v = 0;
    int any = 0, w = 0;

    *neg = 0;
    if (*p == '+' || *p == '-') {
        if (*p == '-') *neg = 1;
        p++;
        w++;
    }
    if ((base == 16 || base == 0) &&
        p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        sc_hexval(p[2]) >= 0 && (maxw == 0 || w + 2 <= maxw)) {
        base = 16;
        p += 2;
        w += 2;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }
    for (;;) {
        int d;
        if (maxw != 0 && w >= maxw) break;
        d = sc_hexval(*p);
        if (d < 0 || d >= base) break;
        v = v * (unsigned long long)base + (unsigned long long)d;
        p++;
        w++;
        any = 1;
    }
    if (!any) {
        *pp = start;
        return 0;
    }
    *pp = p;
    *out = v;
    return 1;
}

/* Разбор вещественного числа (десятичная запись + экспонента). */
static double sc_scan_float(const char **pp, int *ok)
{
    const char *p = *pp;
    const char *start = *pp;
    double sign = 1.0, v = 0.0;
    int nd = 0, e = 0;

    *ok = 0;
    if (*p == '+' || *p == '-') {
        if (*p == '-') sign = -1.0;
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10.0 + (*p - '0');
        p++;
        nd++;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            v = v * 10.0 + (*p - '0');
            p++;
            nd++;
            e--;
        }
    }
    if (nd == 0) {
        *pp = start;
        return 0.0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *save = p;
        int esign = 1, ev = 0;
        p++;
        if (*p == '+' || *p == '-') {
            if (*p == '-') esign = -1;
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            ev = ev * 10 + (*p - '0');
            p++;
        }
        if (ev == 0) p = save;      /* после 'e' нет цифр — откат */
        else e += esign * ev;
    }
    while (e > 0) { v *= 10.0; e--; }
    while (e < 0) { v /= 10.0; e++; }
    *ok = 1;
    *pp = p;
    return sign * v;
}

#define SC_EOF()  do { va_end(ap); return -1; } while (0)
#define SC_DONE() do { va_end(ap); return assigned; } while (0)

int __isoc99_sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    const char *s = str;
    int assigned = 0;

    va_start(ap, fmt);

    while (*fmt) {
        int width = 0, suppress = 0;
        char lenmod = 0;   /* 'H' = hh, 'h', 'l', 'L' = ll/L, 'j'->L, 'z'/'t'->l */
        char conv;

        /* пробелы в формате — пропустить пробелы во входе */
        if (sc_isspace((unsigned char)*fmt)) {
            while (sc_isspace((unsigned char)*fmt)) fmt++;
            while (sc_isspace((unsigned char)*s)) s++;
            continue;
        }

        /* литерал */
        if (*fmt != '%') {
            if (*s == 0) {
                if (assigned == 0) SC_EOF();
                SC_DONE();
            }
            if (*s != *fmt) SC_DONE();
            s++;
            fmt++;
            continue;
        }

        fmt++;                              /* шаг за '%' */
        if (*fmt == '%') {                  /* "%%" -> литеральный '%' */
            if (*s != '%') SC_DONE();
            s++;
            fmt++;
            continue;
        }

        if (*fmt == '*') { suppress = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (fmt[0] == 'h' && fmt[1] == 'h')      { lenmod = 'H'; fmt += 2; }
        else if (fmt[0] == 'l' && fmt[1] == 'l') { lenmod = 'L'; fmt += 2; }
        else if (*fmt == 'h' || *fmt == 'l' || *fmt == 'j' ||
                 *fmt == 'z' || *fmt == 't' || *fmt == 'L') {
            lenmod = (*fmt == 'j') ? 'L'
                   : ((*fmt == 'z' || *fmt == 't') ? 'l' : *fmt);
            fmt++;
        }

        conv = *fmt;
        if (conv == 0) SC_DONE();
        fmt++;

        switch (conv) {

        case 'c': {
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            int n = (width > 0) ? width : 1;
            int i = 0;
            if (*s == 0) {
                if (assigned == 0) SC_EOF();
                SC_DONE();
            }
            while (i < n && *s) {
                if (dst) dst[i] = *s;
                i++;
                s++;
            }
            if (!suppress && i < n) SC_DONE();   /* ввод оборвался */
            if (!suppress) assigned++;
            break;
        }

        case 's': {
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            int n = 0;
            while (sc_isspace((unsigned char)*s)) s++;
            if (*s == 0) {
                if (assigned == 0) SC_EOF();
                SC_DONE();
            }
            while (*s && !sc_isspace((unsigned char)*s) &&
                   (width == 0 || n < width)) {
                if (dst) dst[n] = *s;
                n++;
                s++;
            }
            if (dst) {
                dst[n] = 0;
                assigned++;
            }
            break;
        }

        case '[': {
            char set[256];
            char *dst;
            const char *q, *r;
            int invert = 0, n = 0, i;

            for (i = 0; i < 256; i++) set[i] = 0;
            if (*fmt == '^') { invert = 1; fmt++; }
            if (*fmt == ']') { set[']'] = 1; fmt++; }
            q = fmt;
            while (*q && *q != ']') q++;
            r = fmt;
            while (r < q) {                     /* заполнение с диапазонами a-z */
                if (r + 2 < q && r[1] == '-') {
                    int a = (unsigned char)r[0];
                    int b = (unsigned char)r[2];
                    while (a <= b) { set[a] = 1; a++; }
                    r += 3;
                } else {
                    set[(unsigned char)*r] = 1;
                    r++;
                }
            }
            fmt = (*q == ']') ? q + 1 : q;

            dst = suppress ? (char *)0 : va_arg(ap, char *);
            while (*s && (width == 0 || n < width)) {
                int in = set[(unsigned char)*s] ? 1 : 0;
                if (invert) in = !in;
                if (!in) break;
                if (dst) dst[n] = *s;
                n++;
                s++;
            }
            if (n == 0) {
                if (*s == 0 && assigned == 0) SC_EOF();
                SC_DONE();
            }
            if (dst) {
                dst[n] = 0;
                assigned++;
            }
            break;
        }

        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            unsigned long long v = 0;
            int neg = 0, ok;
            int base = 10;
            void *dst;

            if (conv == 'o') base = 8;
            else if (conv == 'x' || conv == 'X' || conv == 'p') base = 16;
            else if (conv == 'i') base = 0;

            while (sc_isspace((unsigned char)*s)) s++;
            if (*s == 0) {
                if (assigned == 0) SC_EOF();
                SC_DONE();
            }
            ok = sc_scan_int(&s, base, width, &v, &neg);
            if (!ok) SC_DONE();               /* ошибка совпадения */
            if (suppress) break;
            dst = va_arg(ap, void *);
            if (conv == 'd' || conv == 'i') {
                long long sv = neg ? (long long)(0ULL - v) : (long long)v;
                switch (lenmod) {
                case 'H': *(signed char *)dst    = (signed char)sv;    break;
                case 'h': *(short *)dst          = (short)sv;          break;
                case 'l': *(long *)dst           = (long)sv;           break;
                case 'L': *(long long *)dst      = (long long)sv;      break;
                default:  *(int *)dst            = (int)sv;            break;
                }
            } else if (conv == 'p') {
                *(void **)dst = (void *)(unsigned long)v;
            } else {
                switch (lenmod) {
                case 'H': *(unsigned char *)dst    = (unsigned char)v;    break;
                case 'h': *(unsigned short *)dst   = (unsigned short)v;   break;
                case 'l': *(unsigned long *)dst    = (unsigned long)v;    break;
                case 'L': *(unsigned long long *)dst = v;                 break;
                default:  *(unsigned int *)dst     = (unsigned int)v;     break;
                }
            }
            assigned++;
            break;
        }

        case 'a': case 'A': case 'e': case 'E':
        case 'f': case 'F': case 'g': case 'G': {
            double dval;
            int ok;
            void *dst;

            while (sc_isspace((unsigned char)*s)) s++;
            if (*s == 0) {
                if (assigned == 0) SC_EOF();
                SC_DONE();
            }
            dval = sc_scan_float(&s, &ok);
            if (!ok) SC_DONE();
            if (suppress) break;
            dst = va_arg(ap, void *);
            if (lenmod == 'l' || lenmod == 'L') *(double *)dst = dval;
            else                                *(float *)dst = (float)dval;
            assigned++;
            break;
        }

        case 'n': {                       /* %n не считается конверсией */
            if (!suppress) {
                int *dst = va_arg(ap, int *);
                *dst = (int)(s - str);
            }
            break;
        }

        default:                          /* неизвестная конверсия — игнор */
            break;
        }
    }

    SC_DONE();
}
