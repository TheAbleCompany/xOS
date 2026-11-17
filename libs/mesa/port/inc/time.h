#ifndef MESA_PORT_TIME_H
#define MESA_PORT_TIME_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stddef.h>
typedef long time_t;
typedef long clock_t;
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCKS_PER_SEC 1000000L

/* Same as glibc's clockid_t (int); duplicate identical typedef is valid. */
typedef int clockid_t;
time_t time(time_t *t);
int clock_gettime(int clk, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
clock_t clock(void);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);
double difftime(time_t a, time_t b);
time_t mktime(struct tm *tm);
size_t strftime(char *s, size_t n, const char *fmt, const struct tm *tm);

#ifdef __cplusplus
}
#endif
#endif
