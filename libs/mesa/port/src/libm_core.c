// SPDX-License-Identifier: BSD-3-Clause
//
// Математика для mini-libc (x86-64, без libm).
//
// sqrt       — аппаратный sqrtsd;
// fmod       — аппаратный fprem (цикл с проверкой C2);
// sin/cos    — редукция аргумента по 2*pi (Cody-Waite, pi двумя частями)
//              + ряд Тейлора;
// exp        — редукция по ln2 + ряд, затем 2^k;
// log        — приведение m в [sqrt(0.5), sqrt(2)) + atanh-ряд;
// atan       — редукция x -> x/(1+sqrt(1+x^2)) + ряд;
// pow        — через exp/log с обработкой особых случаев;
// ceil/floor/trunc/round/fabs — явные операции над битами/целыми.

#include <math.h>

/* ============================================================
 * Битовые утилиты
 * ============================================================ */

union dview {
        double d;
        unsigned long long u;
};

double fabs(double x)
{
        union dview v;

        v.d = x;
        v.u &= ~(1ULL << 63);
        return v.d;
}

/* Разбирает double: x = m * 2^e, m в [0.5, 1), для x != 0. */
double frexp(double x, int *exp)
{
        union dview v;
        int e;

        v.d = x;

        if (x == 0.0 || x != x || v.u == 0x8000000000000000ULL) {
                if (exp)
                        *exp = 0;
                return x;
        }

        e = (int)((v.u >> 52) & 0x7ff);
        v.u &= ~(0x7ffULL << 52);

        if (e == 0) {
                /* субнормальные: нормализуем вручную */
                int k = 0;

                while ((v.u >> 52) == 0) {
                        v.u <<= 1;
                        k++;
                }

                *exp = 1 - 1023 - k;
                v.u |= (1022ULL << 52);
                return v.d;
        }

        if (exp)
                *exp = e - 1022;

        v.u |= (1022ULL << 52);
        return v.d;
}

double ldexp(double x, int exp)
{
        union dview v;
        int e;
        int sign;

        if (x == 0.0 || x != x)
                return x;

        v.d = x;
        sign = (int)((v.u >> 63) & 1);
        e = (int)((v.u >> 52) & 0x7ff);

        if (e == 0) {
                /*
                 * Субнормальный вход: домножаем на 2^54 (уводим в нормальные
                 * числа), уменьшаем экспоненту и повторяем обычный путь.
                 */
                double scaled = x * 18014398509481984.0;        /* 2^54 */

                exp -= 54;

                if (scaled == 0.0)
                        return sign ? -0.0 : 0.0;

                v.d = scaled;
                e = (int)((v.u >> 52) & 0x7ff);
                x = scaled;
        }

        e += exp;

        if (e >= 0x7ff)
                return sign ? -__builtin_huge_val() : __builtin_huge_val();

        if (e <= 0) {
                /*
                 * Субнормальный результат: умножаем пошагово на 0.5.
                 * exp ограничен, поэтому цикл короткий.
                 */
                double r = x;
                int k = exp;

                /* Уходим от границы нормализации. */
                while (k < 0 && r != 0.0) {
                        r *= 0.5;
                        k++;
                }

                return r;
        }

        v.u = (v.u & ~(0x7ffULL << 52)) | ((unsigned long long)e << 52);
        return v.d;
}

double trunc(double x)
{
        if (x != x || x > 9.2e18 || x < -9.2e18)
                return x;

        return (double)(long long)x;
}

double ceil(double x)
{
        double t = trunc(x);

        if (x > 0.0 && t < x)
                t += 1.0;

        return t;
}

double floor(double x)
{
        double t = trunc(x);

        if (x < 0.0 && t > x)
                t -= 1.0;

        return t;
}

double round(double x)
{
        double t = trunc(x);
        double frac = x - t;

        if (x >= 0.0) {
                if (frac >= 0.5)
                        t += 1.0;
        } else {
                if (frac <= -0.5)
                        t -= 1.0;
        }

        return t;
}

