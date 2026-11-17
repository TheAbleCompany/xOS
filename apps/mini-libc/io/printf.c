// SPDX-License-Identifier: BSD-3-Clause
//
// printf/fprintf/sprintf/snprintf + v-варианты для mini-libc.
//
// Архитектура: единое ядро форматирования (fmt_core), которому
// скармливается "приёмник" вывода. Есть два приёмника:
//   - sink_mem: буфер в памяти с ограничением размера (vsnprintf);
//   - sink_file: FILE* через fwrite порциями по 512 байт (vfprintf).
//
// Поддерживаемые преобразования:
//   %d %i  — знаковые целые (модификаторы h l ll z)
//   %u %o %x %X — беззнаковые целые
//   %c %s %p %%
//   %f %e %E %g %G — числа с плавающей точкой
//   флаги: '-' '0' '+' ' ' '#', ширина, точность
//
// Числа с плавающей точкой: значение нормализуется к d.dddd x 10^E,
// генерируется до 17 значащих цифр, затем округление на нужной позиции.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <internal/io.h>

/* ============================================================
 * Приёмник вывода
 * ============================================================ */

typedef struct sink {
        void (*emit)(struct sink *s, const char *data, size_t n);
        long total;                     /* полная длина вывода */
} sink_t;

typedef struct {
        sink_t base;
        char *p;                        /* куда пишем */
        size_t left;                    /* сколько байт ещё можно */
} sink_mem_t;

static void sink_mem_emit(sink_t *s, const char *data, size_t n)
{
        sink_mem_t *m = (sink_mem_t *)s;

        m->base.total += n;

        while (n--) {
                if (m->left > 1) {
                        *m->p++ = *data;
                        m->left--;
                }
                data++;
        }
}

typedef struct {
        sink_t base;
        FILE *f;
        char buf[512];
        size_t used;
} sink_file_t;

static void sink_file_flush(sink_file_t *sf)
{
        if (sf->used > 0) {
                fwrite(sf->buf, 1, sf->used, sf->f);
                sf->used = 0;
        }
}

static void sink_file_emit(sink_t *s, const char *data, size_t n)
{
        sink_file_t *sf = (sink_file_t *)s;

        sf->base.total += n;

        while (n--) {
                if (sf->used >= sizeof(sf->buf))
                        sink_file_flush(sf);

                sf->buf[sf->used++] = *data;
                data++;
        }
}

/* ============================================================
 * Целые числа
 * ============================================================ */

static const char digits_lc[] = "0123456789abcdef";
static const char digits_uc[] = "0123456789ABCDEF";

/* Разворачивает число в buf в обратном порядке (буфер >= 24 байт). */
static int u64_to_str(unsigned long long v, int base, int upper, char *buf)
{
        const char *d = upper ? digits_uc : digits_lc;
        int i = 0;
        int a, b;

        do {
                buf[i++] = d[v % (unsigned)base];
                v /= (unsigned)base;
        } while (v);

        /* Разворачиваем: цифры шли от младшей к старшей. */
        for (a = 0, b = i - 1; a < b; a++, b--) {
                char t = buf[a];

                buf[a] = buf[b];
                buf[b] = t;
        }

        return i;
}

/* ============================================================
 * Числа с плавающей точкой
 * ============================================================ */

#define SIGD 17

/*
 * value = d0.d1d2... x 10^E.
 * digit[0..ndig] — ndig+1 значащих цифр (последняя — для округления).
 */
/*
 * 10^e с высокой точностью: 1e0..1e22 в double точны,
 * дальше — цепочка умножений на 1e22 (накопленная ошибка ~1e-16).
 */
static double pow10_exact(int e)
{
        double r = 1.0;

        while (e >= 22) {
                r *= 1e22;
                e -= 22;
        }

        while (e <= -22) {
                r /= 1e22;
                e += 22;
        }

        while (e > 0) {
                r *= 10.0;
                e--;
        }

        while (e < 0) {
                r /= 10.0;
                e++;
        }

        return r;
}

