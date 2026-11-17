#include "ixn_l4.h"
#include "ixn_dhcp_dns.h"
#include "ixn_compat.h"

static ixn_socket_t g_ixn_sockets[IXN_TCP_MAX_SOCKETS + IXN_UDP_MAX_SOCKETS];
static volatile ixn_irq_lock_t g_ixn_sock_lock = 0u;
static ixn_u32 g_ixn_sock_next_id = 1u;
static ixn_u32 g_ixn_tcp_isn = 0x12345678u;

void ixn_l4_init(void) {
    ixn_memset(g_ixn_sockets, 0, sizeof(g_ixn_sockets));
    g_ixn_sock_next_id = 1u;
}

ixn_u16 ixn_l4_checksum(ixn_u32 src_ip, ixn_u32 dst_ip, ixn_u8 protocol, const ixn_u8 *data, ixn_u32 len) {
    ixn_pseudo_hdr_t ph;
    ixn_u32 sum = 0u;
    ixn_u32 i;
    ph.src_ip = IXN_htonl(src_ip); /* в pseudo-header байты как на проводе */
    ph.dst_ip = IXN_htonl(dst_ip);
    ph.zero = 0u;
    ph.protocol = protocol;
    ph.len = IXN_htons((ixn_u16)len);
    {
        const ixn_u8 *p = (const ixn_u8 *)&ph;
        for (i = 0u; i + 1u < sizeof(ph); i += 2u) sum += (ixn_u32)p[i] | ((ixn_u32)p[i + 1u] << 8u);
    }
    for (i = 0u; i + 1u < len; i += 2u) sum += (ixn_u32)data[i] | ((ixn_u32)data[i + 1u] << 8u);
    if (i < len) sum += (ixn_u32)data[i];
    while (sum >> 16u) sum = (sum & 0xFFFFu) + (sum >> 16u);
    return (ixn_u16)(~sum & 0xFFFFu);
}

ixn_socket_t *ixn_socket_alloc(ixn_sock_type_t type) {
    ixn_u64 flags;
    ixn_u32 i;
    ixn_u32 cap = (type == IXN_SOCK_TYPE_UDP) ? (IXN_TCP_MAX_SOCKETS + IXN_UDP_MAX_SOCKETS) : IXN_TCP_MAX_SOCKETS;
    flags = ixn_irq_lock_acquire(&g_ixn_sock_lock);
    for (i = 0u; i < cap; i++) {
        if (!g_ixn_sockets[i].in_use) {
            ixn_socket_t *s = &g_ixn_sockets[i];
            ixn_memset(s, 0, sizeof(*s));
            s->id = g_ixn_sock_next_id++;
            s->type = type;
            s->in_use = 1u;
            s->window = IXN_TCP_WINDOW_DEFAULT;
            s->mss = IXN_TCP_MSS_DEFAULT;
            s->tcp_state = IXN_TCP_CLOSED;
            ixn_irq_lock_release(&g_ixn_sock_lock, flags);
            return s;
        }
    }
    ixn_irq_lock_release(&g_ixn_sock_lock, flags);
    return IXN_NULL;
}

void ixn_socket_free(ixn_socket_t *s) {
    ixn_u64 flags;
    if (!s) return;
    flags = ixn_irq_lock_acquire(&g_ixn_sock_lock);
    ixn_skb_queue_purge(&s->rx_queue);
    s->rx_queue_len = 0u;
    s->in_use = 0u;
    s->tcp_state = IXN_TCP_CLOSED;
    ixn_irq_lock_release(&g_ixn_sock_lock, flags);
}

ixn_socket_t *ixn_socket_get(ixn_u32 id) {
    ixn_u32 i;
    for (i = 0u; i < IXN_TCP_MAX_SOCKETS + IXN_UDP_MAX_SOCKETS; i++) {
        if (g_ixn_sockets[i].in_use && g_ixn_sockets[i].id == id) return &g_ixn_sockets[i];
    }
    return IXN_NULL;
}

