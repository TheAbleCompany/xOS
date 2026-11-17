/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef __STDIO_H__
#define __STDIO_H__     1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>

#define EOF (-1)


#define BUFSIZ 1024

typedef unsigned long size_t;

// Флаги для FILE
#define _IO_READ      0x0001
#define _IO_WRITE     0x0002
#define _IO_EOF       0x0010
#define _IO_ERR       0x0020
#define _IO_LINE_BUF  0x0040
#define _IO_UNBUFFERED 0x0080

// Структура FILE
typedef struct FILE {
    int _fileno;
    unsigned char *_buf;
    unsigned char *_p;
    int _r;
    int _w;
    int _flags;
    size_t _bufsize;
} FILE;

// Внешние переменные
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

// Функции ввода-вывода
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
int fputc(int c, FILE *stream);
int putc(int c, FILE *stream);
int putchar(int c);
int fflush(FILE *stream);
int ungetc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);
int puts(const char *s);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fileno(FILE *stream);
char *fgets(char *s, int size, FILE *stream);
FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *stream);

/* Позиционирование (требует lseek в ядре, номер 8). */
typedef long fpos_t;
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int fgetpos(FILE *stream, fpos_t *pos);
int fsetpos(FILE *stream, const fpos_t *pos);
int fprintf(FILE *stream, const char *format, ...);
int printf(const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list ap);
int vprintf(const char *format, va_list ap);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int vsprintf(char *str, const char *format, va_list ap);
int fscanf(FILE *stream, const char *format, ...);
int scanf(const char *format, ...);
int sscanf(const char *str, const char *format, ...);
int remove(const char *pathname);
int rename(const char *oldpath, const char *newpath);
int system(const char *command);

// Функции работы со строками
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcat(char *dest, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strdup(const char *s);
double strtod(const char *nptr, char **endptr);
double atof(const char *nptr);

// Функции памяти
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

// Функции для проверки символов
int isspace(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isupper(int c);
int islower(int c);
int toupper(int c);
int tolower(int c);

#endif /* _STDIO_H */
#ifdef __cplusplus
}
#endif

#endif
