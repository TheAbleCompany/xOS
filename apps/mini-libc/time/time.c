/*
 * time/time.c — время для mini-libc (ArtyomX64).
 *
 * У ядра ArtyomX64 нет часового реального времени с эпохой (syscall
 * AX 163 выдаёт календарь графической подсистемы и не гарантируется
 * для консольных задач), поэтому time() возвращает фиксированную
 * метку сборки libc (TIME_EPOCH, задаётся при сборке через -D).
 *
 * localtime/gmtime — полный корректный пересчёт civil <-> epoch
 * (алгоритм Howard Hinnant), без поправок на часовой пояс и DST:
 * ОС не знает таймзону, поэтому localtime == gmtime (UTC).
 */

#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stddef.h>
#include <errno.h>
#include <internal/syscall.h>

#ifndef TIME_EPOCH
#define TIME_EPOCH 0            /* 1970-01-01 00:00:00 UTC */
#endif

/*
 * AX RTC-часы ядра ArtyomX64 (syscall 163): ax_syscall_time()
 * читает CMOS RTC и возвращает гражданский календарь:
 *   struct { int32_t year, month, day, hour, minute, second; }
 * Используется как резервный источник CLOCK_REALTIME, когда
 * ядро ещё не реализовало clock_gettime(228)/gettimeofday(96),
 * а на хост-шиме (Linux) эти syscall'ы работают напрямую.
 */
typedef struct {
        int year, month, day;
        int hour, minute, second;
} ax_rtc_time_t;

#define AX_SYSCALL_TIME 163

static const char days_in_month[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/* Дней с 1970-01-01 до указанной даты (пролептический Григорианский). */
static long days_from_civil(long y, unsigned m, unsigned d)
{
        long era, yoe, doe;
        unsigned mp;

        y -= (m <= 2);
        mp = (m + 9) % 12;
        era = (y >= 0 ? y : y - 399) / 400;
        yoe = y - era * 400;                    /* [0, 399] */
        doe = yoe * 365 + yoe / 4 - yoe / 100 +
              (mp * 153 + 2) / 5 + d - 1;       /* [0, 146096] */
        return era * 146097 + doe - 719468;     /* с 1970-01-01 */
}

static void civil_from_days(long z, long *py, int *pm, int *pd)
{
        long era, yoe, y, doy, mp;

        z += 719468;
        era = (z >= 0 ? z : z - 146096) / 146097;
        doy = z - era * 146097;                 /* [0, 146096] */
        yoe = (doy - doy / 1460 + doy / 36524 - doy / 146096) / 365;
        y = yoe + era * 400;
        doy -= yoe * 365 + yoe / 4 - yoe / 100;
        mp = (5 * doy + 2) / 153;               /* [0, 11] */
        *pd = (int)(doy - (mp * 153 + 2) / 5 + 1);
        *pm = (int)(mp < 10 ? mp + 3 : mp - 9);
        *py = y + (*pm <= 2);
}

/*
 * Лучшее доступное время: цепочка источников.
 *   1) syscall gettimeofday (96)   — хост-шим Linux / будущее ядро
 *   2) syscall AX time (163)       — CMOS RTC ядра ArtyomX64
 *   3) TIME_EPOCH                  — метка сборки libc
 */
static long best_time_now(long *usec_out)
{
        struct timeval tv;
        ax_rtc_time_t ax;
        long days;

        if (usec_out)
                *usec_out = 0;

        /* 1) gettimeofday: настоящий источник времени (хост-шим). */
        if (syscall(__NR_gettimeofday, (long)&tv, 0, 0, 0, 0, 0) == 0) {
                if (usec_out)
                        *usec_out = tv.tv_usec;
                return tv.tv_sec;
        }

        /* 2) RTC ядра ArtyomX64 (гражданский календарь -> epoch). */
        if (syscall(AX_SYSCALL_TIME, (long)&ax, 0, 0, 0, 0, 0) == 0 &&
            ax.year > 1970) {
                days = days_from_civil(ax.year, (unsigned)ax.month,
                                       (unsigned)ax.day);
                return days * 86400L +
                       ax.hour * 3600L +
                       ax.minute * 60L +
                       ax.second;
        }

        /* 3) Фиксированная метка сборки. */
        return (long)TIME_EPOCH;
}

time_t time(time_t *tloc)
{
        time_t t = (time_t)best_time_now(NULL);

        if (tloc)
                *tloc = t;
        return t;
}

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
        long usec = 0;

        if (tv) {
                tv->tv_sec = best_time_now(&usec);
                tv->tv_usec = usec;
        }
        if (tz) {
                tz->tz_minuteswest = 0;
                tz->tz_dsttime = 0;
        }
        return 0;
}

/*
 * clock_gettime — требуется run.c (llama2.c, time_in_ms()).
 * Порядок:
 *   1) syscall clock_gettime (228) — нативный (Linux-шим / будущее ядро);
 *   2) gettimeofday (96) + пересчёт;
 *   3) RTC ядра (163);
 *   4) TIME_EPOCH.
 * CLOCK_MONOTONIC на ArtyomX64 отдельного источника пока не имеет и
 * даёт те же значения, что и REALTIME (апгрейд — uptime-счётчик ядра).
 */