int ixn_socket_bind(ixn_socket_t *s, ixn_u32 ip, ixn_u16 port) {
    if (!s) return -1;
    s->local_ip = ip;
    s->local_port = port;
    s->flags |= IXN_SOCK_F_BOUND;
    return 0;
}

ixn_u32 ixn_socket_count(ixn_sock_type_t type) {
    ixn_u32 i, n = 0u;
    for (i = 0u; i < IXN_TCP_MAX_SOCKETS + IXN_UDP_MAX_SOCKETS; i++) {
        if (g_ixn_sockets[i].in_use && g_ixn_sockets[i].type == type) n++;
    }
    return n;
}

static ixn_socket_t *ixn_udp_find(ixn_u32 local_ip, ixn_u16 local_port, ixn_u32 remote_ip, ixn_u16 remote_port) {
    ixn_u32 i;
    ixn_socket_t *wild = IXN_NULL;
    for (i = IXN_TCP_MAX_SOCKETS; i < IXN_TCP_MAX_SOCKETS + IXN_UDP_MAX_SOCKETS; i++) {
        ixn_socket_t *s = &g_ixn_sockets[i];
        if (!s->in_use || s->type != IXN_SOCK_TYPE_UDP) continue;
        if (s->local_port != local_port) continue;
        if (s->local_ip == local_ip && (remote_port == 0u || s->remote_port == 0u || s->remote_port == remote_port)) {
            if (remote_ip == 0u || s->remote_ip == 0u || s->remote_ip == remote_ip) return s;
        }
        if (s->local_ip == 0u && !wild) wild = s;
    }
    return wild;
}

int ixn_udp_rx(ixn_netdev_t *dev, ixn_skb_t *skb, ixn_ipv4_hdr_t *iph) {
    ixn_udp_hdr_t *uh;
    ixn_socket_t *s;
    ixn_u16 dport;
    if (!dev || !skb || !iph || skb->len < IXN_UDP_HDR_LEN) return -1;
    uh = (ixn_udp_hdr_t *)skb->data;
    dport = IXN_ntohs(uh->dst);
    if (dport == IXN_DHCP_CLIENT_PORT) {
        ixn_dhcp_client_t *dhcp = ixn_dhcp_get_hook();
        if (dhcp) {
            ixn_skb_pull(skb, IXN_UDP_HDR_LEN);
            ixn_dhcp_rx(dhcp, dev, skb->data, skb->len);
            return 0;
        }
    }
    ixn_skb_pull(skb, IXN_UDP_HDR_LEN);
    s = ixn_udp_find(iph->dst, IXN_ntohs(uh->dst), iph->src, IXN_ntohs(uh->src));
    if (s) {
        ixn_skb_t *q = ixn_skb_clone(skb);
        if (q) {
            ixn_skb_queue_push(&s->rx_queue, q);
            s->rx_queue_len++;
            s->last_activity = ixn_ticks_ms();
        }
        return 0;
    }
    return 0;
}

int ixn_udp_send_skb(ixn_netdev_t *dev, ixn_u32 src_ip, ixn_u16 src_port, ixn_u32 dst_ip, ixn_u16 dst_port, ixn_skb_t *payload) {
    ixn_udp_hdr_t *uh;
    if (!dev || !payload) return -1;
    ixn_skb_push(payload, IXN_UDP_HDR_LEN);
    uh = (ixn_udp_hdr_t *)payload->data;
    uh->src = IXN_htons(src_port);
    uh->dst = IXN_htons(dst_port);
    uh->len = IXN_htons((ixn_u16)payload->len);
    uh->checksum = 0u;
    uh->checksum = ixn_l4_checksum(src_ip, dst_ip, IXN_IP_PROTO_UDP, payload->data, payload->len);
    if (uh->checksum == 0u) uh->checksum = 0xFFFFu;
    return ixn_ipv4_tx(dev, payload, dst_ip, IXN_IP_PROTO_UDP);
}

