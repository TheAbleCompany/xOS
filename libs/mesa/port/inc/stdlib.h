#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef MESA_PORT_STDLIB_H
#define MESA_PORT_STDLIB_H
#ifndef __cplusplus
#ifndef __WCHAR_TYPE__
#define __WCHAR_TYPE__ int
#endif
typedef __WCHAR_TYPE__ wchar_t;
#endif
#include "_mesa_port_cfg.h"
#include <alloca.h>
typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;
div_t div(int n, int d);
ldiv_t ldiv(long n, long d);
lldiv_t lldiv(long long n, long long d);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
double strtod(const char *s, char **end);
void qsort_r(void *b, size_t n, size_t sz,
             int (*cmp)(const void *, const void *, void *), void *arg);
void *malloc(size_t n);
void *calloc(size_t a, size_t b);
void *realloc(void *p, size_t n);
void free(void *p);
void abort(void) __attribute__((noreturn));
void exit(int c) __attribute__((noreturn));
int atexit(void (*function)(void));
int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
double atof(const char *s);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
int abs(int x);
long labs(long x);
long long llabs(long long x);
char *getenv(const char *name);
int setenv(const char *n, const char *v, int overwrite);
int unsetenv(const char *n);
int rand(void);
void srand(unsigned int seed);
int system(const char *cmd);
int mkstemp(char *tmpl);
char *realpath(const char *p, char *resolved);
int posix_memalign(void **out, size_t align, size_t size);
void *aligned_alloc(size_t align, size_t size);
float strtof(const char *s, char **end);
void *bsearch(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *));
void qsort(void *base, size_t n, size_t sz,
           int (*cmp)(const void *, const void *));

void quick_exit(int code) __attribute__((noreturn));
int at_quick_exit(void (*fn)(void));
long double strtold(const char *s, char **end);
void _Exit(int code) __attribute__((noreturn));
int mblen(const char *s, size_t n);
int mbtowc(wchar_t *pwc, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);
#endif
#ifdef __cplusplus
}
#endif
