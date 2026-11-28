#include "ixn_dhcp_dns.h"
#include "ixn_l2l3.h"
#include "ixn_l4.h"
#include "ixn_compat.h"

static ixn_u32 g_ixn_dns_server = 0u;
static ixn_dhcp_client_t *g_ixn_dhcp_hook = 0;

void ixn_dhcp_set_hook(ixn_dhcp_client_t *c) { g_ixn_dhcp_hook = c; }
ixn_dhcp_client_t *ixn_dhcp_get_hook(void) { return g_ixn_dhcp_hook; }
static ixn_u32 g_ixn_dns_xid = 0xABCDEF01u;

void ixn_dhcp_init(ixn_dhcp_client_t *c, ixn_netdev_t *dev) {
    if (!c) return;
    ixn_memset(c, 0, sizeof(*c));
    c->dev = dev;
    c->xid = IXN_DHCP_XID_DEFAULT;
    c->state = IXN_DHCP_STATE_INIT;
    c->lease_time = 86400u;
}

static ixn_u32 ixn_dhcp_find_opt(const ixn_u8 *opts, ixn_u32 len, ixn_u8 type, ixn_u8 *out_val, ixn_u32 cap) {
    ixn_u32 i = 0u;
    while (i + 1u < len) {
        ixn_u8 t = opts[i++];
        if (t == IXN_DHCP_OPT_END) break;
        if (t == IXN_DHCP_OPT_PAD) continue;
        if (i >= len) break;
        ixn_u8 l = opts[i++];
        if (i + l > len) break;
        if (t == type) {
            ixn_u32 copy = (ixn_u32)l < cap ? (ixn_u32)l : cap;
            ixn_u32 k;
            for (k = 0u; k < copy; k++) out_val[k] = opts[i + k];
            return copy;
        }
        i += l;
    }
    return 0u;
}

static void ixn_dhcp_put_ip(ixn_u8 *p, ixn_u32 ip) {
    p[0] = (ixn_u8)(ip >> 24u);
    p[1] = (ixn_u8)(ip >> 16u);
    p[2] = (ixn_u8)(ip >> 8u);
    p[3] = (ixn_u8)ip;
}

static ixn_u32 ixn_dhcp_get_ip(const ixn_u8 *p) {
    return ((ixn_u32)p[0] << 24u) | ((ixn_u32)p[1] << 16u) |
           ((ixn_u32)p[2] << 8u) | (ixn_u32)p[3];
}

static int ixn_dhcp_build(ixn_dhcp_client_t *c, ixn_u8 msg_type, ixn_u8 *buf, ixn_u32 cap) {
    ixn_dhcp_hdr_t *h;
    ixn_u8 *opts;
    ixn_u32 i;
    if (!c || !buf || cap < sizeof(ixn_dhcp_hdr_t) + 32u) return -1;
    ixn_memset(buf, 0, cap);
    h = (ixn_dhcp_hdr_t *)buf;
    h->op = IXN_DHCP_OP_BOOTREQUEST;
    h->htype = 1u;
    h->hlen = IXN_ETH_ALEN;
    h->xid = c->xid;
    h->flags = IXN_htons(0x8000u);
    h->ciaddr = c->dev ? c->dev->ipv4_addr : 0u;
    if (c->dev) for (i = 0u; i < IXN_ETH_ALEN; i++) h->chaddr[i] = c->dev->dev_addr[i];
    h->magic = IXN_htonl(IXN_DHCP_MAGIC_COOKIE);
    opts = buf + sizeof(ixn_dhcp_hdr_t);
    opts[0] = IXN_DHCP_OPT_MSG_TYPE;
    opts[1] = 1u;
    opts[2] = msg_type;
    {
        ixn_u32 o = 3u;
        if (msg_type == IXN_DHCP_MSG_REQUEST && c->offered_ip) {
            opts[o++] = IXN_DHCP_OPT_REQUESTED_IP;
            opts[o++] = 4u;
            ixn_dhcp_put_ip(opts + o, c->offered_ip);
            o += 4u;
        }
        if (msg_type == IXN_DHCP_MSG_REQUEST && c->server_id) {
            opts[o++] = IXN_DHCP_OPT_SERVER_ID;
            opts[o++] = 4u;
            ixn_dhcp_put_ip(opts + o, c->server_id);
            o += 4u;
        }
        opts[o++] = IXN_DHCP_OPT_PARAM_LIST;
        opts[o++] = 4u;
        opts[o++] = IXN_DHCP_OPT_SUBNET_MASK;
        opts[o++] = IXN_DHCP_OPT_ROUTER;
        opts[o++] = IXN_DHCP_OPT_DNS_SERVER;
        opts[o++] = IXN_DHCP_OPT_LEASE_TIME;
        opts[o++] = IXN_DHCP_OPT_END;
        return (int)(sizeof(ixn_dhcp_hdr_t) + o);
    }
}