int ixn_udp_send(ixn_netdev_t *dev, ixn_u32 src_ip, ixn_u16 src_port, ixn_u32 dst_ip, ixn_u16 dst_port, const ixn_u8 *data, ixn_u32 len) {
    ixn_skb_t *skb;
    if (!dev || (!data && len)) return -1;
    skb = ixn_skb_alloc(len + IXN_UDP_HDR_LEN + 20u);
    if (!skb) return -2;
    ixn_skb_reserve(skb, 20u);
    if (len) {
        ixn_skb_put(skb, len);
        ixn_memcpy(skb->data, data, len);
    }
    return ixn_udp_send_skb(dev, src_ip, src_port, dst_ip, dst_port, skb);
}

int ixn_udp_socket_send(ixn_socket_t *s, ixn_u32 dst_ip, ixn_u16 dst_port, const ixn_u8 *data, ixn_u32 len) {
    ixn_netdev_t *dev;
    if (!s || s->type != IXN_SOCK_TYPE_UDP) return -1;
    dev = s->dev ? s->dev : ixn_netdev_get_first_up();
    if (!dev) return -2;
    return ixn_udp_send(dev, s->local_ip ? s->local_ip : dev->ipv4_addr, s->local_port, dst_ip, dst_port, data, len);
}

int ixn_udp_socket_recv(ixn_socket_t *s, ixn_u32 *out_src_ip, ixn_u16 *out_src_port, ixn_u8 *buf, ixn_u32 cap) {
    ixn_skb_t *skb;
    ixn_ipv4_hdr_t *iph;
    ixn_udp_hdr_t *uh;
    ixn_u32 copy;
    if (!s || s->type != IXN_SOCK_TYPE_UDP) return -1;
    skb = ixn_skb_queue_pop(&s->rx_queue);
    if (!skb) return -2;
    s->rx_queue_len--;
    iph = (ixn_ipv4_hdr_t *)ixn_skb_network_header(skb);
    uh = (ixn_udp_hdr_t *)(ixn_skb_network_header(skb) + ixn_ipv4_hdr_len(iph));
    if (out_src_ip) *out_src_ip = iph->src;
    if (out_src_port) *out_src_port = IXN_ntohs(uh->src);
    copy = skb->len < cap ? skb->len : cap;
    if (buf && copy) ixn_memcpy(buf, skb->data, copy);
    ixn_skb_free(skb);
    return (int)copy;
}

static ixn_socket_t *ixn_tcp_find(ixn_u32 local_ip, ixn_u16 local_port, ixn_u32 remote_ip, ixn_u16 remote_port) {
    ixn_u32 i;
    ixn_socket_t *listen = IXN_NULL;
    for (i = 0u; i < IXN_TCP_MAX_SOCKETS; i++) {
        ixn_socket_t *s = &g_ixn_sockets[i];
        if (!s->in_use || s->type != IXN_SOCK_TYPE_TCP) continue;
        if (s->local_port != local_port) continue;
        if (s->tcp_state == IXN_TCP_LISTEN) { if (!listen) listen = s; continue; }
        if (s->remote_ip == remote_ip && s->remote_port == remote_port) return s;
    }
    return listen;
}