static void dtoa_digits(double value, int ndig, char *digit, int *exp_out)
{
        int E = 0;
        int i;
        double x = value;

        for (i = 0; i < SIGD + 4; i++)
                digit[i] = '0';

        if (x == 0.0) {
                *exp_out = 0;
                return;
        }

        /*
         * Сначала грубо определяем порядок E (на исходном значении),
         * затем делим ОДИН раз на точную степень десятки. Повторное
         * деление на 10 копит ошибку уже к 7-й цифре (классический
         * пример: 999999.5 даёт ...4999 вместо ...5 и ломает округление).
         */
        if (x >= 10.0) {
                while (x >= 10.0) {
                        x /= 10.0;
                        E++;
                }
        } else if (x < 1.0) {
                while (x < 1.0) {
                        x *= 10.0;
                        E--;
                }
        }

        x = value / pow10_exact(E);

        /* После точного деления x мог чуть выйти за [1, 10). */
        if (x >= 10.0) {
                x /= 10.0;
                E++;
        } else if (x < 1.0) {
                x *= 10.0;
                E--;
        }

        /*
         * ТОЧНЫЙ путь: если value == N / 10^k при небольших N и k
         * (таковы все "обычные" десятичные литералы: 0.1, 42.5,
         * 999999.5, ...), то десятичные цифры известны точно — это
         * цифры N. Это устраняет дрейф повторного деления и делает
         * корректными округления на границе "половины".
         */
        {
                int k;

                for (k = 0; k <= 17; k++) {
                        double scaled = value * pow10_exact(k);
                        long long si;

                        if (scaled >= 9.2e18 || scaled <= -9.2e18)
                                break;

                        si = (long long)scaled;

                        if ((double)si == scaled && si > 0 &&
                            scaled / pow10_exact(k) == value) {
                                /* Точное десятичное представление найдено. */
                                unsigned long long n = (unsigned long long)si;
                                char rev[24];
                                int nd = 0;
                                int j;

                                do {
                                        rev[nd++] = (char)('0' + n % 10);
                                        n /= 10;
                                } while (n);

                                /* rev — цифры в обратном порядке. */
                                for (j = 0; j < nd && j < SIGD + 3; j++)
                                        digit[j] = rev[nd - 1 - j];

                                *exp_out = nd - 1 - k;
                                return;
                        }
                }
        }

        /* Генерируем ndig+1 цифр. Целочисленное усечение (int)x
         * корректно, т.к. x всегда в [1, 10). */
        for (i = 0; i <= ndig && i < SIGD + 4; i++) {
                int d = (int)x;

                if (d > 9)
                        d = 9;
                if (d < 0)
                        d = 0;

                digit[i] = (char)('0' + d);
                x = (x - (double)d) * 10.0;
        }

        *exp_out = E;
}

/* Округление цифровой строки на позиции ndig (digit[ndig] >= '5'). */
static int round_digits(char *digit, int ndig, int *E)
{
        int i;
        int carry = 1;

        for (i = ndig - 1; i >= 0 && carry; i--) {
                if (digit[i] == '9') {
                        digit[i] = '0';
                } else {
                        digit[i]++;
                        carry = 0;
                }
        }

        if (carry) {
                digit[0] = '1';
                for (i = 1; i <= ndig; i++)
                        digit[i] = '0';
                (*E)++;
        }

        return 0;
}

/*
 * Порядок значения без генерации цифр: value = d.ddd x 10^E.
 * Нужен, чтобы вычислить точку округления для %f заранее.
 */
static int decimal_exponent(double value)
{
        int E = 0;
        double x = value;

        /*
         * ВАЖНО: x == 0.0 обязан обрабатываться до цикла — иначе
         * while (x < 1.0) { x *= 10.0; } никогда не завершится
         * (0.0 * 10 == 0.0). Баг проявлялся как зависание любого
         * printf("%f", 0.0) — например, строки "achieved tok/s"
         * в run.c при нулевом времени генерации.
         */
        if (x == 0.0)
                return 0;

        if (x >= 10.0) {
                while (x >= 10.0) {
                        x /= 10.0;
                        E++;
                }
        } else if (x < 1.0) {
                while (x < 1.0) {
                        x *= 10.0;
                        E--;
                        /* предохранитель: субнормали и мусор */
                        if (E < -400)
                                break;
                }
        }

        return E;
}

