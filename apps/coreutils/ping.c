#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

long syscall(long num, ...);

#define AX_SYS_PING 213

/*
 * ping -- фронтенд к syscall 213.
 *
 * Один echo-request на пакет: ядро само грело ARP-кэш, повторяет
 * неудачную отправку и матчит ответ по паре (id=pid, seq), поэтому
 * повторять запрос из userspace не нужно -- раньше каждый пакет
 * дублировался до 8 раз (PING_MAX_HOPS), а "потерей" считался пакет,
 * на который не ответил НИ ОДИН из восьми дубликатов.  Теперь
 * статистика честная: transmitted = реально отправленные запросы,
 * received = ответы на них.
 *
 * Коды возврата syscall 213:
 *   1  -- ответ получен;
 *   0  -- запрос отправлен, ответа нет (таймаут);
 *  -1  -- неверные аргументы / нет сетевого устройства;
 *  -2  -- отправить не удалось (после повторов в ядре).
 */

typedef struct {
    int sent;
    int recv;
    int last_seq;
    int last_ttl;
    int last_size;
    long last_rtt_us;
    long min_us;
    long max_us;
    long sum_us;
} ping_stats_t;

static int ping_once(const char *ip, int seq, int wait_ms, ping_stats_t *st)
{
    long reply[6];
    long rc;
    int i;

    for (i = 0; i < 6; i++)
        reply[i] = 0;

    rc = syscall(AX_SYS_PING, (long)ip, (long)seq, (long)wait_ms, (long)reply);
    if (rc < 0)
        return -1;
    if (reply[0] != 1)
        return 0;

    st->recv++;
    st->last_seq = (int)reply[1];
    st->last_ttl = (int)reply[3];
    st->last_size = (int)reply[4];
    st->last_rtt_us = reply[5];

    if (st->min_us < 0 || st->last_rtt_us < st->min_us)
        st->min_us = st->last_rtt_us;
    if (st->last_rtt_us > st->max_us)
        st->max_us = st->last_rtt_us;
    st->sum_us += st->last_rtt_us;
    return 1;
}

/* RTT приходит из ядра в микросекундах; печатаем как ms с тремя знаками */
static void print_ms(long us)
{
    printf("%d.%03d", (int)(us / 1000), (int)(us % 1000));
}

int main(int argc, char *argv[])
{
    const char *target;
    int count = 4;
    int wait_ms = 1000;
    ping_stats_t st;
    int i;

    if (argc > 1)
        target = argv[1];
    else
        target = "127.0.0.1";

    if (argc > 2)
        count = atoi(argv[2]);
    if (count <= 0 || count > 1000)
        count = 4;

    if (argc > 3) {
        wait_ms = atoi(argv[3]);
        if (wait_ms <= 0 || wait_ms > 60000)
            wait_ms = 1000;
    }

    memset(&st, 0, sizeof(st));
    st.min_us = -1;

    printf("PING %s (%d packets, wait %d ms per packet):\n", target, count, wait_ms);

    for (i = 1; i <= count; i++) {
        int ok = ping_once(target, i, wait_ms, &st);

        if (ok > 0) {
            st.sent++;
            printf("%d bytes from %s: icmp_seq=%d ttl=%d time=",
                   st.last_size, target, st.last_seq, st.last_ttl);
            print_ms(st.last_rtt_us);
            printf(" ms\n");
        } else if (ok == 0) {
            st.sent++;
            printf("From %s: icmp_seq=%d Request timeout\n", target, i);
        } else {
            printf("From %s: icmp_seq=%d send failed (no device or network down)\n", target, i);
        }
    }

    printf("\n--- %s ping statistics ---\n", target);
    printf("%d packets transmitted, %d received, %d%% packet loss\n",
           st.sent, st.recv,
           st.sent > 0 ? (int)(100 - (st.recv * 100) / st.sent) : 0);

    if (st.recv > 0) {
        long avg = st.sum_us / st.recv;
        printf("rtt min/avg/max = ");
        print_ms(st.min_us);
        printf("/");
        print_ms(avg);
        printf("/");
        print_ms(st.max_us);
        printf(" ms\n");
    }

    return st.recv > 0 ? 0 : 1;
}
