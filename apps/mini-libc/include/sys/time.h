/*
 * include/sys/time.h — mini-libc (ArtyomX64).
 *
 * struct timeval нужен tcc (таймеры опции -time).
 * gettimeofday() возвращает фиксированную метку сборки libc —
 * как time() (см. time/time.c).
 */

#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <time.h>

struct timeval {
        long tv_sec;            /* секунды */
        long tv_usec;           /* микросекунды */
};

struct timezone {
        int tz_minuteswest;
        int tz_dsttime;
};

extern int gettimeofday(struct timeval *tv, struct timezone *tz);

#endif /* _SYS_TIME_H */
