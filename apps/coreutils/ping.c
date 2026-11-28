#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

long syscall(long num, ...);

/* Номера syscall'ов ArtyomX64 (lib/syscalls.c) */
#define AX_SYS_CLOSE     6
#define AX_SYS_YIELD     151
#define AX_SYS_GETPID    182
#define AX_SYS_TICKS_MS  196
#define AX_SYS_SOCKET    200
#define AX_SYS_SENDTO    205
#define AX_SYS_RECVFROM  206
#define AX_SYS_DNS       214

#define AX_AF_INET   2
#define AX_SOCK_RAW  3

#define AX_ICMP_ECHO_REQUEST 8
#define AX_ICMP_ECHO_REPLY   0

#define AX_HTONS(x) ((uint16_t)((((uint16_t)(x) & 0x00FFu) << 8u) | \
                                 (((uint16_t)(x) & 0xFF00u) >> 8u)))

typedef struct {
    uint16_t sa_family;
    uint16_t sa_port;
    uint32_t sa_addr;
    uint8_t  sa_zero[8];
} ax_sockaddr_in_t;

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} ax_icmp_hdr_t;

/*
 * ping -- обычная userspace-программа поверх raw ICMP-сокета.
 *
 * Ядро больше не знает про ping: оно отдаёт копию каждого входящего
 * ICMP-сообщения raw-сокету (syscall 200/205/206), а следующий
 * echo-request, ожидание ответа, RTT и статистику считает эта программа.
 * ARP и контрольную сумму ICMP по-прежнему берёт на себя стек.
 *
 * Имя разрешается syscall'ом 214 (DNS из DHCP/8.8.8.8); если аргумент
 * уже похож на a.b.c.d, запрос к DNS не делается.
 */

typedef struct {
    int  sent;
    int  recv;
    int  last_size;
    long last_rtt_ms;
    long min_ms;
    long max_ms;
    long sum_ms;
} ping_stats_t;

static long now_ms(void)
{
    return syscall(AX_SYS_TICKS_MS);
}

/* "a.b.c.d" -> host order (a.b.c.d == 0xAABBCCDD), 0 если это не IP */
static int parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t ip = 0u, v = 0u;
    int parts = 0;
    int have = 0;

    if (!s || !*s)
        return 0;

    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            v = v * 10u + (uint32_t)(*s - '0');
            if (v > 255u)
                return 0;
            have = 1;
        } else if (*s == '.') {
            if (!have || parts == 3)
                return 0;
            ip = (ip << 8u) | v;
            v = 0u;
            have = 0;
            parts++;
        } else {
            return 0;
        }
    }

    if (parts != 3 || !have)
        return 0;

    *out = (ip << 8u) | v;
    return 1;
}

static void print_ip(uint32_t ip)
{
    printf("%u.%u.%u.%u",
           (unsigned)((ip >> 24u) & 0xFFu),
           (unsigned)((ip >> 16u) & 0xFFu),
           (unsigned)((ip >> 8u) & 0xFFu),
           (unsigned)(ip & 0xFFu));
}

/*
 * Один echo-request и ожидание ответа с нашим (id, seq) не дольше
 * wait_ms.  Возврат: 1 -- ответ, 0 -- таймаут, -1 -- отправка не удалась.
 */