int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
        long sec;

        if (tp == NULL) {
                errno = EINVAL;
                return -1;
        }
        (void)clk_id;   /* все часы пока разделяют один источник */

        /* 1) нативный clock_gettime */
        if (syscall(__NR_clock_gettime, (long)clk_id, (long)tp,
                    0, 0, 0, 0) == 0)
                return 0;

        /* 2-4) резервная цепочка */
        sec = best_time_now(NULL);
        tp->tv_sec = sec;
        tp->tv_nsec = 0;
        return 0;
}

int clock_getres(clockid_t clk_id, struct timespec *res)
{
        if (res == NULL) {
                errno = EINVAL;
                return -1;
        }
        (void)clk_id;

        /* RTC ядра даёт секунды; на хосте точность выше. */
        if (syscall(__NR_clock_gettime, (long)clk_id, (long)res,
                    0, 0, 0, 0) == 0)
                return 0;
        res->tv_sec = 1;
        res->tv_nsec = 0;
        return 0;
}

static struct tm *to_tm(const time_t *timep)
{
        static struct tm tm;
        time_t t = *timep;
        long days = (long)(t / 86400);
        long rem = (long)(t % 86400);
        long y;
        int m, d;
        long yday_idx;
        long dim;

        if (rem < 0) {
                rem += 86400;
                days--;
        }

        tm.tm_hour = (int)(rem / 3600);
        tm.tm_min  = (int)((rem / 60) % 60);
        tm.tm_sec  = (int)(rem % 60);

        civil_from_days(days, &y, &m, &d);

        tm.tm_year = (int)(y - 1900);
        tm.tm_mon  = m - 1;
        tm.tm_mday = d;

        tm.tm_wday = (int)((days + 4) % 7);     /* 1970-01-01 — четверг */
        if (tm.tm_wday < 0)
                tm.tm_wday += 7;

        /* День года. */
        tm.tm_yday = 0;
        for (yday_idx = 0; yday_idx < tm.tm_mon; yday_idx++) {
                dim = days_in_month[yday_idx];
                if (yday_idx == 1 &&
                    ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
                        dim = 29;
                tm.tm_yday += (int)dim;
        }
        tm.tm_yday += d - 1;

        tm.tm_isdst = 0;
        return &tm;
}

struct tm *gmtime(const time_t *timep)
{
        if (timep == NULL)
                return NULL;
        return to_tm(timep);
}

struct tm *localtime(const time_t *timep)
{
        /* Таймзон нет: localtime == UTC. */
        return gmtime(timep);
}

time_t mktime(struct tm *tm)
{
        long days;
        time_t t;
        struct tm *chk;

        if (tm == NULL)
                return (time_t)-1;

        days = days_from_civil((long)tm->tm_year + 1900,
                               (unsigned)(tm->tm_mon + 1),
                               (unsigned)tm->tm_mday);
        t = (time_t)days * 86400 +
            tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;

        /* Нормализуем поля (wday/yday), как ожидает пользователь. */
        chk = to_tm(&t);
        (void)chk;
        return t;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm)
{
        static const char wday_name[7][4] = {
                "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
        };
        static const char mon_name[12][4] = {
                "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };
        size_t n = 0;
        char buf[64];
        const char *p;

        if (s == NULL || format == NULL || tm == NULL)
                return 0;

        for (p = format; *p; p++) {
                const char *rep = NULL;

                if (*p != '%') {
                        if (n + 1 >= max)
                                return 0;
                        s[n++] = *p;
                        continue;
                }

                p++;
                if (*p == 0)
                        break;

                buf[0] = 0;
                switch (*p) {
                case 'a':
                        rep = wday_name[tm->tm_wday % 7];
                        break;
                case 'b':
                case 'h':
                        rep = mon_name[tm->tm_mon % 12];
                        break;
                case 'Y':
                        snprintf(buf, sizeof(buf), "%d", tm->tm_year + 1900);
                        rep = buf;
                        break;
                case 'm':
                        snprintf(buf, sizeof(buf), "%02d", tm->tm_mon + 1);
                        rep = buf;
                        break;
                case 'd':
                        snprintf(buf, sizeof(buf), "%02d", tm->tm_mday);
                        rep = buf;
                        break;
                case 'H':
                        snprintf(buf, sizeof(buf), "%02d", tm->tm_hour);
                        rep = buf;
                        break;
                case 'M':
                        snprintf(buf, sizeof(buf), "%02d", tm->tm_min);
                        rep = buf;
                        break;
                case 'S':
                        snprintf(buf, sizeof(buf), "%02d", tm->tm_sec);
                        rep = buf;
                        break;
                case '%':
                        rep = "%";
                        break;
                default:
                        /* Неподдержанные спецификаторы копируем как есть. */
                        buf[0] = '%';
                        buf[1] = *p;
                        buf[2] = 0;
                        rep = buf;
                        break;
                }

                if (rep == NULL)
                        rep = "";

                while (*rep) {
                        if (n + 1 >= max)
                                return 0;
                        s[n++] = *rep++;
                }
        }

        if (n + 1 > max)
                return 0;
        s[n] = 0;
        return n;
}
