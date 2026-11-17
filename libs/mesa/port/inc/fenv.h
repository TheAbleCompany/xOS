#ifndef MESA_PORT_FENV_H
#define MESA_PORT_FENV_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { unsigned short __x; } fenv_t;
typedef unsigned short fexcept_t;
#define FE_INVALID 0x01
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW 0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT 0x20
#define FE_ALL_EXCEPT 0x3D
#define FE_TONEAREST 0
#define FE_DOWNWARD 0x400
#define FE_UPWARD 0x800
#define FE_TOWARDZERO 0xC00
int feclearexcept(int e);
int fegetexceptflag(fexcept_t *f, int e);
int feraiseexcept(int e);
int fesetexceptflag(const fexcept_t *f, int e);
int fetestexcept(int e);
int fegetround(void);
int fesetround(int r);
int fegetenv(fenv_t *e);
int feholdexcept(fenv_t *e);
int fesetenv(const fenv_t *e);
int feupdateenv(const fenv_t *e);
#ifdef __cplusplus
}
#endif
#endif
