#ifndef IXN_L4_H
#define IXN_L4_H

#include "ixn_types.h"
#include "ixn_skb.h"
#include "ixn_netdev.h"
#include "ixn_l2l3.h"

#define IXN_UDP_HDR_LEN     8u
#define IXN_TCP_HDR_LEN_MIN 20u
#define IXN_TCP_MSS_DEFAULT 1460u
#define IXN_TCP_WINDOW_DEFAULT 65535u
#define IXN_TCP_MAX_SOCKETS  64u
#define IXN_UDP_MAX_SOCKETS  128u

#define IXN_TCP_FIN 0x01u
#define IXN_TCP_SYN 0x02u
#define IXN_TCP_RST 0x04u
#define IXN_TCP_PSH 0x08u
#define IXN_TCP_ACK 0x10u
#define IXN_TCP_URG 0x20u
#define IXN_TCP_ECE 0x40u
#define IXN_TCP_CWR 0x80u

typedef struct {
    ixn_u16 src;
    ixn_u16 dst;
    ixn_u16 len;
    ixn_u16 checksum;
} ixn_udp_hdr_t;

typedef struct {
    ixn_u16 src;
    ixn_u16 dst;
    ixn_u32 seq;
    ixn_u32 ack;
    ixn_u8  off_flags_hi;
    ixn_u8  flags;
    ixn_u16 window;
    ixn_u16 checksum;
    ixn_u16 urg_ptr;
} ixn_tcp_hdr_t;

typedef enum {
    IXN_TCP_CLOSED = 0,
    IXN_TCP_LISTEN,
    IXN_TCP_SYN_SENT,
    IXN_TCP_SYN_RECEIVED,
    IXN_TCP_ESTABLISHED,
    IXN_TCP_FIN_WAIT_1,
    IXN_TCP_FIN_WAIT_2,
    IXN_TCP_CLOSE_WAIT,
    IXN_TCP_CLOSING,
    IXN_TCP_LAST_ACK,
    IXN_TCP_TIME_WAIT
} ixn_tcp_state_t;

typedef enum {
    IXN_SOCK_TYPE_UNUSED = 0,
    IXN_SOCK_TYPE_UDP,
    IXN_SOCK_TYPE_TCP,
    IXN_SOCK_TYPE_RAW
} ixn_sock_type_t;

typedef enum {
    IXN_SOCK_F_BOUND    = (1u << 0),
    IXN_SOCK_F_LISTEN   = (1u << 1),
    IXN_SOCK_F_CONNECT  = (1u << 2),
    IXN_SOCK_F_NONBLOCK = (1u << 3),
    IXN_SOCK_F_REUSE    = (1u << 4)
} ixn_sock_flags_t;

typedef struct {
    ixn_u32 src_ip;
    ixn_u32 dst_ip;
    ixn_u8  zero;
    ixn_u8  protocol;
    ixn_u16 len;
} ixn_pseudo_hdr_t;

typedef struct ixn_socket {
    ixn_u32 id;
    ixn_sock_type_t type;
    ixn_u32 flags;
    ixn_netdev_t *dev;
    ixn_u32 local_ip;
    ixn_u16 local_port;
    ixn_u32 remote_ip;
    ixn_u16 remote_port;

    ixn_skb_t *rx_queue;
    ixn_u32 rx_queue_len;

    ixn_tcp_state_t tcp_state;
    ixn_u32 tx_seq;
    ixn_u32 rx_seq;
    ixn_u32 tx_ack;
    ixn_u16 window;
    ixn_u32 mss;
    ixn_u64 last_activity;

    ixn_u8 in_use;
    ixn_u8 backlog;
    struct ixn_socket *parent;
    struct ixn_socket *next;
} ixn_socket_t;

void ixn_l4_init(void);

int ixn_udp_rx(ixn_netdev_t *dev, ixn_skb_t *skb, ixn_ipv4_hdr_t *iph);
int ixn_udp_send(ixn_netdev_t *dev, ixn_u32 src_ip, ixn_u16 src_port, ixn_u32 dst_ip, ixn_u16 dst_port, const ixn_u8 *data, ixn_u32 len);
int ixn_udp_send_skb(ixn_netdev_t *dev, ixn_u32 src_ip, ixn_u16 src_port, ixn_u32 dst_ip, ixn_u16 dst_port, ixn_skb_t *payload);

int ixn_tcp_rx(ixn_netdev_t *dev, ixn_skb_t *skb, ixn_ipv4_hdr_t *iph);
int ixn_tcp_connect(ixn_socket_t *s, ixn_u32 dst_ip, ixn_u16 dst_port);
int ixn_tcp_send(ixn_socket_t *s, const ixn_u8 *data, ixn_u32 len);
int ixn_tcp_close(ixn_socket_t *s);
int ixn_tcp_listen(ixn_socket_t *s, ixn_u8 backlog);
ixn_socket_t *ixn_tcp_accept(ixn_socket_t *s);

ixn_u16 ixn_l4_checksum(ixn_u32 src_ip, ixn_u32 dst_ip, ixn_u8 protocol, const ixn_u8 *data, ixn_u32 len);

ixn_socket_t *ixn_socket_alloc(ixn_sock_type_t type);
void ixn_socket_free(ixn_socket_t *s);
ixn_socket_t *ixn_socket_get(ixn_u32 id);
int ixn_socket_bind(ixn_socket_t *s, ixn_u32 ip, ixn_u16 port);
int ixn_udp_socket_recv(ixn_socket_t *s, ixn_u32 *out_src_ip, ixn_u16 *out_src_port, ixn_u8 *buf, ixn_u32 cap);
int ixn_udp_socket_send(ixn_socket_t *s, ixn_u32 dst_ip, ixn_u16 dst_port, const ixn_u8 *data, ixn_u32 len);
/* raw ICMP: копия каждого входящего ICMP-сообщения всем raw-сокетам */
int ixn_icmp_socket_push(ixn_skb_t *skb);
int ixn_icmp_socket_recv(ixn_socket_t *s, ixn_u8 *buf, ixn_u32 cap);
int ixn_tcp_socket_recv(ixn_socket_t *s, ixn_u8 *buf, ixn_u32 cap);
ixn_u32 ixn_socket_count(ixn_sock_type_t type);

static inline ixn_u8 ixn_tcp_data_off(const ixn_tcp_hdr_t *h) { return (h->off_flags_hi >> 4u) & 0xFu; }
static inline ixn_u32 ixn_tcp_hdr_len(const ixn_tcp_hdr_t *h) { return (ixn_u32)((h->off_flags_hi >> 4u) & 0xFu) * 4u; }

#endif