/*
 * Форматирование double в строку buf (buf >= 340 байт).
 * conv: 'f' 'e' 'E' 'g' 'G'. Возвращает длину.
 */
static int fmt_double(char *buf, double value, int conv, int prec, int alt)
{
        char digit[SIGD + 4];
        char tmp[340];
        int neg = 0;
        int E = 0;
        int i, pos = 0;
        int upper = (conv == 'E' || conv == 'G');
        char echar = upper ? 'E' : 'e';
        int gconv = (conv == 'g' || conv == 'G');
        int use_e;
        int ndig;

        if (value != value) {                   /* NaN */
                memcpy(buf, "nan", 3);
                return 3;
        }

        if (value < 0) {
                neg = 1;
                value = -value;
        }

        if (value > 1.7976931348623157e308) {   /* +inf */
                memcpy(buf, "inf", 3);
                return 3;
        }

        if (prec < 0)
                prec = 6;

        if (gconv && prec == 0)
                prec = 1;

        if (gconv) {
                ndig = prec;
        } else if (conv == 'e' || conv == 'E') {
                ndig = prec + 1;
        } else {
                /*
                 * %f: округлять нужно на позиции E+1+prec, поэтому порядок
                 * E вычисляем заранее, чтобы знать, сколько цифр генерировать.
                 */
                int Ef = decimal_exponent(value);

                ndig = Ef + 1 + prec;
        }

        if (ndig > SIGD)
                ndig = SIGD;
        if (ndig < 1)
                ndig = 1;

        /*
         * Генерируем до 9 "сторожевых" цифр сверх нужных: если все они
         * нулевые — значение лежит ровно на границе "половины" и glibc
         * округляет к чётной цифре (round-half-even). Если хотя бы одна
         * сторожевая цифра ненулевая — значение не на границе, округляем
         * вверх при >= 5.
         */
        {
                int guard = 9;
                int gen = ndig + 1 + guard;
                int tie = 1;
                int gi;

                if (gen > SIGD + 3)
                        gen = SIGD + 3;

                dtoa_digits(value, gen - 1, digit, &E);

                for (gi = ndig + 1; gi < gen; gi++) {
                        if (digit[gi] != '0') {
                                tie = 0;
                                break;
                        }
                }

                if (digit[ndig] > '5') {
                        round_digits(digit, ndig, &E);
                } else if (digit[ndig] == '5') {
                        if (!tie) {
                                round_digits(digit, ndig, &E);
                        } else if ((digit[ndig - 1] - '0') % 2 == 1) {
                                /* половина: вверх, если предыдущая нечётная */
                                round_digits(digit, ndig, &E);
                        }
                }
        }

        use_e = (conv == 'e' || conv == 'E');

        if (gconv) {
                if (E >= -4 && E < prec) {
                        use_e = 0;
                        prec = prec - 1 - E;
                        if (prec < 0)
                                prec = 0;
                } else {
                        use_e = 1;
                        prec = prec - 1;
                        if (prec < 0)
                                prec = 0;
                }
        }

        if (use_e) {
                /* d.ddddde±XX */
                tmp[pos++] = digit[0];

                if (prec > 0 || alt)
                        tmp[pos++] = '.';

                for (i = 1; i <= prec; i++)
                        tmp[pos++] = (i <= ndig) ? digit[i] : '0';

                tmp[pos++] = echar;

                {
                        int e2 = E;
                        char eb[8];
                        int elen, j;

                        if (e2 < 0) {
                                tmp[pos++] = '-';
                                e2 = -e2;
                        } else {
                                tmp[pos++] = '+';
                        }

                        elen = u64_to_str((unsigned long long)e2, 10, 0, eb);

                        if (elen < 2)
                                tmp[pos++] = '0';

                        for (j = 0; j < elen; j++)
                                tmp[pos++] = eb[j];
                }
        } else {
                /* Обычная десятичная запись. */
                if (E >= 0) {
                        int int_digits = E + 1;

                        if (int_digits <= ndig) {
                                for (i = 0; i < int_digits; i++)
                                        tmp[pos++] = digit[i];
                        } else {
                                for (i = 0; i < ndig; i++)
                                        tmp[pos++] = digit[i];
                                for (i = ndig; i < int_digits; i++)
                                        tmp[pos++] = '0';
                        }

                        if (prec > 0 || alt) {
                                tmp[pos++] = '.';

                                for (i = 0; i < prec; i++) {
                                        int idx = int_digits + i;

                                        if (idx >= 0 && idx <= ndig &&
                                            idx <= SIGD + 2)
                                                tmp[pos++] = digit[idx];
                                        else
                                                tmp[pos++] = '0';
                                }
                        }
                } else {
                        tmp[pos++] = '0';

                        if (prec > 0 || alt)
                                tmp[pos++] = '.';

                        for (i = 0; i < prec; i++) {
                                int idx = E + 1 + i;

                                if (idx >= 0 && idx <= ndig &&
                                    idx <= SIGD + 2)
                                        tmp[pos++] = digit[idx];
                                else
                                        tmp[pos++] = '0';
                        }
                }
        }

        /*
         * %g (без '#'): срезаем хвостовые нули дробной части и точку,
         * если после неё ничего не осталось. Для e-нотации нули
         * срезаем только в мантиссе (до 'e'/'E').
         */
        if (gconv && !alt) {
                int dot = -1;
                int exp_pos = pos;
                int end;

                for (i = 0; i < pos; i++) {
                        if (tmp[i] == 'e' || tmp[i] == 'E') {
                                exp_pos = i;
                                break;
                        }
                }

                for (i = 0; i < exp_pos; i++) {
                        if (tmp[i] == '.') {
                                dot = i;
                                break;
                        }
                }

                end = exp_pos;

                if (dot >= 0) {
                        while (end > dot + 1 && tmp[end - 1] == '0')
                                end--;

                        if (end == dot + 1)
                                end--;      /* убираем и точку */
                }

                if (end != exp_pos) {
                        /* сдвигаем экспоненту на место срезанных нулей */
                        int tail = pos - exp_pos;
                        int k;

                        for (k = 0; k < tail; k++)
                                tmp[end + k] = tmp[exp_pos + k];

                        pos = end + tail;
                }
        }

        if (neg) {
                memmove(tmp + 1, tmp, pos);
                tmp[0] = '-';
                pos++;
        }

        memcpy(buf, tmp, pos);
        return pos;
}

