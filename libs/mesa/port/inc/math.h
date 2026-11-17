#ifndef MESA_PORT_MATH_H
#define MESA_PORT_MATH_H
#ifdef __cplusplus
extern "C" {
#endif
#include <float.h>
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_LOG2E 1.4426950408889634074
#define M_LN2 0.69314718055994530942
#define M_SQRT2 1.4142135623730950488
#define M_E 2.7182818284590452354
#define HUGE_VALF __builtin_huge_valf()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#ifndef __cplusplus
#define isinf(x) __builtin_isinf(x)
#define isnan(x) __builtin_isnan(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x) __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(0, 1, 2, 3, 4, (x))
#else
int isinf(double x);
int isnan(double x);
int isfinite(double x);
int isnormal(double x);
int signbit(double x);
int fpclassify(double x);
#endif
#define FP_NORMAL 4
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_INFINITE 1
#define FP_NAN 0
typedef double double_t;
typedef float float_t;
double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double pow(double x, double y);
double log2(double x);
double exp2(double x);
double sin(double x);
double cos(double x);
double atan2(double y, double x);
float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float powf(float x, float y);
float log2f(float x);
float exp2f(float x);
float sinf(float x);
float cosf(float x);
float roundf(float x);
float truncf(float x);
long lrintf(float x);
long lroundf(float x);
int __isinf(double x);
float fminf(float a, float b);
float fmaxf(float a, float b);
double fmin(double a, double b);
double fmax(double a, double b);
int ilogbf(float x);
long lrint(double x);
long long llrint(double x);
long long llrintf(float x);
long long llround(double x);
long long llroundf(float x);
float nearbyintf(float x);
double nearbyint(double x);
float rintf(float x);
double rint(double x);
float roundf(float x);
float logf(float x);
double trunc(double x);
float acosf(float x);
float asinf(float x);
float atanf(float x);
float atan2f(float y, float x);
float tanf(float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float fmodf(float x, float y);
float cbrtf(float x);
float erff(float x);
float tgammaf(float x);
float hypotf(float x, float y);
double hypot(double x, double y);
float ldexpf(float x, int e);
float log1pf(float x);
double log1p(double x);
float expm1f(float x);
double expm1(double x);

double round(double x);
float expf(float x);
double exp(double x);
double log(double x);
double tan(double x);
double acos(double x);
double asin(double x);
double atan(double x);
double cosh(double x);
double sinh(double x);
double tanh(double x);
double fmod(double x, double y);
float fmodf(float x, float y);
double copysign(double x, double y);
float copysignf(float x, float y);
double log10(double x);
float log10f(float x);
float frexpf(float x, int *e);
double frexp(double x, int *e);
double ldexp(double x, int e);
float ldexpf(float x, int e);
double modf(double x, double *ip);
float modff(float x, float *ip);
double erf(double x);
double tgamma(double x);
double cbrt(double x);
float fmaf(float a, float b, float c);
double fma(double a, double b, double c);

float acoshf(float);
double acosh(double);
float asinhf(float);
double asinh(double);
float atanhf(float);
double atanh(double);
float erfcf(float);
double erfc(double);
float lgammf(float);
double lgamm(double);
float tgammaf(float);
double tgamma(double);
float remainderf(float, float);
double remainder(double, double);
float remquof(float, float, int *);
double remquo(double, double, int *);
float scalbnf(float, int);
double scalbn(double, int);
float scalblnf(float, long);
double scalbln(double, long);
float nextafterf(float, float);
double nextafter(double, double);
float nexttowardf(float, long double);
double nexttoward(double, long double);
float fdimf(float, float);
double fdim(double, double);
float nanf(const char*);
double nan(const char*);
float expm1f(float);
double expm1(double);
float log1pf(float);
double log1p(double);
float logbf(float);
double logb(double);
int ilogbf(float);
int ilogb(double);
float rintf(float);
double rint(double);
float roundf(float);
double round(double);
float truncf(float);
double trunc(double);
float copysignf(float, float);
double copysign(double, double);
float fmaxf(float, float);
double fmax(double, double);
float fminf(float, float);
double fmin(double, double);
float cbrtf(float);
double cbrt(double);
float hypotf(float, float);
double hypot(double, double);
long double acoshl(long double);
long double asinhl(long double);
long double atanhl(long double);
long double cbrtl(long double);
long double copysignl(long double, long double);
long double erfcl(long double);
long double erfl(long double);
long double exp2l(long double);
long double expm1l(long double);
long double fmaxl(long double, long double);
long double fminl(long double, long double);
long double hypotl(long double, long double);
long double lgammal(long double);
long double fdiml(long double, long double);
long double fmal(long double, long double, long double);
int ilogbl(long double);
long long llrintl(long double);
double lgamma(double);
float lgammaf(float);
long double log1pl(long double);
long double log2l(long double);
long double logbl(long double);
long double nanl(const char *);
long double nearbyintl(long double);
long double nextafterl(long double, long double);
long double nexttowardl(long double, long double);
long double remainderl(long double, long double);
long double remquol(long double, long double, int *);
long double rintl(long double);
long double roundl(long double);
long double truncl(long double);
long double tgammal(long double);
long double scalbnl(long double, int);
long double scalblnl(long double, long);
long lround(double);
long lroundf(float);
long long llroundl(long double);
long lrintl(long double);
long lroundl(long double);
#ifdef __cplusplus
}
#endif
#endif
