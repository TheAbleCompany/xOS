/*
 * include/time.h — mini-libc (ArtyomX64).
 */

#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC  100

struct tm {
        int tm_sec;     /* секунды   0-59 */
        int tm_min;     /* минуты    0-59 */
        int tm_hour;    /* часы      0-23 */
        int tm_mday;    /* день      1-31 */
        int tm_mon;     /* месяц     0-11 */
        int tm_year;    /* год - 1900 */
        int tm_wday;    /* день недели 0-6 (вс=0) */
        int tm_yday;    /* день года 0-365 */
        int tm_isdst;   /* летнее время — всегда 0 */
};

extern time_t time(time_t *tloc);
extern struct tm *gmtime(const time_t *timep);
extern struct tm *localtime(const time_t *timep);
extern time_t mktime(struct tm *tm);
extern size_t strftime(char *s, size_t max, const char *format,
                       const struct tm *tm);

#ifndef _TIMESPEC_DEFINED
struct timespec {
        long tv_sec;            /* секунды */
        long tv_nsec;           /* наносекунды */
};
#define _TIMESPEC_DEFINED
#endif

int nanosleep(const struct timespec *t1, struct timespec *t2);
unsigned int sleep(unsigned int s);
unsigned int alarm(unsigned int seconds);

#endif /* _TIME_H */