int ixn_dhcp_discover(ixn_dhcp_client_t *c) {
    ixn_u8 buf[512];
    int len;
    ixn_netdev_t *dev;
    if (!c || !c->dev) return -1;
    dev = c->dev;
    len = ixn_dhcp_build(c, IXN_DHCP_MSG_DISCOVER, buf, sizeof(buf));
    if (len < 0) return -2;
    c->state = IXN_DHCP_STATE_SELECTING;
    c->last_tick = ixn_ticks_ms();
    return ixn_udp_send(dev, 0u, IXN_DHCP_CLIENT_PORT, 0xFFFFFFFFu, IXN_DHCP_SERVER_PORT, buf, (ixn_u32)len);
}

int ixn_dhcp_request(ixn_dhcp_client_t *c) {
    ixn_u8 buf[512];
    int len;
    ixn_netdev_t *dev;
    if (!c || !c->dev) return -1;
    dev = c->dev;
    len = ixn_dhcp_build(c, IXN_DHCP_MSG_REQUEST, buf, sizeof(buf));
    if (len < 0) return -2;
    c->state = IXN_DHCP_STATE_REQUESTING;
    c->last_tick = ixn_ticks_ms();
    return ixn_udp_send(dev, 0u, IXN_DHCP_CLIENT_PORT, 0xFFFFFFFFu, IXN_DHCP_SERVER_PORT, buf, (ixn_u32)len);
}

int ixn_dhcp_rx(ixn_dhcp_client_t *c, ixn_netdev_t *dev, const ixn_u8 *data, ixn_u32 len) {
    ixn_dhcp_hdr_t *h;
    const ixn_u8 *opts;
    ixn_u32 opts_len;
    ixn_u8 msg_type = 0u;
    ixn_u8 val[8];
    ixn_u32 vlen;
    if (!c || !data || len < sizeof(ixn_dhcp_hdr_t)) return -1;
    h = (ixn_dhcp_hdr_t *)data;
    if (h->xid != c->xid) return -2;
    if (h->magic != IXN_htonl(IXN_DHCP_MAGIC_COOKIE)) return -3;
    opts = data + sizeof(ixn_dhcp_hdr_t);
    opts_len = len - sizeof(ixn_dhcp_hdr_t);
    vlen = ixn_dhcp_find_opt(opts, opts_len, IXN_DHCP_OPT_MSG_TYPE, val, sizeof(val));
    if (vlen < 1u) return -4;
    msg_type = val[0];
    if (msg_type == IXN_DHCP_MSG_OFFER && c->state == IXN_DHCP_STATE_SELECTING) {
        c->offered_ip = ixn_dhcp_get_ip((const ixn_u8 *)h + 16u);
        vlen = ixn_dhcp_find_opt(opts, opts_len, IXN_DHCP_OPT_SERVER_ID, val, sizeof(val));
        if (vlen == 4u) c->server_id = ixn_dhcp_get_ip(val);
        return ixn_dhcp_request(c);
    }
    if (msg_type == IXN_DHCP_MSG_ACK && c->state == IXN_DHCP_STATE_REQUESTING) {
        c->offered_ip = ixn_dhcp_get_ip((const ixn_u8 *)h + 16u);
        vlen = ixn_dhcp_find_opt(opts, opts_len, IXN_DHCP_OPT_SUBNET_MASK, val, sizeof(val));
        c->subnet_mask = (vlen == 4u) ? ixn_dhcp_get_ip(val) : 0xFFFFFF00u;
        vlen = ixn_dhcp_find_opt(opts, opts_len, IXN_DHCP_OPT_ROUTER, val, sizeof(val));
        c->router = (vlen == 4u) ? ixn_dhcp_get_ip(val) : 0u;
        vlen = ixn_dhcp_find_opt(opts, opts_len, IXN_DHCP_OPT_DNS_SERVER, val, sizeof(val));
        c->dns_server = (vlen == 4u) ? ixn_dhcp_get_ip(val) : 0u;
        vlen = ixn_dhcp_find_opt(opts, opts_len, IXN_DHCP_OPT_LEASE_TIME, val, sizeof(val));
        c->lease_time = (vlen == 4u) ? ixn_be32_load(val) : 86400u;
        if (dev) {
            ixn_netdev_set_ipv4(dev, c->offered_ip, c->subnet_mask, c->router);
            g_ixn_dns_server = c->dns_server;
        }
        c->state = IXN_DHCP_STATE_BOUND;
        c->last_tick = ixn_ticks_ms();
        return 0;
    }
    if (msg_type == IXN_DHCP_MSG_NAK) {
        c->state = IXN_DHCP_STATE_INIT;
        c->offered_ip = 0u;
    }
    return 0;
}

