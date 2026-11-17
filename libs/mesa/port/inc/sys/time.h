#ifndef MESA_PORT_SYS_TIME_H
#define MESA_PORT_SYS_TIME_H
#ifdef __cplusplus
extern "C" {
#endif
#include <time.h>
/* Use the same guard as glibc's <bits/types/struct_timeval.h> so only
 * one definition survives regardless of include order. */
#ifndef __timeval_defined
#define __timeval_defined 1
struct timeval { long tv_sec; long tv_usec; };
#endif
int gettimeofday(struct timeval *tv, void *tz);

#ifdef __cplusplus
}
#endif
#endif