static int ixn_tcp_send_pkt(ixn_socket_t *s, ixn_u8 flags, const ixn_u8 *data, ixn_u32 len) {
    ixn_netdev_t *dev;
    ixn_skb_t *skb;
    ixn_tcp_hdr_t *th;
    ixn_u32 hdrlen = IXN_TCP_HDR_LEN_MIN;
    if (!s) return -1;
    dev = s->dev ? s->dev : ixn_netdev_get_first_up();
    if (!dev) return -2;
    skb = ixn_skb_alloc(len + hdrlen + 20u);
    if (!skb) return -3;
    ixn_skb_reserve(skb, 20u);
    ixn_skb_put(skb, hdrlen + len);
    th = (ixn_tcp_hdr_t *)skb->data;
    th->src = IXN_htons(s->local_port);
    th->dst = IXN_htons(s->remote_port);
    th->seq = IXN_htonl(s->tx_seq);
    th->ack = IXN_htonl(s->tx_ack);
    th->off_flags_hi = (ixn_u8)((hdrlen / 4u) << 4u);
    th->flags = flags;
    th->window = IXN_htons(s->window);
    th->checksum = 0u;
    th->urg_ptr = 0u;
    if (data && len) ixn_memcpy(skb->data + hdrlen, data, len);
    th->checksum = ixn_l4_checksum(s->local_ip ? s->local_ip : dev->ipv4_addr, s->remote_ip, IXN_IP_PROTO_TCP, skb->data, skb->len);
    {
        ixn_skb_t *ip_skb = skb;
        int rc = ixn_ipv4_tx(dev, ip_skb, s->remote_ip, IXN_IP_PROTO_TCP);
        if (rc == 0) {
            if (flags & IXN_TCP_SYN) s->tx_seq += 1u;
            if (flags & IXN_TCP_FIN) s->tx_seq += 1u;
            if (len) s->tx_seq += len;
        }
        return rc;
    }
}

int ixn_tcp_connect(ixn_socket_t *s, ixn_u32 dst_ip, ixn_u16 dst_port) {
    ixn_netdev_t *dev;
    if (!s || s->type != IXN_SOCK_TYPE_TCP) return -1;
    dev = s->dev ? s->dev : ixn_netdev_get_first_up();
    if (!dev) return -2;
    s->remote_ip = dst_ip;
    s->remote_port = dst_port;
    if (!s->local_ip) s->local_ip = dev->ipv4_addr;
    if (!s->local_port) {
        s->local_port = IXN_htons(40000u + (ixn_u16)(ixn_atomic_inc32(&g_ixn_sock_next_id) & 0xFFFF));
    }
    s->tx_seq = g_ixn_tcp_isn + (ixn_ticks_ms() & 0xFFFFFFFFu);
    s->rx_seq = 0u;
    s->tx_ack = 0u;
    s->tcp_state = IXN_TCP_SYN_SENT;
    return ixn_tcp_send_pkt(s, IXN_TCP_SYN, IXN_NULL, 0u);
}

int ixn_tcp_listen(ixn_socket_t *s, ixn_u8 backlog) {
    if (!s || s->type != IXN_SOCK_TYPE_TCP) return -1;
    s->backlog = backlog ? backlog : 8u;
    s->tcp_state = IXN_TCP_LISTEN;
    s->flags |= IXN_SOCK_F_LISTEN;
    return 0;
}

ixn_socket_t *ixn_tcp_accept(ixn_socket_t *s) {
    ixn_u32 i;
    if (!s || s->type != IXN_SOCK_TYPE_TCP || s->tcp_state != IXN_TCP_LISTEN) return IXN_NULL;
    for (i = 0u; i < IXN_TCP_MAX_SOCKETS; i++) {
        ixn_socket_t *c = &g_ixn_sockets[i];
        if (c->in_use && c->parent == s && c->tcp_state == IXN_TCP_ESTABLISHED) {
            c->parent = IXN_NULL;
            return c;
        }
    }
    return IXN_NULL;
}

int ixn_tcp_send(ixn_socket_t *s, const ixn_u8 *data, ixn_u32 len) {
    ixn_u32 sent = 0u;
    if (!s || s->type != IXN_SOCK_TYPE_TCP) return -1;
    if (s->tcp_state != IXN_TCP_ESTABLISHED) return -2;
    while (sent < len) {
        ixn_u32 chunk = (len - sent) > s->mss ? s->mss : (len - sent);
        int rc = ixn_tcp_send_pkt(s, IXN_TCP_ACK | IXN_TCP_PSH, data + sent, chunk);
        if (rc != 0) return (int)sent;
        sent += chunk;
    }
    return (int)sent;
}