int ixn_dhcp_run(ixn_dhcp_client_t *c) {
    if (!c) return -1;
    if (c->state == IXN_DHCP_STATE_INIT) return ixn_dhcp_discover(c);
    return 0;
}

int ixn_dhcp_release(ixn_dhcp_client_t *c) {
    ixn_u8 buf[512];
    int len;
    if (!c || !c->dev) return -1;
    len = ixn_dhcp_build(c, IXN_DHCP_MSG_RELEASE, buf, sizeof(buf));
    if (len < 0) return -2;
    return ixn_udp_send(c->dev, c->offered_ip, IXN_DHCP_CLIENT_PORT, c->server_id, IXN_DHCP_SERVER_PORT, buf, (ixn_u32)len);
}

void ixn_dhcp_tick(ixn_dhcp_client_t *c) {
    ixn_u64 now;
    if (!c) return;
    now = ixn_ticks_ms();
    if (c->state == IXN_DHCP_STATE_SELECTING && (now - c->last_tick) > 5000u) {
        ixn_dhcp_discover(c);
    } else if (c->state == IXN_DHCP_STATE_REQUESTING && (now - c->last_tick) > 5000u) {
        ixn_dhcp_request(c);
    } else if (c->state == IXN_DHCP_STATE_BOUND && c->lease_time > 0u) {
        ixn_u64 elapsed = (now - c->last_tick) / 1000u;
        if (elapsed > (c->lease_time / 2u)) {
            c->state = IXN_DHCP_STATE_RENEWING;
            ixn_dhcp_request(c);
        }
    }
}

ixn_u32 ixn_dns_get_server(ixn_netdev_t *dev) {
    (void)dev;
    return g_ixn_dns_server;
}

static ixn_u32 ixn_dns_encode_name(const char *name, ixn_u8 *buf, ixn_u32 cap) {
    ixn_u32 i = 0u, label_start = 0u, pos = 0u;
    if (!name || !buf) return 0u;
    while (name[i] && pos + 1u < cap) {
        if (name[i] == '.') {
            ixn_u32 llen = i - label_start;
            if (llen > 63u || pos + 1u + llen >= cap) return 0u;
            buf[pos++] = (ixn_u8)llen;
            ixn_u32 j;
            for (j = 0u; j < llen; j++) buf[pos++] = name[label_start + j];
            label_start = i + 1u;
        }
        i++;
    }
    if (i > label_start) {
        ixn_u32 llen = i - label_start;
        if (llen > 63u || pos + 1u + llen >= cap) return 0u;
        buf[pos++] = (ixn_u8)llen;
        ixn_u32 j;
        for (j = 0u; j < llen; j++) buf[pos++] = name[label_start + j];
    }
    if (pos < cap) buf[pos++] = 0u;
    return pos;
}

static ixn_u32 ixn_dns_skip_name(const ixn_u8 *data, ixn_u32 len, ixn_u32 pos) {
    while (pos < len) {
        ixn_u8 l = data[pos];
        if ((l & 0xC0u) == 0xC0u) return pos + 2u;
        if (l == 0u) return pos + 1u;
        pos += 1u + l;
    }
    return pos;
}

