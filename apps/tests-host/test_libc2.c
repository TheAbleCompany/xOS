/*
 * tests-host/test_libc2.c — тест новых модулей mini-libc (хост-шим):
 * qsort, strtoll/strtoull, atexit, time/localtime, fseek/ftell/rewind,
 * mkstemp, getenv, malloc/realloc под нагрузкой.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
        if (cond) { printf("OK   %s\n", (msg)); } \
        else { printf("FAIL %s\n", (msg)); failures++; } \
} while (0)

static const char *atexit_log = "";

static void ah1(void) { atexit_log = "h1"; }
static void ah2(void) { atexit_log = "h1,h2"; }

static int cmp_int(const void *a, const void *b)
{
        return *(const int *)a - *(const int *)b;
}

static int cmp_str(const void *a, const void *b)
{
        return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void test_qsort(void)
{
        int a[] = { 5, -1, 3, 9, 0, 3, -7, 100, 2, 2, 8 };
        size_t n = sizeof(a) / sizeof(a[0]);
        int i, sorted = 1;

        qsort(a, n, sizeof(int), cmp_int);
        for (i = 1; i < (int)n; i++)
                if (a[i - 1] > a[i])
                        sorted = 0;

        CHECK(sorted && a[0] == -7 && a[n - 1] == 100, "qsort ints");

        {
                const char *s[] = { "pear", "apple", "fig", "banana" };
                qsort(s, 4, sizeof(char *), cmp_str);
                CHECK(strcmp(s[0], "apple") == 0 &&
                      strcmp(s[3], "pear") == 0, "qsort strings");
        }

        /* Крайние случаи: 0 и 1 элемент, одинаковые элементы. */
        qsort(NULL, 0, sizeof(int), cmp_int);
        {
                int one = 42;
                qsort(&one, 1, sizeof(int), cmp_int);
                CHECK(one == 42, "qsort n=1");
        }
}

static void test_strtox(void)
{
        char *end;

        CHECK(strtoll("  -42abc", &end, 10) == -42 &&
              end && *end == 'a', "strtoll base10 negative");
        CHECK(strtoll("0x1f", &end, 16) == 31, "strtoll base16");
        CHECK(strtoll("0x10", &end, 0) == 16, "strtoll base0 hex");
        CHECK(strtoll("010", &end, 0) == 8, "strtoll base0 octal");
        CHECK(strtoll("777", &end, 8) == 511, "strtoll base8");

        /* endptr при отсутствии цифр — на начало строки */
        {
                const char *p = "zzz";
                long long v = strtoll(p, &end, 10);

                CHECK(v == 0 && end == p, "strtoll no-digits endptr=nptr");
        }

        CHECK(strtoull("18446744073709551615", &end, 10) ==
              18446744073709551615ULL, "strtoull max");
        CHECK(strtoll("9223372036854775807", &end, 10) ==
              9223372036854775807LL, "strtoll max");
        CHECK(strtoll("-9223372036854775808", &end, 10) ==
              (-9223372036854775807LL - 1), "strtoll min");

        errno = 0;
        CHECK(strtoll("99999999999999999999999", &end, 10) == 9223372036854775807LL
              && errno == 34 /* ERANGE */, "strtoll ERANGE");
}

static void test_time(void)
{
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        char buf[64];
        size_t n;

        CHECK(tm != NULL, "localtime returns");
        CHECK(tm->tm_mday >= 1 && tm->tm_mday <= 31, "tm_mday range");
        CHECK(tm->tm_mon >= 0 && tm->tm_mon <= 11, "tm_mon range");
        CHECK(tm->tm_hour >= 0 && tm->tm_hour < 24, "tm_hour range");

        n = strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
        CHECK(n > 0 && strlen(buf) == 19, "strftime basic");

        /* roundtrip: mktime(гмт-поля) == исходный t */
        {
                time_t t2 = mktime(tm);

                CHECK(t2 == t, "mktime roundtrip");
        }
}

static void test_fseek(void)
{
        const char *fn = "/tmp/artyom_fseek_test.bin";
        FILE *f;
        char buf[16];
        long pos;

        f = fopen(fn, "w+b");
        CHECK(f != NULL, "fopen w+b");
        if (!f)
                return;

        fwrite("0123456789ABCDEF", 1, 16, f);
        fflush(f);

        CHECK(ftell(f) == 16, "ftell after write");

        rewind(f);
        CHECK(ftell(f) == 0, "ftell after rewind");
        CHECK(fgetc(f) == '0', "fgetc after rewind");
        CHECK(ftell(f) == 1, "ftell after 1 char read (buffered)");

        fseek(f, 5, SEEK_SET);
        CHECK(ftell(f) == 5, "ftell after fseek SET");
        CHECK(fgetc(f) == '5', "fgetc after fseek SET");

        fseek(f, 3, SEEK_CUR);
        pos = ftell(f);
        CHECK(pos == 9, "fseek CUR +3");
        CHECK(fgetc(f) == '9', "read at pos 9");

        fseek(f, -4, SEEK_END);
        pos = ftell(f);
        CHECK(pos == 12, "fseek END -4");
        CHECK(fgetc(f) == 'C', "read at pos 12");

        /* перемешанное чтение -> seek -> чтение (инвалидация буфера) */
        rewind(f);
        fread(buf, 1, 8, f);            /* буфер набит */
        fseek(f, 10, SEEK_SET);          /* seek мимо буфера */
        fread(buf, 1, 4, f);
        CHECK(memcmp(buf, "ABCD", 4) == 0, "read after seek w/ buffer");

        fclose(f);
        unlink(fn);
}

static void test_mkstemp(void)
{
        char tmpl[] = "/tmp/artyom_tcc_XXXXXX";
        int fd = mkstemp(tmpl);
        FILE *f;

        CHECK(fd >= 0, "mkstemp returns fd");
        CHECK(strlen(tmpl) == strlen("/tmp/artyom_tcc_XXXXXX") &&
              strcmp(tmpl + strlen(tmpl) - 6, "XXXXXX") != 0,
              "mkstemp expanded template");

        if (fd >= 0) {
                (void)write(fd, "data", 4);
                close(fd);
                f = fopen(tmpl, "r");
                CHECK(f && fgetc(f) == 'd', "mkstemp file readable");
                if (f)
                        fclose(f);
                unlink(tmpl);
        }
}

static void test_env(void)
{
        CHECK(getenv("definitely_not_set_xy") == NULL,
              "getenv returns NULL (no env in kernel)");
}

static void test_malloc_stress(void)
{
        /* Нагрузка как у tcc: много маленьких + большие realloc. */
        void *ptrs[256];
        int i;
        int ok = 1;
        char *big;

        for (i = 0; i < 256; i++) {
                ptrs[i] = malloc(1 + (i * 37) % 512);
                if (!ptrs[i])
                        ok = 0;
        }
        for (i = 0; i < 256; i += 2)
                free(ptrs[i]);
        for (i = 1; i < 256; i += 2)
                if (!ptrs[i])
                        ok = 0;
        for (i = 1; i < 256; i += 2)
                free(ptrs[i]);
        CHECK(ok, "malloc/free 256 blocks");

        big = malloc(4096);
        CHECK(big != NULL, "malloc 4K");
        big = realloc(big, 1 << 20);    /* 1 МиБ подряд */
        CHECK(big != NULL, "realloc to 1MiB");
        memset(big, 0xAB, 1 << 20);
        CHECK(big[0] == (char)0xAB && big[(1 << 20) - 1] == (char)0xAB,
              "1MiB block writable");
        free(big);

        {
                void *p = calloc(64, 64);

                CHECK(p != NULL, "calloc 4K");
                ok = 1;
                {
                        unsigned char *c = (unsigned char *)p;
                        for (i = 0; i < 4096; i++)
                                if (c[i] != 0)
                                        ok = 0;
                }
                free(p);
                CHECK(ok, "calloc zeroed");
        }
}

int main(void)
{
        atexit(ah1);
        atexit(ah2);

        test_qsort();
        test_strtox();
        test_time();
        test_fseek();
        test_mkstemp();
        test_env();
        test_malloc_stress();

        if (failures == 0) {
                printf("test_libc2: ALL PASSED\n");
                return 0;       /* здесь сработают atexit h2 -> h1 */
        }
        printf("test_libc2: %d FAILURES\n", failures);
        return 1;
}