/* ============================================================
 * Ядро форматирования
 * ============================================================ */

static void sink_pad(sink_t *s, char c, int n)
{
        char tmp[16];
        int i;

        for (i = 0; i < 16; i++)
                tmp[i] = c;

        while (n > 0) {
                int chunk = n > 16 ? 16 : n;

                s->emit(s, tmp, chunk);
                n -= chunk;
        }
}

static void fmt_core(sink_t *s, const char *format, va_list ap)
{
        const char *cur = format;

        while (*cur) {
                int minus = 0, zero = 0, plus = 0, space = 0, alt = 0;
                int width = 0, prec = -1;
                int lmod = 0;           /* 0 | 'h' | 'l' | 'L' */
                char spec;
                int prefix_len = 0;    /* length of "0x" in numbuf */
                char cbuf[2];
                char numbuf[64];
                char fbuf[400];
                const char *sval;
                int nval;
                char signchar = 0;

                if (*cur != '%') {
                        s->emit(s, cur, 1);
                        cur++;
                        continue;
                }

                cur++;

                /* Флаги. */
                while (*cur == '-' || *cur == '0' || *cur == '+' ||
                       *cur == ' ' || *cur == '#') {
                        switch (*cur) {
                        case '-': minus = 1; break;
                        case '0': zero = 1; break;
                        case '+': plus = 1; break;
                        case ' ': space = 1; break;
                        case '#': alt = 1; break;
                        }
                        cur++;
                }

                /* Ширина. */
                if (*cur == '*') {
                        width = va_arg(ap, int);
                        if (width < 0) {
                                minus = 1;
                                width = -width;
                        }
                        cur++;
                } else {
                        while (*cur >= '0' && *cur <= '9') {
                                width = width * 10 + (*cur - '0');
                                cur++;
                        }
                }

                /* Точность. */
                if (*cur == '.') {
                        cur++;
                        if (*cur == '*') {
                                prec = va_arg(ap, int);
                                cur++;
                        } else {
                                prec = 0;
                                while (*cur >= '0' && *cur <= '9') {
                                        prec = prec * 10 + (*cur - '0');
                                        cur++;
                                }
                        }
                }

                /* Модификаторы длины. */
                while (*cur == 'l' || *cur == 'h' || *cur == 'z' ||
                       *cur == 'L' || *cur == 'j' || *cur == 't') {
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
                case 'i': {
                        long long v;

                        if (lmod == 'L')
                                v = va_arg(ap, long long);
                        else if (lmod == 'l')
                                v = va_arg(ap, long);
                        else
                                v = va_arg(ap, int);

                        if (v < 0) {
                                signchar = '-';
                                v = -v;
                        } else if (plus) {
                                signchar = '+';
                        } else if (space) {
                                signchar = ' ';
                        }

                        prefix_len = 0;
                        nval = u64_to_str((unsigned long long)v, 10, 0,
                                          numbuf);
                        goto print_num;
                }

                case 'u':
                case 'o':
                case 'x':
                case 'X': {
                        unsigned long long v;
                        int base = (spec == 'o') ? 8 :
                                   (spec == 'u') ? 10 : 16;

                        if (lmod == 'L')
                                v = va_arg(ap, unsigned long long);
                        else if (lmod == 'l')
                                v = va_arg(ap, unsigned long);
                        else
                                v = va_arg(ap, unsigned int);

                        prefix_len = 0;

                        if (alt && (spec == 'x' || spec == 'X') && v != 0) {
                                numbuf[0] = '0';
                                numbuf[1] = spec;
                                prefix_len = 2;

                                nval = prefix_len +
                                       u64_to_str(v, base, spec == 'X',
                                                  numbuf + prefix_len);
                        } else if (alt && spec == 'o' &&
                                   numbuf[0] != '0') {
                                int k;

                                for (k = nval; k > 0; k--)
                                        numbuf[k] = numbuf[k - 1];
                                numbuf[0] = '0';
                                nval++;
                        } else {
                                nval = u64_to_str(v, base, spec == 'X',
                                                  numbuf);
                        }

                        goto print_num;
                }

                case 'c': {
                        cbuf[0] = (char)va_arg(ap, int);

                        sval = cbuf;
                        nval = 1;
                        prefix_len = 0;
                        goto print_str;
                }

                case 's': {
                        sval = va_arg(ap, const char *);

                        if (sval == NULL)
                                sval = "(null)";

                        nval = 0;
                        while (sval[nval])
                                nval++;

                        if (prec >= 0 && prec < nval)
                                nval = prec;

                        prefix_len = 0;
                        goto print_str;
                }

                case 'p': {
                        void *v = va_arg(ap, void *);

                        numbuf[0] = '0';
                        numbuf[1] = 'x';
                        nval = 2 + u64_to_str((unsigned long long)v, 16, 0,
                                              numbuf + 2);
                        signchar = 0;
                        prefix_len = 0;
                        goto print_num;
                }

                case 'f':
                case 'e':
                case 'E':
                case 'g':
                case 'G': {
                        double v = va_arg(ap, double);
                        int flen;

                        flen = fmt_double(fbuf, v, spec, prec, alt);
                        fbuf[flen] = '\0';

                        sval = fbuf;
                        nval = flen;
                        prefix_len = 0;
                        goto print_str;
                }

                case '%':
                        s->emit(s, "%", 1);
                        continue;

                default:
                        s->emit(s, "%", 1);
                        s->emit(s, &spec, 1);
                        continue;
                }

print_num:
                /*
                 * numbuf: [префикс][цифры] в естественном порядке.
                 * nval — общая длина, prefix_len — длина префикса.
                 */
                if (prec >= 0) {
                        int nzeros = prec - (nval - prefix_len);

                        if (nzeros > 0) {
                                int k;

                                /* Сдвигаем цифры вправо, нули после префикса. */
                                for (k = nval; k >= prefix_len; k--)
                                        numbuf[k + nzeros] = numbuf[k];

                                for (k = 0; k < nzeros; k++)
                                        numbuf[prefix_len + k] = '0';

                                nval += nzeros;
                        }

                        zero = 0;   /* точность подавляет флаг '0' */
                }

                sval = numbuf;

print_str:
                {
                        int total = nval + (signchar ? 1 : 0);

                        if (zero && !minus && prefix_len > 0 &&
                            width > total) {
                                /*
                                 * Формат вида %#08x: знак, префикс 0x,
                                 * затем нули ширины и цифры.
                                 * ВАЖНО: здесь нельзя break — мы внутри
                                 * цикла while, break завершил бы его.
                                 */
                                if (signchar)
                                        s->emit(s, &signchar, 1);

                                s->emit(s, sval, prefix_len);
                                sink_pad(s, '0', width - total);
                                s->emit(s, sval + prefix_len,
                                        nval - prefix_len);
                        } else {
                                if (!minus && width > total) {
                                        if (zero) {
                                                if (signchar) {
                                                        s->emit(s, &signchar, 1);
                                                        signchar = 0;
                                                }
                                                sink_pad(s, '0',
                                                         width - total);
                                        } else {
                                                sink_pad(s, ' ',
                                                         width - total);
                                        }
                                }

                                if (signchar)
                                        s->emit(s, &signchar, 1);

                                s->emit(s, sval, nval);

                                if (minus && width > total)
                                        sink_pad(s, ' ', width - total);
                        }
                }
        }
}

