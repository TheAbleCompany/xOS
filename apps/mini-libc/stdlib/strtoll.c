/*
 * stdlib/strtoll.c — strtoll/strtoull для mini-libc (ArtyomX64).
 *
 * Корректная реализация по C99/C11:
 *   - пропускаем пробелы (isspace), опциональный знак;
 *   - base 0: автоопределение (0x/0X -> 16, 0 -> 8, иначе 10);
 *   - base 16: допускается префикс 0x/0X;
 *   - переполнение: ERANGE + ULLONG_MAX/LLONG_MAX/LLONG_MIN;
 *   - endptr: указатель на начало числа, если цифр не было — на nptr.
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>

static unsigned long long strtox_ull(const char *nptr, char **endptr,
                                     int base, int is_signed)
{
        const char *s = nptr;
        unsigned long long acc;
        unsigned long long cutoff;
        int neg = 0;
        int any = 0;
        int cutlim;
        int c;

        if (base != 0 && (base < 2 || base > 36)) {
                errno = EINVAL;
                if (endptr)
                        *endptr = (char *)nptr;
                return 0;
        }

        while (isspace((unsigned char)*s))
                s++;

        if (*s == '+') {
                s++;
        } else if (*s == '-') {
                neg = 1;
                s++;
        }

        /* Автоопределение основания. */
        if ((base == 0 || base == 16) &&
            s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
            (isxdigit((unsigned char)s[2]))) {
                s += 2;
                base = 16;
        } else if (base == 0) {
                base = (s[0] == '0') ? 8 : 10;
        }

        acc = 0;

        /* Порог переполнения для беззнакового накопителя. */
        if (is_signed)
                cutoff = neg ? (unsigned long long)LLONG_MAX + 1ULL
                             : (unsigned long long)LLONG_MAX;
        else
                cutoff = ULLONG_MAX;
        cutlim = (int)(cutoff % (unsigned long long)base);
        cutoff /= (unsigned long long)base;

        while (1) {
                c = (unsigned char)*s;
                if (c >= '0' && c <= '9')
                        c -= '0';
                else if (c >= 'a' && c <= 'z')
                        c -= 'a' - 10;
                else if (c >= 'A' && c <= 'Z')
                        c -= 'A' - 10;
                else
                        break;

                if (c >= base)
                        break;

                if (any < 0) {
                        /* Уже переполнение — только двигаем endptr. */
                } else if (acc > cutoff ||
                           (acc == cutoff && c > cutlim)) {
                        any = -1;
                        errno = ERANGE;
                } else {
                        acc = acc * (unsigned long long)base +
                              (unsigned long long)c;
                }
                if (any == 0)
                        any = 1;
                s++;
        }

        if (endptr)
                *endptr = (char *)(any ? s : nptr);

        if (any < 0)
                return neg ? (unsigned long long)LLONG_MIN
                           : (unsigned long long)LLONG_MAX;

        if (neg && is_signed)
                return (unsigned long long)(-(long long)acc);
        if (neg && !is_signed)
                return (unsigned long long)(-(long long)acc);

        return acc;
}

long long strtoll(const char *nptr, char **endptr, int base)
{
        return (long long)strtox_ull(nptr, endptr, base, 1);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
        return strtox_ull(nptr, endptr, base, 0);
}

/* glibc-совместимый алиас (некоторые сборки тянут имя __isoc23_*). */
unsigned long long __isoc23_strtoull(const char *str, char **endptr, int base);
unsigned long long __isoc23_strtoull(const char *str, char **endptr, int base)
{
        return strtoull(str, endptr, base);
}
