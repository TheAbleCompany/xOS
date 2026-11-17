/*
 * ============================================================
 * bench.c -- бенчмарк ArtyomXOS (сравнение с Linux)
 * ============================================================
 *
 * Измеряет то, в чём микроядерная ОС с лёгким userspace
 * может честно соперничать с Linux:
 *
 *   1. Сисколл-латентность (getpid, чистый int $0x80,
 *      без vDSO -- как на Linux без vDSO).
 *   2. IPC ping-pong между двумя задачами через именованные
 *      порты (аналог pipe ping-pong на Linux -- там это
 *      2 системных вызова + 2 переключения контекста).
 *   3. Запуск нового процесса (exec + ожидание смерти:
 *      полный цикл fork+exec+exit по стоимости).
 *   4. Пропускная способность памяти (SHM-буферы, memcpy).
 *
 * Короткие участки измеряются через RDTSC; syscall 196 оставлен
 * только для таймаутов IPC. RDTSC выдаёт такты TSC, не наносекунды.
 *
 * Запуск: bench        -- все тесты (из xsh: bench)
 *         bench --pong <имя-порта-мастера> <count>  -- воркер IPC
 *         bench --fast                              -- воркер exec
 * ============================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ============================================================
 * Сисколлы ArtyomX64 (int $0x80, см. lib/syscalls.c)
 * ============================================================ */

#define SYS_EXIT     1
#define SYS_EXEC     11
#define SYS_YIELD    151
#define SYS_ALIVE    167
#define SYS_PCREATE  178
#define SYS_PLOOKUP  179
#define SYS_PSEND    180
#define SYS_PRECV    181
#define SYS_GETPID   182
#define SYS_SHMALLOC 183
#define SYS_SHMFREE  184
#define SYS_TICKS    196

static long sc3(long n, long a1, long a2, long a3)
{
    long ret;

    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3)
        : "memory");

    return ret;
}

/* TSC даёт значительно более точное измерение коротких участков,
 * чем системные миллисекундные часы. Сравнивать cycle/op можно
 * только на одном и том же физическом CPU и при одинаковом режиме
 * энергопотребления. */
