/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef __STDLIB_H__
#define __STDLIB_H__    1

#ifdef __cplusplus
extern "C" {
#endif

#include <internal/types.h>
#include <internal/essentials.h>

#define EXIT_SUCCESS    0
#define EXIT_FAILURE    1

#define RAND_MAX        32767

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void *reallocarray(void *ptr, size_t nmemb, size_t size);
long exit(long exit_code);
void abort(void);
void _exit(int exit_code);

int abs(int j);
long labs(long j);

int atoi(const char *nptr);
long atol(const char *nptr);
double atof(const char *nptr);
double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
unsigned long long __isoc23_strtoull(const char *str, char **endptr, int base);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

int atexit(void (*function)(void));
int mkstemp(char *template);

char *getenv(const char *name);
char *realpath(const char *path, char *resolved_path);
#define PATH_MAX 4096
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);
int clearenv(void);
int system(const char *command);

#ifdef __cplusplus
}
#endif

#endif
