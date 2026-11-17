// SPDX-License-Identifier: BSD-3-Clause
//
// fscanf/scanf/sscanf для mini-libc.
//
// Поддерживаются (в объёме, необходимом lua-1.0 и типичным программам):
//   %d %i %u %o %x %X    — целые (модификаторы h l ll)
//   %f %e %g %E %G       — вещественные ('l' -> double*, иначе float*)
//   %s                   — слово (пропускает ведущие пробелы)
//   %c                   — символы без пропуска пробелов (с шириной)
//   %[набор]             — скансет, поддержка '^', ']' и диапазонов
//   %n %% и подавление '*'
//
// Литеральные символы формата обязаны совпасть с вводом.
// Пробел в формате пропускает произвольное количество пробелов во вводе.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

/* ============================================================
 * Абстрактный источник символов
 * ============================================================ */

typedef struct {
        int (*getc_fn)(void *ctx);
        void (*unget_fn)(int c, void *ctx);
        void *ctx;
} scanf_src_t;

static int src_getc(scanf_src_t *src)
{
        return src->getc_fn(src->ctx);
}

static void src_unget(scanf_src_t *src, int c)
{
        if (c != EOF)
                src->unget_fn(c, src->ctx);
}

/* --- источник: FILE --- */

static int file_getc(void *ctx)
{
        return fgetc((FILE *)ctx);
}

static void file_unget(int c, void *ctx)
{
        ungetc(c, (FILE *)ctx);
}

/* --- источник: строка --- */

typedef struct {
        const char *p;
} str_src_t;

static int str_getc(void *ctx)
{
        str_src_t *s = ctx;

        if (*s->p == '\0')
                return EOF;

        return (unsigned char)*s->p++;
}

static void str_unget(int c, void *ctx)
{
        str_src_t *s = ctx;

        if (c != EOF)
                s->p--;
}

/* ============================================================
 * Чтение целого числа
 * ============================================================ */

static int scan_int(scanf_src_t *src, int base, int width,
                    int *assigned_flag, void *target, int size_mod)
{
        unsigned long long acc = 0;
        int neg = 0;
        int any = 0;
        int c;

        c = src_getc(src);

        /* Знак. */
        if (c == '+' || c == '-') {
                neg = (c == '-');
                if (width == 1)
                        return 0;       /* вся ширина ушла на знак */
                if (width > 0)
                        width--;
                c = src_getc(src);
        }

        /* Префиксы для base=0 (%i) и 0x для base=16 (%x). */
        if (c == '0' && (base == 0 || base == 16)) {
                int c2;

                if (width > 0 && --width == 0) {
                        /* вся ширина ушла на '0' — это уже число 0 */
                        any = 1;
                        c = EOF;
                        goto have_digits;
                }

                c2 = src_getc(src);

                if (c2 == 'x' || c2 == 'X') {
                        if (width > 0 && --width == 0) {
                                /* "0x" без цифр в ширине: трактуем как 0 */
                                any = 1;
                                c = EOF;
                                goto have_digits;
                        }

                        base = 16;
                        c = src_getc(src);
                } else {
                        /* "0..." — восьмеричное или просто 0 */
                        if (base == 0)
                                base = 8;
                        any = 1;        /* ведущий '0' */
                        acc = 0;
                        if (c2 != EOF)
                                c = c2;
                        else
                                c = EOF;
                        goto have_digits;
                }
        } else if (base == 0) {
                base = 10;
        }

        while (c != EOF) {
                int d;

                if (c >= '0' && c <= '9')
                        d = c - '0';
                else if (c >= 'a' && c <= 'f')
                        d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                        d = c - 'A' + 10;
                else
                        break;

                if (d >= base)
                        break;

                acc = acc * (unsigned)base + (unsigned)d;
                any = 1;

                if (width > 0 && --width == 0)
                        break;

                c = src_getc(src);
        }

have_digits:
        if (c != EOF && !any && c != '0')
                src_unget(src, c);
        else if (c != EOF)
                src_unget(src, c);

        if (!any) {
                if (assigned_flag)
                        *assigned_flag = 0;
                return 0;
        }

        if (size_mod == 'l') {
                if (neg)
                        *(long *)target = -(long)acc;
                else
                        *(long *)target = (long)acc;
        } else if (size_mod == 'L') {
                if (neg)
                        *(long long *)target = -(long long)acc;
                else
                        *(long long *)target = (long long)acc;
        } else if (size_mod == 'h') {
                if (neg)
                        *(short *)target = -(short)acc;
                else
                        *(short *)target = (short)acc;
        } else {
                if (neg)
                        *(int *)target = -(int)acc;
                else
                        *(int *)target = (int)acc;
        }

        if (assigned_flag)
                *assigned_flag = 1;

        return 1;
}

