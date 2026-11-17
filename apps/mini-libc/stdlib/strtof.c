/*
 * stdlib/strtof.c — strtof/strtold для mini-libc.
 *
 * Реализация через strtod:
 *   - strtof:  double имеет 53 бита мантиссы, float — 24, поэтому
 *     приведение (float)strtod(...) всегда корректно;
 *   - strtold: long double x86_64 — 64 бита мантиссы, т.е. точность
 *     выше double только для литералов длиннее ~19 значащих цифр.
 *     Ограничение mini-libc: длинные long double литералы дают
 *     погрешность в последнем разряде (для tcc достаточно).
 */

#include <stdlib.h>
#include <errno.h>

float strtof(const char *nptr, char **endptr)
{
        return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr)
{
        return (long double)strtod(nptr, endptr);
}
