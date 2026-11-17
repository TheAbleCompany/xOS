#ifndef MESA_PORT_STDIO_H
#define MESA_PORT_STDIO_H
typedef long fpos_t;
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <stdarg.h>
typedef struct mesa_port_file FILE;
extern FILE *stdout;
extern FILE *stderr;
extern FILE *stdin;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 8192
#define FILENAME_MAX 4096
#define L_tmpnam 256
#define TMP_MAX 238328
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
int fclose(FILE *f);
int fflush(FILE *f);
FILE *fopen(const char *p, const char *mode);
FILE *freopen(const char *p, const char *mode, FILE *f);
FILE *fdopen(int fd, const char *mode);
FILE *tmpfile(void);
char *tmpnam(char *s);
void setbuf(FILE *f, char *buf);
int setvbuf(FILE *f, char *buf, int mode, size_t n);
int fprintf(FILE *f, const char *fmt, ...);
int printf(const char *fmt, ...);
int sprintf(char *s, const char *fmt, ...);
int snprintf(char *s, size_t n, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);
int vsprintf(char *s, const char *fmt, va_list ap);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int vasprintf(char **out, const char *fmt, va_list ap);
int asprintf(char **out, const char *fmt, ...);
int fscanf(FILE *f, const char *fmt, ...);
int scanf(const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);
int vfscanf(FILE *f, const char *fmt, va_list ap);
int vscanf(const char *fmt, va_list ap);
int vsscanf(const char *s, const char *fmt, va_list ap);
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int putchar(int c);
int puts(const char *s);
int fputs(const char *s, FILE *f);
char *fgets(char *s, int n, FILE *f);
char *gets(char *s);
size_t fread(void *p, size_t s, size_t n, FILE *f);
size_t fwrite(const void *p, size_t s, size_t n, FILE *f);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int fgetpos(FILE *f, fpos_t *pos);
int fsetpos(FILE *f, const fpos_t *pos);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
void perror(const char *s);
int remove(const char *p);
int rename(const char *a, const char *b);
int fileno(FILE *f);
int ungetc(int c, FILE *f);
FILE *popen(const char *cmd, const char *mode);
int pclose(FILE *f);
#ifdef __cplusplus
}
#endif
#endif
