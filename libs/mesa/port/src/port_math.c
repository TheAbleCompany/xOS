#include <stddef.h>
#include <stdint.h>

#include "../../../lib/common.h"

double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double modf(double x, double *ip);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double trunc(double x);
double round(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
float fabsf(float x);
float fmodf(float x, float y);
float modff(float x, float *ip);
float frexpf(float x, int *exp);
float ldexpf(float x, int exp);
float ceilf(float x);
float floorf(float x);
float roundf(float x);
float truncf(float x);
float sqrtf(float x);
float powf(float x, float y);
float expf(float x);
float logf(float x);
float log10f(float x);
float log2f(float x);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
long double ldexpl(long double x, int exp);
long double frexpl(long double x, int *exp);
long double fabsl(long double x);
long double floorl(long double x);
long double ceill(long double x);

double exp2(double x)
{
    return pow(2.0, x);
}

float exp2f(float x)
{
    return (float)pow(2.0, (double)x);
}

double copysign(double x, double y)
{
    union { double d; uint64_t u; } a, b;

    a.d = x;
    b.d = y;
    a.u = (a.u & 0x7FFFFFFFFFFFFFFFULL) | (b.u & 0x8000000000000000ULL);

    return a.d;
}

float copysignf(float x, float y)
{
    union { float f; uint32_t u; } a, b;

    a.f = x;
    b.f = y;
    a.u = (a.u & 0x7FFFFFFFu) | (b.u & 0x80000000u);

    return a.f;
}

double fmax(double a, double b)
{
    if (a != a)
        return b;
    if (b != b)
        return a;
    return a > b ? a : b;
}

double fmin(double a, double b)
{
    if (a != a)
        return b;
    if (b != b)
        return a;
    return a < b ? a : b;
}

float fmaxf(float a, float b)
{
    if (a != a)
        return b;
    if (b != b)
        return a;
    return a > b ? a : b;
}

float fminf(float a, float b)
{
    if (a != a)
        return b;
    if (b != b)
        return a;
    return a < b ? a : b;
}

double fma(double a, double b, double c)
{
    return a * b + c;
}

float fmaf(float a, float b, float c)
{
    return a * b + c;
}

double rint(double x)
{
    double t = trunc(x);

    if (fabs(x - t) > 0.5)
        return t + (x > 0.0 ? 1.0 : -1.0);

    if (fabs(x - t) == 0.5) {
        long n = (long)t;
        if (n & 1L)
            return t + (x > 0.0 ? 1.0 : -1.0);
    }

    return t;
}

float rintf(float x)
{
    return (float)rint((double)x);
}

long lrint(double x)
{
    return (long)rint(x);
}

long lrintf(float x)
{
    return (long)rint((double)x);
}

long lroundf(float x)
{
    return (long)round((double)x);
}

long long llrint(double x)
{
    return (long long)rint(x);
}

long long llround(double x)
{
    return (long long)round(x);
}

int isnormal(double x)
{
    union { double d; uint64_t u; } v;
    uint64_t exp;

    v.d = x;
    exp = (v.u >> 52) & 0x7FFu;

    return (exp != 0 && exp != 0x7FFu) ? 1 : 0;
}

double cbrt(double x)
{
    double r;
    int i;

    if (x == 0.0)
        return x;

    if (x < 0.0)
        return -cbrt(-x);

    r = x > 1.0 ? x : 1.0;
    for (i = 0; i < 60; i++)
        r = (2.0 * r + x / (r * r)) / 3.0;

    return r;
}

float cbrtf(float x)
{
    return (float)cbrt((double)x);
}