double modf(double x, double *iptr)
{
        double t;

        if (x != x) {
                if (iptr)
                        *iptr = x;
                return x;
        }

        t = trunc(x);

        if (iptr)
                *iptr = t;

        return x - t;
}

/* ============================================================
 * sqrt / fmod
 * ============================================================ */

double sqrt(double x)
{
        double r;

        if (x < 0.0)
                return __builtin_nan("");

        __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
        return r;
}

double fmod(double x, double y)
{
        double r;

        if (y == 0.0 || x != x || y != y)
                return __builtin_nan("");

        if (x == 0.0)
                return x;

        __asm__ volatile(
                "1: fprem\n"
                "fnstsw %%ax\n"
                "andw $0x0400, %%ax\n"          /* C2 — незавершённое */
                "jnz 1b\n"
                : "=t"(r)
                : "0"(x), "u"(y)
                : "ax"
        );

        return r;
}

/* ============================================================
 * sin / cos / tan
 * ============================================================ */

static const double PI_HI = 3.14159265358979156;        /* старшие биты pi */
static const double PI_LO = 1.6473226914455068e-16;
static const double TWO_PI_HI = 6.28318530717958312;    /* 2*pi */
static const double TWO_PI_LO = 3.2946453828910136e-16;

/* Редукция: x -> r в [-pi, pi], r = x - 2*pi*k. */
static double reduce_2pi(double x)
{
        double k;
        double r;

        /* x уже конечен; k = round(x / (2*pi)) */
        k = trunc(x / TWO_PI_HI);

        r = ((x - k * TWO_PI_HI) - k * TWO_PI_LO);

        /* Контрольная коррекция: r могло выйти за pi из-за ошибок. */
        if (r > PI_HI + PI_LO) {
                r -= TWO_PI_HI + TWO_PI_LO;
        } else if (r < -(PI_HI + PI_LO)) {
                r += TWO_PI_HI + TWO_PI_LO;
        }

        return r;
}

double sin(double x)
{
        double r;
        double x2;
        double sum;
        double term;
        int i;

        if (x != x)
                return x;

        if (x > 1e15 || x < -1e15)
                return __builtin_nan("");       /* редукция ненадёжна */

        /* После редукции r в [-pi, pi]: ряд сходится, знак уже в r. */
        r = reduce_2pi(x);
        x2 = r * r;

        /* ряд для sin(r): r - r^3/3! + ... - r^25/25! */
        sum = r;
        term = r;

        for (i = 1; i <= 12; i++) {
                term *= -x2 / ((2.0 * i) * (2.0 * i + 1.0));
                sum += term;
        }

        return sum;
}

double cos(double x)
{
        double r;
        double x2;
        double sum;
        double term;
        int i;

        if (x != x)
                return x;

        if (x > 1e15 || x < -1e15)
                return __builtin_nan("");

        r = reduce_2pi(x);
        x2 = r * r;

        /* ряд для cos(r): 1 - r^2/2! + ... - r^24/24! */
        sum = 1.0;
        term = 1.0;

        for (i = 1; i <= 12; i++) {
                term *= -x2 / ((2.0 * i - 1.0) * (2.0 * i));
                sum += term;
        }

        return sum;
}

double tan(double x)
{
        return sin(x) / cos(x);
}

/* ============================================================
 * exp / log / log10 / log2 / pow
 * ============================================================ */

static const double LN2_HI = 0.693147180559451;         /* ln2, старшие биты */
static const double LN2_LO = 2.3190468138462996e-17;
static const double LN10 = 2.302585092994046;
static const double INV_LN2 = 1.4426950408889634;

double exp(double x)
{
        int k;
        double r;
        double sum;
        double term;
        int i;

        if (x != x)
                return x;

        if (x > 709.782712893384)
                return __builtin_huge_val();

        if (x < -745.133219101069)
                return 0.0;

        k = (int)(x * INV_LN2 + (x >= 0 ? 0.5 : -0.5));
        r = (x - k * LN2_HI) - k * LN2_LO;

        /* exp(r), |r| <= ln2/2: ряд до r^13 */
        sum = 1.0;
        term = 1.0;

        for (i = 1; i <= 13; i++) {
                term *= r / (double)i;
                sum += term;
        }

        /* 2^k: ldexp сам обработает k в [-1075, 1024]. */
        return sum * ldexp(1.0, k);
}

