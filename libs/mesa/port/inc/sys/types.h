#ifndef MESA_PORT_SYS_TYPES_H
#define MESA_PORT_SYS_TYPES_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <stdint.h>
typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned int mode_t;
/* glibc's <bits/socket.h> expects __socklen_t from <sys/types.h>. */
typedef unsigned int __socklen_t;

#ifdef __cplusplus
}
#endif
#endif
