#ifndef MESA_PORT_UNISTD_H
#define MESA_PORT_UNISTD_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { unsigned long __val[16]; } sigset_t;
#include <stddef.h>
#include <stdint.h>
typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
long mesa_port_sysconf(int name);
int getpid(void);
int isatty(int fd);
int access(const char *p, int mode);
int close(int fd);
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
off_t lseek(int fd, off_t off, int whence);
int unlink(const char *p);
unsigned int getuid(void);
unsigned int geteuid(void);
unsigned int getgid(void);
unsigned int getegid(void);
#define _SC_PAGESIZE 30
#define sysconf(n) mesa_port_sysconf(n)
#define _exit(c) mesa_port_exit(c)
void mesa_port_exit(int code) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif
