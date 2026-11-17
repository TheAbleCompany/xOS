#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define HAVE_PTHREAD 1
#include "c11/threads.h"
#include "util/u_thread.h"
#include "util/memstream.h"
#include "util/u_qsort.h"
#include <unistd.h>

static void zero(void *p, size_t n)
{
    unsigned char *b = (unsigned char *)p;
    for (size_t i = 0; i < n; i++)
        b[i] = 0;
}

void call_once(once_flag *flag, void (*func)(void))
{
    if (flag) {
        int *raw = (int *)flag;
        if (!*raw) {
            *raw = 1;
            func();
        }
    }
}

int cnd_init(cnd_t *c)
{
    if (c) zero(c, sizeof(*c));
    return 0;
}

void cnd_destroy(cnd_t *c)
{
    (void)c;
}

int cnd_signal(cnd_t *c)
{
    (void)c;
    return 0;
}

int cnd_broadcast(cnd_t *c)
{
    (void)c;
    return 0;
}

int cnd_wait(cnd_t *c, mtx_t *m)
{
    (void)c;
    (void)m;
    return 0;
}

int mtx_init(mtx_t *m, int type)
{
    (void)type;
    if (m) zero(m, sizeof(*m));
    return 0;
}

void mtx_destroy(mtx_t *m)
{
    (void)m;
}

int mtx_lock(mtx_t *m)
{
    (void)m;
    return 0;
}

int mtx_trylock(mtx_t *m)
{
    (void)m;
    return 0;
}

int mtx_unlock(mtx_t *m)
{
    (void)m;
    return 0;
}

thrd_t thrd_current(void)
{
    return 0;
}

int thrd_equal(thrd_t a, thrd_t b)
{
    return a == b;
}

int thrd_join(thrd_t t, int *result)
{
    (void)t;

    if (result)
        *result = 0;

    return 0;
}

void util_barrier_init(util_barrier *barrier, unsigned count)
{
    (void)count;

    if (barrier)
        zero(barrier, sizeof(*barrier));
}

void util_barrier_destroy(util_barrier *barrier)
{
    (void)barrier;
}

bool util_barrier_wait(util_barrier *barrier)
{
    (void)barrier;
    return true;
}

int u_thread_create(thrd_t *thrd, int (*routine)(void *), void *param)
{
    (void)routine;
    (void)param;

    if (thrd)
        *thrd = 0;

    return 1;
}

bool u_memstream_open(struct u_memstream *mem, char **bufp, size_t *sizep)
{
    (void)mem;
    (void)bufp;
    (void)sizep;
    return false;
}

void u_memstream_close(struct u_memstream *mem)
{
    (void)mem;
}

int u_memstream_flush(struct u_memstream *mem)
{
    (void)mem;
    return 1;
}

static int (*tls_compar)(const void *, const void *, void *);
static void *tls_arg;

static int tls_adapt(const void *a, const void *b)
{
    return tls_compar(a, b, tls_arg);
}

void util_tls_qsort_r(void *base, size_t nmemb, size_t size,
                      int (*compar)(const void *, const void *, void *),
                      void *arg)
{
    int (*saved_compar)(const void *, const void *, void *) = tls_compar;
    void *saved_arg = tls_arg;

    tls_compar = compar;
    tls_arg = arg;

    qsort(base, nmemb, size, tls_adapt);

    tls_compar = saved_compar;
    tls_arg = saved_arg;
}

int atexit(void (*fn)(void))
{
    (void)fn;
    return 0;
}

int at_quick_exit(void (*fn)(void))
{
    (void)fn;
    return 0;
}

void quick_exit(int code)
{
    exit(code);
}

void _Exit(int code)
{
    exit(code);
}

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)fn;
    (void)arg;
    (void)dso;
    return 0;
}

int __cxa_guard_acquire(uint64_t *guard)
{
    if (*guard)
        return 0;

    *guard = 1;
    return 1;
}

void __cxa_guard_release(uint64_t *guard)
{
    (void)guard;
}

void __cxa_pure_virtual(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

void *__dso_handle = 0;