static uint64_t tsc_read(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence\n\trdtsc\n\tlfence"
        : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t ms_clock(void)
{
    return (uint64_t)sc3(SYS_TICKS, 0, 0, 0);
}

static uint64_t median7(uint64_t v[7])
{
    int i, j;
    for (i = 1; i < 7; ++i) {
        uint64_t x = v[i];
        for (j = i; j > 0 && v[j - 1] > x; --j)
            v[j] = v[j - 1];
        v[j] = x;
    }
    return v[3];
}

static void sys_yield(void)
{
    sc3(SYS_YIELD, 0, 0, 0);
}

static void sys_exit(int code)
{
    sc3(SYS_EXIT, code, 0, 0);

    for (;;)
        ;
}

/* ============================================================
 * Конверт сообщений (тот же layout, что lib/axport.h; ядро
 * копирует его целиком при PORT_SEND/PORT_RECV).
 * ============================================================ */

#define AX_MSG_DATA_MAX 980

typedef struct {
    uint32_t type;
    uint32_t win;
    uint32_t from;
    uint32_t a, b;
    uint32_t x, y;
    uint32_t len;
    uint8_t  data[AX_MSG_DATA_MAX];
} ax_msg_t;

#define MSG_READY   1u      /* воркер -> мастер: from = порт воркера */
#define MSG_PING    2u      /* мастер -> воркер                      */
#define MSG_PONG    3u      /* воркер -> мастер                      */

static void port_send_wait(int port, ax_msg_t *m)
{
    for (;;)
        if (sc3(SYS_PSEND, port, (long)m, 0) == 0)
            return;
        else
            sys_yield();            /* очередь полна */
}

/* recv: покрутиться без yield (быстрый путь), затем
 * перейти на yield-ожидание, чтобы гарантированно не
 * монополизировать CPU. */
static int port_recv_wait(int port, ax_msg_t *m)
{
    int i;

    for (i = 0; i < 2000; i++)
        if (sc3(SYS_PRECV, port, (long)m, 0) == 1)
            return 1;

    for (;;)
        if (sc3(SYS_PRECV, port, (long)m, 0) == 1)
            return 1;
        else
            sys_yield();
}

/* ============================================================
 * Режим воркера
 * ============================================================ */

static void worker_pong(const char *master_name, int count)
{
    ax_msg_t m;
    int mport, wport, i;

    memset(&m, 0, sizeof(m));

    mport = (int)sc3(SYS_PLOOKUP, (long)master_name, 0, 0);
    wport = (int)sc3(SYS_PCREATE, (long)"bench-w", 0, 0);

    if (mport < 0 || wport < 0)
        sys_exit(1);

    /* Рука помощи мастеру: READY + свой порт. */
    m.type = MSG_READY;
    m.from = (uint32_t)wport;
    port_send_wait(mport, &m);

    for (i = 0; i < count; i++) {

        port_recv_wait(wport, &m);

        if (m.type != MSG_PING)
            break;

        m.type = MSG_PONG;
        port_send_wait(mport, &m);
    }

    sys_exit(0);
}

/* ============================================================
 * Тесты (мастер)
 * ============================================================ */

/* --- 1. Сисколл-латентность --- */
static void test_syscall(void)
{
    const long N = 200000;
    uint64_t samples[7];
    volatile long pid_sink = 0;
    int r;

    /* Семь независимых серий; публикуем медиану, а не один случайный
     * замер. В цикле измеряется реальный int $0x80 + обработчик ядра. */
    for (r = 0; r < 7; ++r) {
        uint64_t t0 = tsc_read();
        long i;
        for (i = 0; i < N; ++i)
            pid_sink += sc3(SYS_GETPID, 0, 0, 0);
        samples[r] = tsc_read() - t0;
    }

    {
        uint64_t med = median7(samples);
        printf("1) syscall getpid (int 0x80):\n");
        printf("   ops=%ld median=%lu cycles/op\n",
               N, (unsigned long)(med / N));
        printf("   samples(cycles): %lu %lu %lu %lu %lu %lu %lu\n",
               (unsigned long)samples[0], (unsigned long)samples[1],
               (unsigned long)samples[2], (unsigned long)samples[3],
               (unsigned long)samples[4], (unsigned long)samples[5],
               (unsigned long)samples[6]);
    }
    (void)pid_sink;
}

/* --- 2. IPC ping-pong --- */
static void test_ipc(void)
{
    const int COUNT = 2000;
    const char *wname = "bench";
    char *wargv[4];
    ax_msg_t m;
    uint64_t t0, t1;
    int mport, wport = -1, count = COUNT;
    int i, pid, first, ready = 0;

    memset(&m, 0, sizeof(m));

    mport = (int)sc3(SYS_PCREATE, (long)"bench-m", 0, 0);
    if (mport < 0) {
        printf("2) IPC ping-pong: FAILED (port_create)\n");
        return;
    }

    wargv[0] = (char *)wname;
    wargv[1] = "--pong";
    wargv[2] = "bench-m";
    wargv[3] = "2000";

    pid = (int)sc3(SYS_EXEC, (long)wname, 4, (long)wargv);
    if (pid < 0) {
        printf("2) IPC ping-pong: FAILED (exec)\n");
        return;
    }

    /* Ждём READY (с таймаутом по часам ядра). */
    {
        uint64_t deadline = ms_clock() + 3000;

        for (;;) {
            if (sc3(SYS_PRECV, mport, (long)&m, 0) == 1) {
                if (m.type == MSG_READY) {
                    wport = (int)m.from;
                    ready = 1;
                    break;
                }
                continue;   /* чужое сообщение -- игнорируем */
            }

            if (ms_clock() > deadline)
                break;

            sys_yield();
        }
    }

    if (!ready || wport < 0) {
        printf("2) IPC ping-pong: FAILED (no READY)\n");
        return;
    }

    first = 1;
    t0 = ms_clock();

    for (i = 0; i < count; i++) {

        m.type = MSG_PING;
        port_send_wait(wport, &m);

        port_recv_wait(mport, &m);

        if (first && m.type != MSG_PONG) {
            printf("2) IPC ping-pong: bad reply type %u\n", m.type);
            return;
        }
        first = 0;
    }

    t1 = ms_clock();

    /* Дождаться смерти воркера. */
    while (sc3(SYS_ALIVE, pid, 0, 0) == 1)
        sys_yield();

    if (t1 == t0)
        t1 = t0 + 1;

    printf("2) IPC ping-pong (%d round-trips, 1012B msg):\n", count);
    printf("   total=%lu ms -> %lu us/round-trip\n",
           (unsigned long)(t1 - t0),
           (unsigned long)((t1 - t0) * 1000UL / count));
}

/* --- 3. Запуск процесса --- */
static void test_exec(void)
{
    const int N = 12;
    char *wargv[2];
    uint64_t t0, t1, sum = 0, best = 0;
    int i, pid, ok = 0;

    wargv[0] = (char *)"bench";
    wargv[1] = "--fast";

    for (i = 0; i < N; i++) {

        t0 = tsc_read();

        pid = (int)sc3(SYS_EXEC, (long)"bench", 2, (long)wargv);
        if (pid < 0) {
            printf("3) exec spawn: FAILED at iter %d\n", i);
            return;
        }

        while (sc3(SYS_ALIVE, pid, 0, 0) == 1)
            sys_yield();

        t1 = tsc_read();

        {
            uint64_t d = t1 - t0;

            sum += d;
            if (i == 0 || d < best)
                best = d;
            ok++;
        }
    }

    if (!ok)
        return;

    printf("3) exec spawn+exit (полный цикл, включая ожидание):\n");
    printf("   best=%lu cycles  avg=%lu cycles  (N=%d)\n",
           (unsigned long)best,
           (unsigned long)(sum / ok), ok);
}

/* --- 4. Память (SHM + memcpy) --- */
static void test_memcpy(void)
{
    const uint64_t CHUNK = 256u * 1024u;
    const int ROUNDS = 800;     /* 800 * 256KB = 200 MB */
    uint8_t *a, *b;
    uint64_t t0, t1, c0, c1, bytes;
    volatile uint64_t checksum = 0;
    int i;
    uint64_t off;

    a = (uint8_t *)sc3(SYS_SHMALLOC, (long)CHUNK, 0, 0);
    b = (uint8_t *)sc3(SYS_SHMALLOC, (long)CHUNK, 0, 0);

    if (!a || !b) {
        printf("4) memcpy: FAILED (shm_alloc)\n");
        return;
    }

    memset(a, 0x5A, CHUNK);
    memset(b, 0, CHUNK);

    t0 = ms_clock();
    c0 = tsc_read();

    for (i = 0; i < ROUNDS; i++)
        memcpy(b, a, CHUNK);

    c1 = tsc_read();
    t1 = ms_clock();
    /* Контроль результата: не даём считать тест пустой работой. */
    for (off = 0; off < CHUNK; off += 4096)
        checksum += b[off];

    if (t1 == t0)
        t1 = t0 + 1;

    bytes = (uint64_t)ROUNDS * CHUNK;

    printf("4) memcpy 256KB x %d:\n", ROUNDS);
    printf("   %lu MB in %lu ms -> %lu MB/s\n",
           (unsigned long)(bytes >> 20),
           (unsigned long)(t1 - t0),
           (unsigned long)(bytes >> 20) * 1000UL / (unsigned long)(t1 - t0));
    printf("   %lu milli-cycles/byte; checksum=%lu\n",
           (unsigned long)(((c1 - c0) * 1000ull) / bytes),
           (unsigned long)checksum);

    sc3(SYS_SHMFREE, (long)a, 0, 0);
    sc3(SYS_SHMFREE, (long)b, 0, 0);
}

/* ============================================================
 * Точка входа
 * ============================================================ */

int main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "--pong") == 0) {
        int count = 2000;

        if (argc >= 4) {
            /* argv[3] = count (десятичное, без знака). */
            const char *p = argv[3];

            count = 0;
            while (*p >= '0' && *p <= '9')
                count = count * 10 + (*p++ - '0');
        }

        worker_pong(argv[2], count);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--fast") == 0)
        return 0;               /* exec-воркер: помер сразу */

    printf("=== ArtyomXOS benchmark ===\n");
    printf("pid=%ld\n", sc3(SYS_GETPID, 0, 0, 0));

    test_syscall();
    test_ipc();
    test_exec();
    test_memcpy();

    printf("=== done ===\n");
    return 0;
}