int ixn_tcp_close(ixn_socket_t *s) {
    if (!s) return -1;
    if (s->tcp_state == IXN_TCP_ESTABLISHED) {
        ixn_tcp_send_pkt(s, IXN_TCP_FIN | IXN_TCP_ACK, IXN_NULL, 0u);
        s->tcp_state = IXN_TCP_FIN_WAIT_1;
    } else {
        s->tcp_state = IXN_TCP_CLOSED;
    }
    return 0;
}

int ixn_tcp_socket_recv(ixn_socket_t *s, ixn_u8 *buf, ixn_u32 cap) {
    ixn_skb_t *skb;
    ixn_u32 copy;
    if (!s || s->type != IXN_SOCK_TYPE_TCP) return -1;
    skb = ixn_skb_queue_pop(&s->rx_queue);
    if (!skb) return -2;
    s->rx_queue_len--;
    copy = skb->len < cap ? skb->len : cap;
    if (buf && copy) ixn_memcpy(buf, skb->data, copy);
    ixn_skb_free(skb);
    return (int)copy;
}

int ixn_tcp_rx(ixn_netdev_t *dev, ixn_skb_t *skb, ixn_ipv4_hdr_t *iph) {
    ixn_tcp_hdr_t *th;
    ixn_socket_t *s;
    ixn_u32 hlen, datalen;
    ixn_u16 src_port, dst_port;
    ixn_u32 seq, ack;
    ixn_u8 flags;
    if (!dev || !skb || !iph || skb->len < IXN_TCP_HDR_LEN_MIN) return -1;
    th = (ixn_tcp_hdr_t *)skb->data;
    src_port = IXN_ntohs(th->src);
    dst_port = IXN_ntohs(th->dst);
    seq = IXN_ntohl(th->seq);
    ack = IXN_ntohl(th->ack);
    flags = th->flags;
    hlen = ixn_tcp_hdr_len(th);
    if (hlen < IXN_TCP_HDR_LEN_MIN || skb->len < hlen) return -2;
    datalen = skb->len - hlen;
    s = ixn_tcp_find(iph->dst, dst_port, iph->src, src_port);
    if (!s) {
        if (flags & IXN_TCP_SYN) {
            ixn_skb_t *rst;
            ixn_tcp_hdr_t *r;
            ixn_netdev_t *d = dev;
            rst = ixn_skb_alloc(IXN_TCP_HDR_LEN_MIN + 20u);
            if (!rst) return -3;
            ixn_skb_reserve(rst, 20u);
            ixn_skb_put(rst, IXN_TCP_HDR_LEN_MIN);
            r = (ixn_tcp_hdr_t *)rst->data;
            r->src = IXN_htons(dst_port);
            r->dst = IXN_htons(src_port);
            r->seq = 0u;
            r->ack = IXN_htonl(seq + 1u);
            r->off_flags_hi = (ixn_u8)((IXN_TCP_HDR_LEN_MIN / 4u) << 4u);
            r->flags = IXN_TCP_RST | IXN_TCP_ACK;
            r->window = 0u;
            r->checksum = 0u;
            r->urg_ptr = 0u;
            r->checksum = ixn_l4_checksum(iph->dst, iph->src, IXN_IP_PROTO_TCP, rst->data, rst->len);
            return ixn_ipv4_tx(d, rst, iph->src, IXN_IP_PROTO_TCP);
        }
        return 0;
    }
    switch (s->tcp_state) {
        case IXN_TCP_LISTEN:
            if (flags & IXN_TCP_SYN) {
                ixn_socket_t *child = ixn_socket_alloc(IXN_SOCK_TYPE_TCP);
                if (child) {
                    child->local_ip = iph->dst;
                    child->local_port = dst_port;
                    child->remote_ip = iph->src;
                    child->remote_port = src_port;
                    child->rx_seq = seq + 1u;
                    child->tx_ack = seq + 1u;
                    child->tx_seq = g_ixn_tcp_isn + (ixn_ticks_ms() & 0xFFFFFFFFu);
                    child->parent = s;
                    child->dev = dev;
                    child->tcp_state = IXN_TCP_SYN_RECEIVED;
                    ixn_tcp_send_pkt(child, IXN_TCP_SYN | IXN_TCP_ACK, IXN_NULL, 0u);
                }
            }
            break;
        case IXN_TCP_SYN_SENT:
            if ((flags & (IXN_TCP_SYN | IXN_TCP_ACK)) == (IXN_TCP_SYN | IXN_TCP_ACK)) {
                s->rx_seq = seq + 1u;
                s->tx_ack = seq + 1u;
                s->tcp_state = IXN_TCP_ESTABLISHED;
                ixn_tcp_send_pkt(s, IXN_TCP_ACK, IXN_NULL, 0u);
            } else if (flags & IXN_TCP_SYN) {
                s->rx_seq = seq + 1u;
                s->tx_ack = seq + 1u;
                s->tcp_state = IXN_TCP_SYN_RECEIVED;
                ixn_tcp_send_pkt(s, IXN_TCP_SYN | IXN_TCP_ACK, IXN_NULL, 0u);
            }
            break;
        case IXN_TCP_SYN_RECEIVED:
            if (flags & IXN_TCP_ACK) {
                s->tcp_state = IXN_TCP_ESTABLISHED;
            }
            break;
        case IXN_TCP_ESTABLISHED:
            if (datalen > 0u) {
                if (seq == s->rx_seq) {
                    ixn_skb_t *q = ixn_skb_clone(skb);
                    if (q) {
                        ixn_skb_pull(q, hlen);
                        ixn_skb_queue_push(&s->rx_queue, q);
                        s->rx_queue_len++;
                        s->last_activity = ixn_ticks_ms();
                    }
                    s->rx_seq += datalen;
                    s->tx_ack = s->rx_seq;
                    ixn_tcp_send_pkt(s, IXN_TCP_ACK, IXN_NULL, 0u);
                } else {
                    ixn_tcp_send_pkt(s, IXN_TCP_ACK, IXN_NULL, 0u);
                }
            }
            if (flags & IXN_TCP_FIN) {
                s->rx_seq += 1u;
                s->tx_ack = s->rx_seq;
                s->tcp_state = IXN_TCP_CLOSE_WAIT;
                ixn_tcp_send_pkt(s, IXN_TCP_ACK, IXN_NULL, 0u);
            }
            if (flags & IXN_TCP_RST) {
                s->tcp_state = IXN_TCP_CLOSED;
            }
            break;
        case IXN_TCP_FIN_WAIT_1:
            if (flags & IXN_TCP_ACK) s->tcp_state = IXN_TCP_FIN_WAIT_2;
            if (flags & IXN_TCP_FIN) {
                s->tcp_state = IXN_TCP_TIME_WAIT;
                ixn_tcp_send_pkt(s, IXN_TCP_ACK, IXN_NULL, 0u);
            }
            break;
        case IXN_TCP_FIN_WAIT_2:
            if (flags & IXN_TCP_FIN) {
                s->tcp_state = IXN_TCP_TIME_WAIT;
                ixn_tcp_send_pkt(s, IXN_TCP_ACK, IXN_NULL, 0u);
            }
            break;
        case IXN_TCP_LAST_ACK:
            if (flags & IXN_TCP_ACK) s->tcp_state = IXN_TCP_CLOSED;
            break;
        case IXN_TCP_CLOSING:
            if (flags & IXN_TCP_ACK) s->tcp_state = IXN_TCP_TIME_WAIT;
            break;
        default:
            break;
    }
    return 0;
}