int ixn_dns_resolve(ixn_netdev_t *dev, ixn_u32 dns_server, const char *name, ixn_u32 *out_ip) {
    ixn_u8 buf[512];
    ixn_dns_hdr_t *h;
    ixn_u32 off, nlen;
    ixn_u16 xid;
    ixn_socket_t *s;
    ixn_u32 rsp_ip;
    ixn_u16 rsp_port;
    int rlen;
    ixn_u64 deadline;
    if (!dev || !name || !out_ip || dns_server == 0u) return -1;
    xid = (ixn_u16)(ixn_atomic_inc32(&g_ixn_dns_xid) & 0xFFFFu);
    ixn_memset(buf, 0, sizeof(buf));
    h = (ixn_dns_hdr_t *)buf;
    h->id = IXN_htons(xid);
    h->flags = IXN_htons(0x0100u);
    h->qdcount = IXN_htons(1u);
    off = sizeof(ixn_dns_hdr_t);
    nlen = ixn_dns_encode_name(name, buf + off, sizeof(buf) - off - 4u);
    if (nlen == 0u) return -2;
    off += nlen;
    ixn_be16_store(buf + off, IXN_DNS_TYPE_A);
    off += 2u;
    ixn_be16_store(buf + off, IXN_DNS_CLASS_IN);
    off += 2u;
    s = ixn_socket_alloc(IXN_SOCK_TYPE_UDP);
    if (!s) return -3;
    s->dev = dev;
    s->local_ip = dev->ipv4_addr;
    /*
     * local_port хранится в host order, как у всех сокетов стека:
     * ixn_udp_find() сравнивает его с IXN_ntohs(uh->dst).  Раньше здесь
     * лежал IXN_htons(...), и ответ DNS не находил свой сокет.
     */
    s->local_port = (ixn_u16)(10000u + (xid & 0xFFu));
    /*
     * Без MAC сервера ixn_ipv4_tx выбрасывает датаграмму, а не ждёт
     * ARP, поэтому первый запрос после старта терялся.
     */
    if (!ixn_netdev_is_loopback(dev))
        (void)ixn_arp_ensure(dev, dns_server, 500u);
    {
        int rc = ixn_udp_send(dev, dev->ipv4_addr, s->local_port, dns_server, IXN_DNS_PORT, buf, off);
        if (rc != 0) { ixn_socket_free(s); return -4; }
    }
    /*
     * Ждём по настоящему времени (а не по счётчику итераций): ответ
     * через интернет идёт десятки миллисекунд.  Планировщик
     * кооперативный, поэтому каждый виток уступает квант.
     */
    deadline = ixn_ticks_ms() + 3000u;
    for (;;) {
        rlen = ixn_udp_socket_recv(s, &rsp_ip, &rsp_port, buf, sizeof(buf));
        if (rlen > 0) break;
        if (ixn_ticks_ms() >= deadline) break;
        ixn_netdev_poll_all(IXN_NETDEV_RX_BUDGET);
        if (ixn_yield_hook) ixn_yield_hook();
        else ixn_pause();
    }
    if (rlen <= 0) { ixn_socket_free(s); return -5; }
    ixn_socket_free(s);
    if ((ixn_u32)rlen < sizeof(ixn_dns_hdr_t)) return -6;
    h = (ixn_dns_hdr_t *)buf;
    if (IXN_ntohs(h->id) != xid) return -7;
    if (IXN_ntohs(h->ancount) == 0u) return -8;
    off = sizeof(ixn_dns_hdr_t);
    off = ixn_dns_skip_name(buf, rlen, off);
    off += 4u;
    {
        ixn_u32 a;
        for (a = 0u; a < IXN_ntohs(h->ancount) && off + 12u <= (ixn_u32)rlen; a++) {
            ixn_u16 type, cls;
            ixn_u16 rdlen;
            off = ixn_dns_skip_name(buf, rlen, off);
            if (off + 10u > (ixn_u32)rlen) break;
            type = ixn_be16_load(buf + off); off += 2u;
            cls = ixn_be16_load(buf + off); off += 2u;
            off += 4u;
            rdlen = ixn_be16_load(buf + off); off += 2u;
            if (type == IXN_DNS_TYPE_A && rdlen == 4u && off + 4u <= (ixn_u32)rlen) {
                *out_ip = ixn_be32_load(buf + off);
                return 0;
            }
            off += rdlen;
            (void)cls;
        }
    }
    return -9;
}

int ixn_dns_resolve_default(const char *name, ixn_u32 *out_ip) {
    ixn_netdev_t *dev = ixn_netdev_get_first_up();
    ixn_u32 dns = g_ixn_dns_server;
    if (!dev) return -1;
    if (dns == 0u) dns = 0x08080808u;
    return ixn_dns_resolve(dev, dns, name, out_ip);
}
