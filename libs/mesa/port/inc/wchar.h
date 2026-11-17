#ifndef MESA_PORT_WCHAR_H
#define MESA_PORT_WCHAR_H
#ifndef __cplusplus
#ifndef __WCHAR_TYPE__
#define __WCHAR_TYPE__ int
#endif
typedef __WCHAR_TYPE__ wchar_t;
#endif
#include <stddef.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned int wint_t;
typedef unsigned int wctype_t;
typedef unsigned int wctrans_t;
typedef struct { int dummy; } mbstate_t;
typedef struct { long __dummy; } wint_t_state;
#define WEOF ((wint_t)-1)
size_t wcslen(const wchar_t *s);
int btowc(int c);
int wctob(wint_t c);
wint_t fgetwc(void *f);
wint_t getwc(void *f);
wint_t fputwc(wint_t c, void *f);
wint_t putwc(wint_t c, void *f);
wint_t ungetwc(wint_t c, void *f);
int fwide(void *f, int mode);
wchar_t *fgetws(wchar_t *s, int n, void *f);
int fputws(const wchar_t *s, void *f);
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps);
size_t wcscspn(const wchar_t *s, const wchar_t *r);
size_t wcsspn(const wchar_t *s, const wchar_t *a);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcscpy(wchar_t *d, const wchar_t *s);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int swprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...);
int vswprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap);
int fwprintf(void *f, const wchar_t *fmt, ...);
int wprintf(const wchar_t *fmt, ...);
int fwscanf(void *f, const wchar_t *fmt, ...);
int swscanf(const wchar_t *s, const wchar_t *fmt, ...);
int vfwprintf(void *f, const wchar_t *fmt, va_list ap);
int vwprintf(const wchar_t *fmt, va_list ap);
wint_t towlower(wint_t c);
wint_t towupper(wint_t c);
wint_t towctrans(wint_t c, wctrans_t d);
int wcscoll(const wchar_t *a, const wchar_t *b);
size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *a);
wchar_t *wcsstr(const wchar_t *h, const wchar_t *n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n);
wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcscat(wchar_t *d, const wchar_t *s);
wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcstok(wchar_t *s, const wchar_t *d, wchar_t **p);
int wcwidth(wchar_t c);
int wcswidth(const wchar_t *s, size_t n);
int getwchar(void);
int putwchar(wchar_t c);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
int mbsinit(const mbstate_t *ps);
int vfwscanf(void *f, const wchar_t *fmt, va_list ap);
int vswscanf(const wchar_t *s, const wchar_t *fmt, va_list ap);
int vwscanf(const wchar_t *fmt, va_list ap);
int wscanf(const wchar_t *fmt, ...);
double wcstod(const wchar_t *s, wchar_t **e);
float wcstof(const wchar_t *s, wchar_t **e);
long double wcstold(const wchar_t *s, wchar_t **e);
long wcstol(const wchar_t *s, wchar_t **e, int b);
long long wcstoll(const wchar_t *s, wchar_t **e, int b);
unsigned long wcstoul(const wchar_t *s, wchar_t **e, int b);
unsigned long long wcstoull(const wchar_t *s, wchar_t **e, int b);
size_t wcsftime(wchar_t *s, size_t n, const wchar_t *fmt, const struct tm *tm);
#ifdef __cplusplus
}
#endif
#endif
