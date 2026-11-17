#ifndef MESA_PORT_STRING_H
#define MESA_PORT_STRING_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strncat(char *d, const char *s, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strtok(char *s, char *d);
char *strtok_r(char *s, const char *d, char **save);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strerror(int e);
size_t strspn(const char *s, const char *a);
size_t strcspn(const char *s, const char *r);
char *strpbrk(const char *s, const char *a);

int strcoll(const char *a, const char *b);
size_t strxfrm(char *d, const char *s, size_t n);
int strerror_r(int err, char *buf, size_t n);
char *strerror(int err);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
char *strtok_r(char *s, const char *delim, char **save);
char *strndup(const char *s, size_t n);
size_t strnlen(const char *s, size_t n);

#ifdef __cplusplus
}
#endif
#endif