/* ============================================================
 * Публичные функции
 * ============================================================ */

int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
        sink_mem_t m;
        sink_t *s = &m.base;

        if (str == NULL)
                size = 0;

        s->emit = sink_mem_emit;
        s->total = 0;
        m.p = str;
        m.left = size;

        if (size > 0)
                str[0] = '\0';

        fmt_core(s, format, ap);

        if (m.left > 0)
                m.p[0] = '\0';  /* терминатор в пределах буфера */

        return (int)s->total;
}

int snprintf(char *str, size_t size, const char *format, ...)
{
        va_list ap;
        int ret;

        va_start(ap, format);
        ret = vsnprintf(str, size, format, ap);
        va_end(ap);

        return ret;
}

int sprintf(char *str, const char *format, ...)
{
        va_list ap;
        int ret;

        va_start(ap, format);
        ret = vsnprintf(str, (size_t)-1 / 2, format, ap);
        va_end(ap);

        return ret;
}

int vsprintf(char *str, const char *format, va_list ap)
{
        return vsnprintf(str, (size_t)-1 / 2, format, ap);
}

int vfprintf(FILE *stream, const char *format, va_list ap)
{
        sink_file_t sf;

        if (stream == NULL || format == NULL)
                return -1;

        sf.base.emit = sink_file_emit;
        sf.base.total = 0;
        sf.f = stream;
        sf.used = 0;

        fmt_core(&sf.base, format, ap);
        sink_file_flush(&sf);

        return (int)sf.base.total;
}

int fprintf(FILE *stream, const char *format, ...)
{
        va_list ap;
        int ret;

        va_start(ap, format);
        ret = vfprintf(stream, format, ap);
        va_end(ap);

        return ret;
}

int vprintf(const char *format, va_list ap)
{
        return vfprintf(stdout, format, ap);
}

int printf(const char *format, ...)
{
        va_list ap;
        int ret;

        va_start(ap, format);
        ret = vfprintf(stdout, format, ap);
        va_end(ap);

        return ret;
}
