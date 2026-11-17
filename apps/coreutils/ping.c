#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

long syscall(long num, ...);

#define AX_SYS_PING 213

#define PING_MAX_HOPS 8

typedef struct {
    int sent;
    int recv;
    int last_seq;
    int last_ttl;
    int last_size;
    long last_rtt_ms;
    long min_ms;
    long max_ms;
    long sum_ms;
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
        return 0;
    if (reply[0] != 1)
        return 0;

    st->recv++;
    st->last_seq = (int)reply[1];
    st->last_ttl = (int)reply[3];
    st->last_size = (int)reply[4];
    st->last_rtt_ms = reply[5];

    if (st->min_ms < 0 || st->last_rtt_ms < st->min_ms)
        st->min_ms = st->last_rtt_ms;
    if (st->last_rtt_ms > st->max_ms)
        st->max_ms = st->last_rtt_ms;
    st->sum_ms += st->last_rtt_ms;
    return 1;
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

    memset(&st, 0, sizeof(st));
    st.min_ms = -1;

    printf("PING %s (wait %d ms per packet):\n", target, wait_ms);

    for (i = 1; i <= count; i++) {
        int ok = 0;
        int hop;

        st.sent++;

        for (hop = 0; hop < PING_MAX_HOPS && !ok; hop++) {
            ok = ping_once(target, i, wait_ms, &st);
            if (!ok) {
                int k;
                for (k = 0; k < 0x200000; k++)
                    ;
            }
        }

        if (ok) {
            int frac = (int)(st.last_rtt_ms / 10) % 100;
            printf("%d bytes from %s: icmp_seq=%d ttl=%d time=%d.%d%d ms\n",
                   st.last_size, target, st.last_seq, st.last_ttl,
                   (int)(st.last_rtt_ms / 1000), (int)((st.last_rtt_ms / 100) % 10), frac);
        } else {
            printf("From %s: icmp_seq=%d Request timeout\n", target, i);
        }
    }

    printf("\n--- %s ping statistics ---\n", target);
    printf("%d packets transmitted, %d received, %d%% packet loss\n",
           st.sent, st.recv,
           st.sent > 0 ? (int)(100 - (st.recv * 100) / st.sent) : 0);

    if (st.recv > 0) {
        long avg = st.sum_ms / st.recv;
        printf("rtt min/avg/max = %d.%d/%d.%d/%d.%d ms\n",
               (int)(st.min_ms / 1000), (int)((st.min_ms / 100) % 10),
               (int)(avg / 1000), (int)((avg / 100) % 10),
               (int)(st.max_ms / 1000), (int)((st.max_ms / 100) % 10));
    }

    return st.recv > 0 ? 0 : 1;
}