/* Чтение беззнакового (%u %o %x %X). */
static int scan_uint(scanf_src_t *src, int base, int width, int *ok,
                     void *target, int size_mod)
{
        unsigned long long acc = 0;
        int any = 0;
        int c;

        c = src_getc(src);

        while (c != EOF) {
                int d;

                if (c >= '0' && c <= '9')
                        d = c - '0';
                else if (c >= 'a' && c <= 'f')
                        d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                        d = c - 'A' + 10;
                else
                        break;

                if (d >= base)
                        break;

                acc = acc * (unsigned)base + (unsigned)d;
                any = 1;

                if (width > 0 && --width == 0)
                        break;

                c = src_getc(src);
        }

        if (c != EOF)
                src_unget(src, c);

        if (!any) {
                if (ok)
                        *ok = 0;
                return 0;
        }

        if (size_mod == 'l')
                *(unsigned long *)target = (unsigned long)acc;
        else if (size_mod == 'L')
                *(unsigned long long *)target = acc;
        else if (size_mod == 'h')
                *(unsigned short *)target = (unsigned short)acc;
        else
                *(unsigned int *)target = (unsigned int)acc;

        if (ok)
                *ok = 1;

        return 1;
}

/*
 * Чтение вещественного.
 *
 * Собираем символы "плавающего" набора [0-9.+-eE] в буфер, затем
 * обрезаем до самого длинного префикса, который понимает strtod.
 * Откат лишних символов в поток не делаем сознательно: у FILE-источника
 * есть только односимвольный pushback, а на корректном входе
 * ("123.45 next") набор символов и так останавливается на разделителе.
 */
static int scan_float(scanf_src_t *src, int width, int *ok, void *target,
                      int is_double)
{
        char buf[128];
        int i = 0;
        int c;
        char *endp = NULL;

        while (1) {
                c = src_getc(src);

                if (c == EOF)
                        break;

                if (c != '.' && c != '+' && c != '-' && c != 'e' && c != 'E' &&
                    (c < '0' || c > '9'))
                        break;          /* не входит в набор — отдадим назад */

                if (i < (int)sizeof(buf) - 1)
                        buf[i++] = (char)c;

                if (width > 0 && --width == 0)
                        break;
        }

        if (c != EOF)
                src_unget(src, c);

        buf[i] = '\0';

        /* Самый длинный корректный префикс числа. */
        (void)strtod(buf, &endp);

        if (endp != NULL)
                i = (int)(endp - buf);

        if (i <= 0 || (buf[0] != '.' && buf[0] != '+' && buf[0] != '-' &&
                       (buf[0] < '0' || buf[0] > '9'))) {
                if (ok)
                        *ok = 0;
                return 0;
        }

        buf[i] = '\0';

        if (is_double)
                *(double *)target = strtod(buf, NULL);
        else
                *(float *)target = (float)strtod(buf, NULL);

        if (ok)
                *ok = 1;

        return 1;
}

/* ============================================================
 * Ядро vfscanf
 * ============================================================ */