static int ping_once(int fd, uint32_t dst, uint16_t id, int seq,
                     int wait_ms, ping_stats_t *st)
{
    uint8_t pkt[sizeof(ax_icmp_hdr_t) + 14];
    uint8_t rx[512];
    ax_icmp_hdr_t *out = (ax_icmp_hdr_t *)pkt;
    ax_sockaddr_in_t sa;
    uint16_t nid = AX_HTONS(id);
    uint16_t nseq = AX_HTONS((uint16_t)seq);
    long t0, deadline;

    memset(pkt, 0, sizeof(pkt));
    out->type = AX_ICMP_ECHO_REQUEST;
    out->code = 0;
    out->checksum = 0;              /* пересчитывает стек */
    out->id = nid;
    out->seq = nseq;
    memcpy(pkt + sizeof(*out), "ArtyomX64 ping", 14);

    memset(&sa, 0, sizeof(sa));
    sa.sa_family = AX_AF_INET;
    sa.sa_addr = dst;

    if (syscall(AX_SYS_SENDTO, fd, (long)pkt, (long)sizeof(pkt), 0,
                (long)&sa, (long)sizeof(sa)) < 0)
        return -1;

    t0 = now_ms();
    deadline = t0 + wait_ms;

    for (;;) {
        int n = (int)syscall(AX_SYS_RECVFROM, fd, (long)rx, (long)sizeof(rx),
                             0, 0, 0);

        if (n >= (int)sizeof(ax_icmp_hdr_t)) {
            ax_icmp_hdr_t *r = (ax_icmp_hdr_t *)rx;

            if (r->type == AX_ICMP_ECHO_REPLY && r->id == nid && r->seq == nseq) {
                long rtt = now_ms() - t0;

                st->recv++;
                st->last_size = n;
                if (st->min_ms < 0 || rtt < st->min_ms)
                    st->min_ms = rtt;
                if (rtt > st->max_ms)
                    st->max_ms = rtt;
                st->sum_ms += rtt;
                st->last_rtt_ms = rtt;
                return 1;
            }
            /* Чужой/запоздалый ответ: читаем дальше, пока не истечёт время. */
        }

        if (now_ms() >= deadline)
            break;

        /* Кооперативный планировщик: отдаём квант остальным задачам. */
        syscall(AX_SYS_YIELD);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    const char *target;
    uint32_t dst = 0u;
    int numeric;
    int count = 4;
    int wait_ms = 1000;
    ping_stats_t st;
    uint16_t id;
    int fd, i;

    target = argc > 1 ? argv[1] : "127.0.0.1";

    if (argc > 2)
        count = atoi(argv[2]);
    if (count <= 0 || count > 1000)
        count = 4;

    if (argc > 3) {
        wait_ms = atoi(argv[3]);
        if (wait_ms <= 0 || wait_ms > 60000)
            wait_ms = 1000;
    }

    numeric = parse_ipv4(target, &dst);
    if (!numeric) {
        if (syscall(AX_SYS_DNS, (long)target, (long)&dst) != 0 || dst == 0u) {
            printf("ping: cannot resolve %s (DNS unavailable)\n", target);
            return 1;
        }
    }

    fd = (int)syscall(AX_SYS_SOCKET, AX_AF_INET, AX_SOCK_RAW, 1);
    if (fd < 0) {
        printf("ping: raw socket failed (no network device?)\n");
        return 1;
    }

    id = (uint16_t)syscall(AX_SYS_GETPID);

    memset(&st, 0, sizeof(st));
    st.min_ms = -1;

    printf("PING %s", target);
    if (!numeric) {
        printf(" (");
        print_ip(dst);
        printf(")");
    }
    printf(" (%d packets, wait %d ms per packet):\n", count, wait_ms);

    for (i = 1; i <= count; i++) {
        int ok = ping_once(fd, dst, id, i, wait_ms, &st);

        if (ok > 0) {
            st.sent++;
            printf("%d bytes from %s: icmp_seq=%d time=%d ms\n",
                   st.last_size, target, i, (int)st.last_rtt_ms);
        } else if (ok == 0) {
            st.sent++;
            printf("From %s: icmp_seq=%d Request timeout\n", target, i);
        } else {
            printf("From %s: icmp_seq=%d send failed (no route or arp)\n", target, i);
        }
    }

    syscall(AX_SYS_CLOSE, fd);

    printf("\n--- %s ping statistics ---\n", target);
    printf("%d packets transmitted, %d received, %d%% packet loss\n",
           st.sent, st.recv,
           st.sent > 0 ? (int)(100 - (st.recv * 100) / st.sent) : 0);

    if (st.recv > 0) {
        long avg = st.sum_ms / st.recv;
        printf("rtt min/avg/max = %d/%d/%d ms\n",
               (int)st.min_ms, (int)avg, (int)st.max_ms);
    }

    return st.recv > 0 ? 0 : 1;
}