double log(double x)
{
        int k;
        double m;
        double s;
        double s2;
        double sum;
        double term;
        int i;

        if (x != x)
                return x;

        if (x < 0.0)
                return __builtin_nan("");

        if (x == 0.0)
                return -__builtin_huge_val();

        m = frexp(x, &k);               /* x = m * 2^k, m в [0.5, 1) */

        /* Приводим m в [sqrt(0.5), sqrt(2)) — ~0.707..1.414. */
        if (m < 0.70710678118654752440) {
                m *= 2.0;
                k--;
        }

        /* log(m) = 2*atanh((m-1)/(m+1)). */
        s = (m - 1.0) / (m + 1.0);
        s2 = s * s;
        sum = s;
        term = s;

        for (i = 1; i <= 18; i++) {
                term *= s2;
                sum += term / (2.0 * i + 1.0);
        }

        return 2.0 * sum + (double)k * (LN2_HI + LN2_LO);
}

double log10(double x)
{
        return log(x) / LN10;
}

double log2(double x)
{
        return log(x) / (LN2_HI + LN2_LO);
}

double pow(double x, double y)
{
        int yint;
        int yodd_neg = 0;

        if (y == 0.0)
                return 1.0;

        if (x != x || y != y)
                return __builtin_nan("");

        if (x == 0.0) {
                if (y < 0.0)
                        return __builtin_huge_val();
                return 0.0;
        }

        if (x == 1.0)
                return 1.0;

        yint = (int)y;

        /* Целый показатель: считаем точно через exp2/log нельзя для x<0. */
        if ((double)yint == y && yint > -1000000 && yint < 1000000) {
                double base = x;
                double acc = 1.0;
                int n;
                int neg = 0;

                if (yint < 0) {
                        neg = 1;
                        n = -yint;
                } else {
                        n = yint;
                }

                while (n) {
                        if (n & 1)
                                acc *= base;

                        base *= base;
                        n >>= 1;
                }

                if (neg)
                        acc = 1.0 / acc;

                return acc;
        }

        if (x < 0.0)
                return __builtin_nan("");       /* нецелая степень отрицательного */

        (void)yodd_neg;
        return exp(y * log(x));
}

/* ============================================================
 * atan / atan2 / asin / acos
 * ============================================================ */

double atan(double x)
{
        double orig = x;
        int neg = 0;
        int big = 0;
        double mult = 1.0;
        double sum;
        double term;
        double x2;
        int i;

        if (x != x)
                return x;

        if (x < 0.0) {
                neg = 1;
                x = -x;
        }

        if (x > 1.0) {
                big = 1;
                x = 1.0 / x;
        }

        /* Редукция: atan(y) = 2*atan(y / (1 + sqrt(1+y^2))). */
        while (x > 0.05) {
                x = x / (1.0 + sqrt(1.0 + x * x));
                mult *= 2.0;
        }

        /* Ряд: atan(z) = z - z^3/3 + z^5/5 - ... при z <= 0.05 */
        x2 = x * x;
        sum = x;
        term = x;

        for (i = 1; i <= 10; i++) {
                term *= -x2;
                sum += term / (2.0 * i + 1.0);
        }

        sum *= mult;

        if (big)
                sum = (PI_HI + PI_LO) / 2.0 - sum;

        return neg ? -sum : sum;
}

double atan2(double y, double x)
{
        if (x != x || y != y)
                return __builtin_nan("");

        if (x == 0.0) {
                if (y == 0.0)
                        return 0.0;     /* ветвление по знаку опускаем */

                if (y > 0.0)
                        return (PI_HI + PI_LO) / 2.0;

                return -(PI_HI + PI_LO) / 2.0;
        }

        if (x > 0.0)
                return atan(y / x);

        if (y >= 0.0)
                return atan(y / x) + (PI_HI + PI_LO);

        return atan(y / x) - (PI_HI + PI_LO);
}