static int vfscanf_core(scanf_src_t *src, const char *format, va_list ap)
{
        int assigned = 0;
        const char *cur = format;
        int input_eof = 0;

        while (*cur) {
                int suppress = 0;
                int width = 0;
                int lmod = 0;
                char spec;
                int ok = 0;

                /* Пробел в формате — пропускаем пробелы во вводе. */
                if (isspace((unsigned char)*cur)) {
                        int c;

                        while (isspace((unsigned char)(c = src_getc(src))))
                                ;

                        if (c != EOF)
                                src_unget(src, c);
                        else
                                input_eof = 1;

                        cur++;
                        continue;
                }

                if (*cur != '%') {
                        /* Литеральный символ — обязан совпасть. */
                        int c = src_getc(src);

                        if (c == EOF) {
                                input_eof = 1;
                                break;
                        }

                        if (c != (unsigned char)*cur) {
                                src_unget(src, c);
                                break;
                        }

                        cur++;
                        continue;
                }

                cur++;

                if (*cur == '*') {
                        suppress = 1;
                        cur++;
                }

                while (*cur >= '0' && *cur <= '9')
                        width = width * 10 + (*cur++ - '0');

                while (*cur == 'l' || *cur == 'h' || *cur == 'L') {
                        if (*cur == 'l') {
                                if (lmod == 'l')
                                        lmod = 'L';
                                else
                                        lmod = 'l';
                        } else if (lmod == 0) {
                                lmod = *cur;
                        }
                        cur++;
                }

                spec = *cur;
                if (spec == '\0')
                        break;
                cur++;

                switch (spec) {
                case 'd':
                case 'i':
                case 'u':
                case 'o':
                case 'x':
                case 'X': {
                        int base = (spec == 'd') ? 10 :
                                   (spec == 'i') ? 0 :
                                   (spec == 'o') ? 8 : 16;
                        void *target = suppress ? NULL : va_arg(ap, void *);

                        /* Пропускаем пробелы. */
                        {
                                int c;

                                while (isspace((unsigned char)
                                               (c = src_getc(src))))
                                        ;

                                if (c == EOF) {
                                        input_eof = 1;
                                        goto finish;
                                }

                                src_unget(src, c);
                        }

                        if (spec == 'd' || spec == 'i')
                                scan_int(src, base, width, &ok, target, lmod);
                        else
                                scan_uint(src, base, width, &ok, target, lmod);

                        if (ok && !suppress)
                                assigned++;
                        else if (!ok)
                                goto finish;

                        break;
                }

                case 'f':
                case 'e':
                case 'g':
                case 'E':
                case 'G': {
                        void *target = suppress ? NULL : va_arg(ap, void *);

                        {
                                int c;

                                while (isspace((unsigned char)
                                               (c = src_getc(src))))
                                        ;

                                if (c == EOF) {
                                        input_eof = 1;
                                        goto finish;
                                }

                                src_unget(src, c);
                        }

                        scan_float(src, width, &ok, target, lmod == 'l');

                        if (ok && !suppress)
                                assigned++;
                        else if (!ok)
                                goto finish;

                        break;
                }

                case 's': {
                        char *out = suppress ? NULL : va_arg(ap, char *);
                        int c;
                        int n = 0;

                        int wmax = width ? width : 255;

                        while (isspace((unsigned char)(c = src_getc(src))))
                                ;

                        if (c == EOF) {
                                input_eof = 1;
                                goto finish;
                        }

                        while (c != EOF && !isspace((unsigned char)c)) {
                                /*
                                 * Считаем совпавшие символы всегда:
                                 * при подавлении out == NULL, но пустое
                                 * совпадение должно определяться по вводу,
                                 * а не по факту записи.
                                 */
                                if (out && n < wmax)
                                        out[n] = (char)c;

                                n++;

                                if (width > 0 && --width == 0)
                                        break;

                                c = src_getc(src);
                        }

                        if (c != EOF)
                                src_unget(src, c);

                        if (n == 0)
                                goto finish;

                        if (out) {
                                int term = n < wmax ? n : wmax;

                                out[term] = '\0';
                        }

                        if (!suppress)
                                assigned++;

                        break;
                }

                case 'c': {
                        char *out = suppress ? NULL : va_arg(ap, char *);
                        int c;
                        int n = 0;
                        int w = width ? width : 1;

                        while (n < w) {
                                c = src_getc(src);

                                if (c == EOF) {
                                        input_eof = 1;
                                        goto finish;
                                }

                                if (out)
                                        out[n] = (char)c;

                                n++;
                        }

                        if (!suppress)
                                assigned++;

                        break;
                }

                case '[': {
                        char set[256];
                        int invert = 0;
                        char *out = suppress ? NULL : va_arg(ap, char *);
                        int n = 0;
                        int c;

                        memset(set, 0, sizeof(set));

                        if (*cur == '^') {
                                invert = 1;
                                cur++;
                        }

                        if (*cur == ']') {
                                set[(unsigned char)']'] = 1;
                                cur++;
                        }

                        while (*cur && *cur != ']') {
                                if (*cur == '-' && cur[1] && cur[1] != ']' &&
                                    cur[-1] != '[' && cur[-1] != '^') {
                                        char lo = cur[-1] == '-' ?
                                                '\0' : *(cur - 1);
                                        char hi = cur[1];
                                        int k;

                                        (void)lo;
                                        for (k = (unsigned char)*(cur - 1);
                                             k <= (unsigned char)hi; k++)
                                                set[k] = 1;

                                        cur += 2;
                                        continue;
                                }

                                set[(unsigned char)*cur] = 1;
                                cur++;
                        }

                        if (*cur == ']')
                                cur++;

                        int wmax = width ? width : 255;

                        c = src_getc(src);

                        if (c == EOF) {
                                input_eof = 1;
                                goto finish;
                        }

                        while (c != EOF) {
                                int in_set = set[(unsigned char)c];

                                if (invert)
                                        in_set = !in_set;

                                if (!in_set)
                                        break;

                                if (out && n < wmax)
                                        out[n] = (char)c;

                                n++;

                                if (width > 0 && --width == 0)
                                        break;

                                c = src_getc(src);
                        }

                        if (c != EOF)
                                src_unget(src, c);

                        if (n == 0)
                                goto finish;

                        if (out) {
                                int term = n < wmax ? n : wmax;

                                out[term] = '\0';
                        }

                        if (!suppress)
                                assigned++;

                        break;
                }

                case 'n': {
                        int *out = va_arg(ap, int *);

                        if (out)
                                *out = 0;       /* позицию не считаем точно */

                        break;
                }

                case '%': {
                        int c = src_getc(src);

                        if (c == EOF) {
                                input_eof = 1;
                                goto finish;
                        }

                        if (c != '%') {
                                src_unget(src, c);
                                goto finish;
                        }

                        break;
                }

                default:
                        /* неизвестная конверсия — останавливаемся */
                        goto finish;
                }
        }

finish:
        if (assigned == 0 && input_eof)
                return EOF;

        return assigned;
}

/* ============================================================
 * Публичные функции
 * ============================================================ */

int vfscanf(FILE *stream, const char *format, va_list ap)
{
        scanf_src_t src;

        src.getc_fn = file_getc;
        src.unget_fn = file_unget;
        src.ctx = stream;

        return vfscanf_core(&src, format, ap);
}

int fscanf(FILE *stream, const char *format, ...)
{
        va_list ap;
        int ret;

        va_start(ap, format);
        ret = vfscanf(stream, format, ap);
        va_end(ap);

        return ret;
}

int scanf(const char *format, ...)
{
        va_list ap;
        int ret;

        va_start(ap, format);
        ret = vfscanf(stdin, format, ap);
        va_end(ap);

        return ret;
}

int vsscanf(const char *str, const char *format, va_list ap)
{
        scanf_src_t src;
        str_src_t s;

        s.p = str;

        src.getc_fn = str_getc;
        src.unget_fn = str_unget;
        src.ctx = &s;

        return vfscanf_core(&src, format, ap);
}

int sscanf(const char *str, const char *format, ...)
{
        va_list ap;
        int ret;

        va_start(ap, format);
        ret = vsscanf(str, format, ap);
        va_end(ap);

        return ret;
}
