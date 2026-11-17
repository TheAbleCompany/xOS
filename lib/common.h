#ifndef COMMON_H
#define COMMON_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef unsigned long long  u64int;
typedef          long long  s64int;
typedef unsigned int        u32int;
typedef          int        s32int;
typedef unsigned short      u16int;
typedef          short      s16int;
typedef unsigned char       u8int;
typedef          char       s8int;

void outb(u16int port, u8int value);
u8int inb(u16int port);
u16int inw(u16int port);

void outl(u16int port, u32int value);
u32int inl(u16int port);

void outw(u16int port, u16int value);

void *memcpy(void *dest, const void *src, size_t len);
void *memset(void *dest, int val, size_t len);
int strcmp(const char *str1, const char *str2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dest, const char *src);
char* strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
size_t strlen(const char *src);
char *strtok(char *srcString, char *delim);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
void *memchr(const void *s, int c, size_t n);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t cap, const char *fmt, ...);
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list args);
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
int abs(int x);
int rename(const char *from, const char *to);

int printf(const char *fmt, ...);
#define PANIC(msg) panic(msg, __FILE__, __LINE__);
#define ASSERT(b) ((b) ? (void)0 : panic_assert(__FILE__, __LINE__, #b))

extern void panic(const char *message, const char *file, u32int line);
extern void panic_assert(const char *file, u32int line, const char *desc);

/*
 * Debug serial output (implemented in lib/syscalls.c).
 *
 * Declared here so every kernel subsystem (fs/open.c, fs/exec.c,
 * the axshell split files, ...) can use them without each carrying
 * its own extern prototype.
 */
void debug_putc(char c);
void debug_puts(const char *s);
void debug_putnum(uint64_t n);
/* Копия всего debug-вывода (для storage.log); len -- число байт. */
const char *debug_log_data(uint32_t *len);

/* COM1 serial (lib/common.c) */
void serial_putc_ax(char c);
void serial_puts_ax(const char *s);

#endif