double asin(double x)
{
        if (x != x)
                return x;

        if (x > 1.0 || x < -1.0)
                return __builtin_nan("");

        if (x == 1.0)
                return (PI_HI + PI_LO) / 2.0;

        if (x == -1.0)
                return -(PI_HI + PI_LO) / 2.0;

        return atan(x / sqrt(1.0 - x * x));
}

double acos(double x)
{
        if (x != x)
                return x;

        if (x > 1.0 || x < -1.0)
                return __builtin_nan("");

        return (PI_HI + PI_LO) / 2.0 - asin(x);
}

/* ============================================================
 * Гиперболические
 * ============================================================ */

double sinh(double x)
{
        double e = exp(x);

        return (e - 1.0 / e) * 0.5;
}

double cosh(double x)
{
        double e = exp(x);

        return (e + 1.0 / e) * 0.5;
}

double tanh(double x)
{
        double e = exp(2.0 * x);

        return (e - 1.0) / (e + 1.0);
}

/*
 * long double (x87) обёртки — нужны tcc при вычислении
 * long double констант. long double x86_64 = 80 бит.
 */
long double ldexpl(long double x, int exp)
{
        return (long double)ldexp((double)x, exp);
}

long double frexpl(long double x, int *exp)
{
        return (long double)frexp((double)x, exp);
}

long double fabsl(long double x)
{
        return (long double)fabs((double)x);
}

long double floorl(long double x)
{
        return (long double)floor((double)x);
}

long double ceill(long double x)
{
        return (long double)ceil((double)x);
}

/* ============================================================
 * float-варианты (C99) — run.c (llama2.c): expf/sqrtf/powf и др.
 * Обёртки над double-версиями: результаты корректны (double ->
 * float сужение даёт максимум 0.5 ulp float), производительность
 * достаточна для инференса на миниатюрных моделях.
 * ============================================================ */

float fabsf(float x)
{
        return (float)fabs((double)x);
}

float fmodf(float x, float y)
{
        return (float)fmod((double)x, (double)y);
}

float modff(float x, float *iptr)
{
        double i;
        float r;

        r = (float)modf((double)x, &i);
        if (iptr)
                *iptr = (float)i;
        return r;
}

float frexpf(float x, int *exp)
{
        return (float)frexp((double)x, exp);
}

float ldexpf(float x, int exp)
{
        return (float)ldexp((double)x, exp);
}

float ceilf(float x)
{
        return (float)ceil((double)x);
}

float floorf(float x)
{
        return (float)floor((double)x);
}

float roundf(float x)
{
        return (float)round((double)x);
}

float truncf(float x)
{
        return (float)trunc((double)x);
}

float sqrtf(float x)
{
        return (float)sqrt((double)x);
}

float powf(float x, float y)
{
        return (float)pow((double)x, (double)y);
}

float expf(float x)
{
        return (float)exp((double)x);
}

float logf(float x)
{
        return (float)log((double)x);
}

float log10f(float x)
{
        return (float)log10((double)x);
}

float log2f(float x)
{
        return (float)log2((double)x);
}

float sinf(float x)
{
        return (float)sin((double)x);
}

float cosf(float x)
{
        return (float)cos((double)x);
}

float tanf(float x)
{
        return (float)tan((double)x);
}

float asinf(float x)
{
        return (float)asin((double)x);
}

float acosf(float x)
{
        return (float)acos((double)x);
}

float atanf(float x)
{
        return (float)atan((double)x);
}

float atan2f(float y, float x)
{
        return (float)atan2((double)y, (double)x);
}

float sinhf(float x)
{
        return (float)sinh((double)x);
}

float coshf(float x)
{
        return (float)cosh((double)x);
}

float tanhf(float x)
{
        return (float)tanh((double)x);
}
