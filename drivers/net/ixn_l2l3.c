#include "ixn_l2l3.h"
#include "ixn_l4.h"
#include "ixn_compat.h"

static ixn_arp_entry_t g_ixn_arp_table[IXN_ARP_TABLE_SIZE];
static volatile ixn_irq_lock_t g_ixn_arp_lock = 0u;
static ixn_u32 g_ixn_ip_id_counter = 1u;
static ixn_u32 g_ixn_icmp_seq = 0u;

static ixn_ping_reply_t g_ixn_ping_queue[IXN_PING_QUEUE_SIZE];
static ixn_u32 g_ixn_ping_head = 0u;
static ixn_u32 g_ixn_ping_tail = 0u;
static volatile ixn_irq_lock_t g_ixn_ping_lock = 0u;

void ixn_l2l3_init(void) {
    ixn_memset(g_ixn_arp_table, 0, sizeof(g_ixn_arp_table));
    g_ixn_ip_id_counter = 1u;
    g_ixn_icmp_seq = 0u;
    ixn_ping_flush();
}

void ixn_ping_flush(void) {
    ixn_u64 flags = ixn_irq_lock_acquire(&g_ixn_ping_lock);
    g_ixn_ping_head = 0u;
    g_ixn_ping_tail = 0u;
    ixn_memset(g_ixn_ping_queue, 0, sizeof(g_ixn_ping_queue));
    ixn_irq_lock_release(&g_ixn_ping_lock, flags);
}

int ixn_ping_get_reply(ixn_ping_reply_t *out) {
    ixn_u64 flags;
    int rc = -1;
    if (!out) return -1;
    flags = ixn_irq_lock_acquire(&g_ixn_ping_lock);
    if (g_ixn_ping_head != g_ixn_ping_tail) {
        *out = g_ixn_ping_queue[g_ixn_ping_head];
        g_ixn_ping_head = (g_ixn_ping_head + 1u) % IXN_PING_QUEUE_SIZE;
        rc = 0;
    }
    ixn_irq_lock_release(&g_ixn_ping_lock, flags);
    return rc;
}

int ixn_ping_get_reply_match(ixn_u16 id, ixn_u32 seq, ixn_ping_reply_t *out) {
    ixn_u64 flags;
    int rc = -1;
    if (!out) return -1;
    flags = ixn_irq_lock_acquire(&g_ixn_ping_lock);
    if (g_ixn_ping_head != g_ixn_ping_tail) {
        ixn_u32 n = (g_ixn_ping_tail + IXN_PING_QUEUE_SIZE - g_ixn_ping_head) % IXN_PING_QUEUE_SIZE;
        ixn_u32 k, pos;
        for (k = 0u; k < n; k++) {
            ixn_u32 idx = (g_ixn_ping_head + k) % IXN_PING_QUEUE_SIZE;
            ixn_ping_reply_t *e = &g_ixn_ping_queue[idx];
            if (e->in_use && e->id == id && e->seq == seq) {
                *out = *e;
                /* Сдвигаем хвост очереди, затирая найденный слот: чужие
                 * ответы остаются ждать своих владельцев. */
                for (pos = k; pos + 1u < n; pos++) {
                    ixn_u32 cur = (g_ixn_ping_head + pos) % IXN_PING_QUEUE_SIZE;
                    ixn_u32 nxt = (g_ixn_ping_head + pos + 1u) % IXN_PING_QUEUE_SIZE;
                    g_ixn_ping_queue[cur] = g_ixn_ping_queue[nxt];
                }
                g_ixn_ping_tail = (g_ixn_ping_tail + IXN_PING_QUEUE_SIZE - 1u) % IXN_PING_QUEUE_SIZE;
                rc = 0;
                break;
            }
        }
    }
    ixn_irq_lock_release(&g_ixn_ping_lock, flags);
    return rc;
}

static void ixn_ping_push(ixn_u32 src_ip, ixn_u8 type, ixn_u8 code, ixn_u16 id_be,
                          ixn_u16 seq_be, ixn_u32 ttl, ixn_u32 size, ixn_u64 ts_ms) {
    ixn_u64 flags = ixn_irq_lock_acquire(&g_ixn_ping_lock);
    ixn_u32 next = (g_ixn_ping_tail + 1u) % IXN_PING_QUEUE_SIZE;
    ixn_ping_reply_t *r;
    if (next == g_ixn_ping_head) {
        /*
         * Очередь полна: выталкиваем самый старый ответ, а не молча
         * роняем самый свежий -- потеря только что пришедшего ответа
         * ping засчитал бы как потерю пакета.
         */
        g_ixn_ping_head = (g_ixn_ping_head + 1u) % IXN_PING_QUEUE_SIZE;
    }
    r = &g_ixn_ping_queue[g_ixn_ping_tail];
    ixn_memset(r, 0, sizeof(*r));
    r->in_use = 1u;
    r->code = code;
    r->src_ip = src_ip;
    r->id = (ixn_u16)IXN_ntohs(id_be);
    r->seq = (ixn_u32)IXN_ntohs(seq_be);
    r->ttl = ttl;
    r->size = size;
    r->ts_ms = ts_ms;
    g_ixn_ping_tail = next;
    (void)type;
    ixn_irq_lock_release(&g_ixn_ping_lock, flags);
}

ixn_u16 ixn_checksum16(const ixn_u8 *data, ixn_u32 len, ixn_u32 init) {
    ixn_u32 sum = init;
    ixn_u32 i;
    for (i = 0u; i + 1u < len; i += 2u) {
        sum += (ixn_u32)data[i] | ((ixn_u32)data[i + 1u] << 8u);
    }
    if (i < len) sum += (ixn_u32)data[i];
    while (sum >> 16u) sum = (sum & 0xFFFFu) + (sum >> 16u);
    return (ixn_u16)(~sum & 0xFFFFu);
}

ixn_u16 ixn_checksum16_combine(ixn_u16 c1, ixn_u16 c2, ixn_u32 len2) {
    ixn_u32 sum = (ixn_u32)c1 + (ixn_u32)c2;
    while (sum >> 16u) sum = (sum & 0xFFFFu) + (sum >> 16u);
    (void)len2;
    return (ixn_u16)(~sum & 0xFFFFu);
}

void ixn_eth_build_header(ixn_skb_t *skb, ixn_netdev_t *dev, const ixn_u8 *dst_mac, ixn_u16 eth_type) {
    ixn_eth_hdr_t *eth;
    ixn_u32 i;
    if (!skb || !dev) return;
    eth = (ixn_eth_hdr_t *)skb->data;
    for (i = 0u; i < IXN_ETH_ALEN; i++) eth->dst[i] = dst_mac ? dst_mac[i] : dev->broadcast[i];
    for (i = 0u; i < IXN_ETH_ALEN; i++) eth->src[i] = dev->dev_addr[i];
    eth->type = IXN_htons(eth_type);
    ixn_skb_set_mac_header(skb, (ixn_u32)(skb->data - skb->head));
}

int ixn_eth_tx(ixn_netdev_t *dev, ixn_skb_t *skb, const ixn_u8 *dst_mac, ixn_u16 eth_type) {
    if (!dev || !skb) return -1;
    ixn_skb_push(skb, IXN_ETH_HLEN);
    ixn_eth_build_header(skb, dev, dst_mac, eth_type);
    skb->protocol = eth_type;
    return ixn_netdev_xmit(dev, skb);
}

int ixn_eth_rx(ixn_netdev_t *dev, ixn_skb_t *skb) {
    ixn_eth_hdr_t *eth;
    ixn_u16 type;
    if (!dev || !skb || skb->len < IXN_ETH_HLEN) return -1;
    eth = (ixn_eth_hdr_t *)skb->data;
    ixn_skb_set_mac_header(skb, (ixn_u32)(skb->data - skb->head));
    ixn_skb_pull(skb, IXN_ETH_HLEN);
    ixn_skb_set_network_header(skb, (ixn_u32)(skb->data - skb->head));
    type = IXN_ntohs(eth->type);
    skb->protocol = type;
    if (type == IXN_ETH_TYPE_IPV4) {
        /*
         * Учим ARP по входящим IP-пакетам: кто прислал пакет, тому и
         * отвечаем без ARP-запроса.  Записи для чужих подсетей при
         * маршрутизации не используются -- для них next_hop всегда
         * шлюз, так что "отравить" кэш нельзя.
         */
        if (skb->len >= 20u) {
            const ixn_ipv4_hdr_t *ip = (const ixn_ipv4_hdr_t *)skb->data;
            ixn_u32 src = IXN_ntohl(ip->src); /* на проводе -- network order */
            if (src != 0u && src != 0xFFFFFFFFu && (src & 0xF0000000u) != 0xE0000000u)
                ixn_arp_update(dev, src, eth->src);
        }
        return ixn_ipv4_rx(dev, skb);
    }
    if (type == IXN_ETH_TYPE_ARP) return ixn_arp_rx(dev, skb);
    return 0;
}

void ixn_arp_update(ixn_netdev_t *dev, ixn_u32 ip, const ixn_u8 *mac) {
    ixn_u64 flags;
    ixn_u32 i, oldest = 0u;
    ixn_u64 oldest_ts = (ixn_u64)-1;
    if (ip == 0u) return;
    flags = ixn_irq_lock_acquire(&g_ixn_arp_lock);
    for (i = 0u; i < IXN_ARP_TABLE_SIZE; i++) {
        if (g_ixn_arp_table[i].state != IXN_ARP_STATE_FREE && g_ixn_arp_table[i].ip == ip) {
            ixn_u32 j;
            for (j = 0u; j < IXN_ETH_ALEN; j++) g_ixn_arp_table[i].mac[j] = mac[j];
            g_ixn_arp_table[i].dev = dev;
            g_ixn_arp_table[i].timestamp = ixn_ticks_ms();
            g_ixn_arp_table[i].state = IXN_ARP_STATE_RESOLVED;
            ixn_irq_lock_release(&g_ixn_arp_lock, flags);
            return;
        }
    }
    for (i = 0u; i < IXN_ARP_TABLE_SIZE; i++) {
        if (g_ixn_arp_table[i].state == IXN_ARP_STATE_FREE) {
            g_ixn_arp_table[i].ip = ip;
            ixn_u32 j;
            for (j = 0u; j < IXN_ETH_ALEN; j++) g_ixn_arp_table[i].mac[j] = mac[j];
            g_ixn_arp_table[i].dev = dev;
            g_ixn_arp_table[i].timestamp = ixn_ticks_ms();
            g_ixn_arp_table[i].state = IXN_ARP_STATE_RESOLVED;
            ixn_irq_lock_release(&g_ixn_arp_lock, flags);
            return;
        }
        if (g_ixn_arp_table[i].timestamp < oldest_ts) { oldest_ts = g_ixn_arp_table[i].timestamp; oldest = i; }
    }
    g_ixn_arp_table[oldest].ip = ip;
    { ixn_u32 j; for (j = 0u; j < IXN_ETH_ALEN; j++) g_ixn_arp_table[oldest].mac[j] = mac[j]; }
    g_ixn_arp_table[oldest].dev = dev;
    g_ixn_arp_table[oldest].timestamp = ixn_ticks_ms();
    g_ixn_arp_table[oldest].state = IXN_ARP_STATE_RESOLVED;
    ixn_irq_lock_release(&g_ixn_arp_lock, flags);
}

ixn_arp_entry_t *ixn_arp_lookup(ixn_u32 ip) {
    ixn_u64 flags;
    ixn_u32 i;
    ixn_u64 now = ixn_ticks_ms();
    flags = ixn_irq_lock_acquire(&g_ixn_arp_lock);
    for (i = 0u; i < IXN_ARP_TABLE_SIZE; i++) {
        if (g_ixn_arp_table[i].state == IXN_ARP_STATE_RESOLVED && g_ixn_arp_table[i].ip == ip) {
            if (now - g_ixn_arp_table[i].timestamp > IXN_ARP_CACHE_TTL_MS) {
                g_ixn_arp_table[i].state = IXN_ARP_STATE_STALE;
            }
            ixn_irq_lock_release(&g_ixn_arp_lock, flags);
            return &g_ixn_arp_table[i];
        }
    }
    ixn_irq_lock_release(&g_ixn_arp_lock, flags);
    return IXN_NULL;
}

void ixn_arp_flush(ixn_netdev_t *dev) {
    ixn_u64 flags;
    ixn_u32 i;
    flags = ixn_irq_lock_acquire(&g_ixn_arp_lock);
    for (i = 0u; i < IXN_ARP_TABLE_SIZE; i++) {
        if (g_ixn_arp_table[i].dev == dev) g_ixn_arp_table[i].state = IXN_ARP_STATE_FREE;
    }
    ixn_irq_lock_release(&g_ixn_arp_lock, flags);
}

/* Выбор следующего хопа: адрес в своей подсети -- напрямую,
 * иначе через шлюз (если он задан). */
static ixn_u32 ixn_route_next_hop(ixn_netdev_t *dev, ixn_u32 dst_ip) {
    if (dev->ipv4_addr != 0u && dev->ipv4_netmask != 0u &&
        (dst_ip & dev->ipv4_netmask) == (dev->ipv4_addr & dev->ipv4_netmask))
        return dst_ip;
    return dev->ipv4_gateway ? dev->ipv4_gateway : dst_ip;
}

int ixn_arp_resolve(ixn_netdev_t *dev, ixn_u32 ip, ixn_u8 *out_mac) {
    ixn_arp_entry_t *e;
    ixn_skb_t *skb;
    ixn_arp_pkt_t *arp;
    ixn_u32 i;
    if (!dev || !out_mac || ip == 0u) return -1;
    ip = ixn_route_next_hop(dev, ip);
    e = ixn_arp_lookup(ip);
    if (e) {
        for (i = 0u; i < IXN_ETH_ALEN; i++) out_mac[i] = e->mac[i];
        return 0;
    }
    /*
     * В кэше записи нет: отправляем ARP-запрос, но MAC ещё не известен.
     * out_mac НЕ заполняется -- раньше отсюда возвращали успех и
     * ipv4_tx уходил на неинициализированный MAC, теряя пакет.
     * Теперь вызывающий видит IXN_ARP_RESOLVE_SENT и не шлёт в никуда.
     */
    skb = ixn_skb_alloc(sizeof(ixn_arp_pkt_t) + IXN_ETH_HLEN);
    if (!skb) return -2;
    ixn_skb_reserve(skb, IXN_ETH_HLEN);
    ixn_skb_put(skb, sizeof(ixn_arp_pkt_t));
    arp = (ixn_arp_pkt_t *)skb->data;
    arp->htype = IXN_htons(IXN_ARP_HW_ETHER);
    arp->ptype = IXN_htons(IXN_ETH_TYPE_IPV4);
    arp->hlen = IXN_ETH_ALEN;
    arp->plen = IXN_IPV4_ALEN;
    arp->oper = IXN_htons(IXN_ARP_OP_REQUEST);
    for (i = 0u; i < IXN_ETH_ALEN; i++) arp->sha[i] = dev->dev_addr[i];
    ixn_be32_store(arp->spa, dev->ipv4_addr);
    for (i = 0u; i < IXN_ETH_ALEN; i++) arp->tha[i] = 0u;
    ixn_be32_store(arp->tpa, ip);
    {
        ixn_u8 bcast[IXN_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        if (ixn_eth_tx(dev, skb, bcast, IXN_ETH_TYPE_ARP) != 0) return -2;
    }
    return IXN_ARP_RESOLVE_SENT;
}

/*
 * Гарантированная ARP-разрешённость перед отправкой: если записи в кэше
 * нет -- крутит poll и повторяет ARP-запрос, пока MAC не придёт или не
 * истечёт таймаут.  Используется ping'ом, чтобы первый echo-request
 * после старта не терялся.
 */
int ixn_arp_ensure(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u32 timeout_ms) {
    ixn_u8 mac[IXN_ETH_ALEN];
    ixn_u64 deadline, last_req;
    ixn_u32 next_hop;
    if (!dev) return -1;
    if (dev->ipv4_addr != 0u && dst_ip == dev->ipv4_addr) return 0;
    next_hop = ixn_route_next_hop(dev, dst_ip);
    if (ixn_arp_resolve(dev, next_hop, mac) == 0) return 0;
    deadline = ixn_ticks_ms() + (ixn_u64)(timeout_ms ? timeout_ms : 200u);
    last_req = ixn_ticks_ms();
    for (;;) {
        ixn_netdev_poll_all(64u);
        if (ixn_arp_lookup(next_hop)) return 0;
        if (ixn_ticks_ms() >= deadline) return -2;
        if (ixn_ticks_ms() - last_req >= 50u) {
            last_req = ixn_ticks_ms();
            (void)ixn_arp_resolve(dev, next_hop, mac); /* повторный запрос */
        }
        /*
         * Планировщик кооперативный: без уступания кванта этот цикл
         * замораживал бы всю систему на время ожидания ARP.
         */
        if (ixn_yield_hook) ixn_yield_hook();
    }
}

int ixn_arp_rx(ixn_netdev_t *dev, ixn_skb_t *skb) {
    ixn_arp_pkt_t *arp;
    ixn_u32 spa, tpa;
    if (!dev || !skb || skb->len < sizeof(ixn_arp_pkt_t)) return -1;
    arp = (ixn_arp_pkt_t *)skb->data;
    if (IXN_ntohs(arp->htype) != IXN_ARP_HW_ETHER) return -2;
    if (IXN_ntohs(arp->ptype) != IXN_ETH_TYPE_IPV4) return -3;
    spa = ixn_be32_load(arp->spa);
    tpa = ixn_be32_load(arp->tpa);
    ixn_arp_update(dev, spa, arp->sha);
    if (IXN_ntohs(arp->oper) == IXN_ARP_OP_REQUEST && tpa == dev->ipv4_addr) {
        ixn_skb_t *reply = ixn_skb_alloc(sizeof(ixn_arp_pkt_t) + IXN_ETH_HLEN);
        ixn_arp_pkt_t *r;
        ixn_u32 i;
        if (!reply) return -4;
        ixn_skb_reserve(reply, IXN_ETH_HLEN);
        ixn_skb_put(reply, sizeof(ixn_arp_pkt_t));
        r = (ixn_arp_pkt_t *)reply->data;
        r->htype = IXN_htons(IXN_ARP_HW_ETHER);
        r->ptype = IXN_htons(IXN_ETH_TYPE_IPV4);
        r->hlen = IXN_ETH_ALEN;
        r->plen = IXN_IPV4_ALEN;
        r->oper = IXN_htons(IXN_ARP_OP_REPLY);
        for (i = 0u; i < IXN_ETH_ALEN; i++) r->sha[i] = dev->dev_addr[i];
        ixn_be32_store(r->spa, dev->ipv4_addr);
        for (i = 0u; i < IXN_ETH_ALEN; i++) r->tha[i] = arp->sha[i];
        ixn_be32_store(r->tpa, spa);
        return ixn_eth_tx(dev, reply, arp->sha, IXN_ETH_TYPE_ARP);
    }
    return 0;
}

ixn_ipv4_hdr_t *ixn_ipv4_hdr(ixn_skb_t *skb) {
    if (!skb) return IXN_NULL;
    return (ixn_ipv4_hdr_t *)ixn_skb_network_header(skb);
}

int ixn_is_local_ip(ixn_netdev_t *dev, ixn_u32 dst_ip) {
    if (!dev) return 0;
    if (dst_ip == dev->ipv4_addr && dev->ipv4_addr != 0u) return 1;
    /* 127/8 — всегда петлевой диапазон, адреса интерфейса там нет */
    if ((dst_ip & 0xFF000000u) == 0x7F000000u) return 1;
    return 0;
}

static void ixn_ipv4_build_hdr(ixn_netdev_t *dev, ixn_skb_t *skb, ixn_u32 dst_ip, ixn_u8 protocol) {
    ixn_ipv4_hdr_t *iph;
    ixn_skb_push(skb, IXN_IPV4_IHL_MIN * 4u);
    iph = (ixn_ipv4_hdr_t *)skb->data;
    iph->ver_ihl = (IXN_IPV4_VERSION << 4u) | IXN_IPV4_IHL_MIN;
    iph->tos = 0u;
    iph->tot_len = IXN_htons((ixn_u16)skb->len);
    iph->id = IXN_htons((ixn_u16)ixn_atomic_inc32(&g_ixn_ip_id_counter));
    iph->flags_frag = IXN_htons(IXN_IPV4_FLAG_DF);
    iph->ttl = IXN_IPV4_TTL_DEFAULT;
    iph->protocol = protocol;
    iph->checksum = 0u;
    /*
     * Внутри стека IP-адреса хранятся в host order (a.b.c.d == 0xAABBCCDD),
     * а на проводе должен быть network order.  Раньше u32 писался в
     * заголовок как есть, и на little-endian байты адреса уходили в
     * обратную сторону: 10.0.2.15 превращался в 15.2.0.10.  Внешние
     * стеки такие пакеты молча выбрасывали -- отсюда потеря ping.
     */
    iph->src = IXN_htonl(dev->ipv4_addr);
    iph->dst = IXN_htonl(dst_ip);
    iph->checksum = ixn_checksum16((const ixn_u8 *)iph, IXN_IPV4_IHL_MIN * 4u, 0u);
    ixn_skb_set_network_header(skb, (ixn_u32)(skb->data - skb->head));
}

int ixn_ipv4_tx(ixn_netdev_t *dev, ixn_skb_t *skb, ixn_u32 dst_ip, ixn_u8 protocol) {
    ixn_u8 dst_mac[IXN_ETH_ALEN];
    ixn_u32 i;
    int rc;
    if (!dev || !skb) return -1;

    if (dst_ip == 0xFFFFFFFFu || (dst_ip & 0xF0000000u) == 0xE0000000u) {
        for (i = 0u; i < IXN_ETH_ALEN; i++) dst_mac[i] = 0xFFu;
    } else if (ixn_is_local_ip(dev, dst_ip)) {
        /*
         * Локальная доставка: адрес свой или петлевой диапазон 127/8.
         * Пакет не уходит в драйвер, а сразу разбирается стеком приёма —
         * так работает loopback, даже если отдельного "lo" нет, и ping
         * собственного адреса не зависит от PHY.  Владение skb такое же,
         * как у драйверов: rx-пути skb не освобождают, освобождаем здесь.
         * Раньше здесь делали ixn_skb_clone и отдавали клон в eth_rx без
         * освобождения -- один клон утекал на каждый loopback-пакет, пул
         * из 256 skb кончался через ~128 пингов, и ping начинал терять
         * 100% пакетов (send_echo не мог выделить буфер).
         */
        ixn_ipv4_build_hdr(dev, skb, dst_ip, protocol);
        rc = ixn_ipv4_rx(dev, skb);
        ixn_skb_free(skb);
        return rc;
    } else {
        if (ixn_arp_resolve(dev, ixn_route_next_hop(dev, dst_ip), dst_mac) != 0) {
            /*
             * MAC не разрешён (в т.ч. только что отправлен ARP-запрос):
             * с неинициализированным dst_mac пакет на проводе
             * гарантированно теряется -- дешевле сбросить и вернуться
             * с ошибкой -3.  Отправитель (ping) дождётся ответа ARP
             * и повторит запрос.
             */
            ixn_skb_free(skb);
            return -3;
        }
    }
    ixn_ipv4_build_hdr(dev, skb, dst_ip, protocol);
    return ixn_eth_tx(dev, skb, dst_mac, IXN_ETH_TYPE_IPV4);
}

int ixn_ipv4_rx(ixn_netdev_t *dev, ixn_skb_t *skb) {
    ixn_ipv4_hdr_t *iph;
    ixn_u32 hlen;
    if (!dev || !skb || skb->len < 20u) return -1;
    iph = (ixn_ipv4_hdr_t *)skb->data;
    if (ixn_ipv4_version(iph) != IXN_IPV4_VERSION) return -2;
    hlen = ixn_ipv4_hdr_len(iph);
    if (hlen < 20u || skb->len < hlen) return -3;
    {
        ixn_u16 ck = ixn_checksum16((const ixn_u8 *)iph, hlen, 0u);
        if (ck != 0u) return -4;
    }
    /*
     * Нормализуем адреса к host order (как их хранит весь стек --
     * dev->ipv4_addr, сокеты, ARP).  Строго ПОСЛЕ проверки checksum:
     * та считается по исходным байтам пакета.
     */
    iph->src = IXN_ntohl(iph->src);
    iph->dst = IXN_ntohl(iph->dst);
    if (iph->dst != dev->ipv4_addr && iph->dst != 0xFFFFFFFFu &&
        (iph->dst & 0xF0000000u) != 0xE0000000u) {
        if (dev->type != IXN_NETDEV_TYPE_LOOPBACK) return 0;
    }
    ixn_skb_set_transport_header(skb, (ixn_u32)(skb->data - skb->head) + hlen);
    ixn_skb_pull(skb, hlen);
    if (iph->protocol == IXN_IP_PROTO_ICMP) return ixn_icmp_rx(dev, skb);
    if (iph->protocol == IXN_IP_PROTO_UDP) return ixn_udp_rx(dev, skb, iph);
    if (iph->protocol == IXN_IP_PROTO_TCP) return ixn_tcp_rx(dev, skb, iph);
    return 0;
}

int ixn_icmp_send(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u8 type, ixn_u8 code, const ixn_u8 *payload, ixn_u32 payload_len) {
    ixn_skb_t *skb;
    ixn_icmp_hdr_t *icmp;
    if (!dev) return -1;
    skb = ixn_skb_alloc(payload_len + sizeof(ixn_icmp_hdr_t) + 20u);
    if (!skb) return -2;
    ixn_skb_reserve(skb, 20u);
    ixn_skb_put(skb, sizeof(ixn_icmp_hdr_t));
    icmp = (ixn_icmp_hdr_t *)skb->data;
    icmp->type = type;
    icmp->code = code;
    icmp->checksum = 0u;
    icmp->id = 0u;
    icmp->seq = 0u;
    if (payload && payload_len) {
        ixn_skb_put(skb, payload_len);
        ixn_memcpy(skb->data + sizeof(ixn_icmp_hdr_t), payload, payload_len);
    }
    icmp->checksum = ixn_checksum16(skb->data, skb->len, 0u);
    return ixn_ipv4_tx(dev, skb, dst_ip, IXN_IP_PROTO_ICMP);
}

int ixn_icmp_send_echo(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u16 id, ixn_u16 seq, const ixn_u8 *data, ixn_u32 data_len) {
    ixn_skb_t *skb;
    ixn_icmp_hdr_t *icmp;
    if (!dev) return -1;
    skb = ixn_skb_alloc(data_len + sizeof(ixn_icmp_hdr_t) + 20u);
    if (!skb) return -2;
    ixn_skb_reserve(skb, 20u);
    ixn_skb_put(skb, sizeof(ixn_icmp_hdr_t));
    icmp = (ixn_icmp_hdr_t *)skb->data;
    icmp->type = IXN_ICMP_ECHO_REQUEST;
    icmp->code = 0u;
    icmp->checksum = 0u;
    icmp->id = IXN_htons(id);
    icmp->seq = IXN_htons(seq);
    if (data && data_len) {
        ixn_skb_put(skb, data_len);
        ixn_memcpy(skb->data + sizeof(ixn_icmp_hdr_t), data, data_len);
    }
    icmp->checksum = ixn_checksum16(skb->data, skb->len, 0u);
    return ixn_ipv4_tx(dev, skb, dst_ip, IXN_IP_PROTO_ICMP);
}

int ixn_icmp_rx(ixn_netdev_t *dev, ixn_skb_t *skb) {
    ixn_icmp_hdr_t *icmp;
    if (!dev || !skb || skb->len < sizeof(ixn_icmp_hdr_t)) return -1;
    icmp = (ixn_icmp_hdr_t *)skb->data;
    if (icmp->type == IXN_ICMP_ECHO_REQUEST) {
        ixn_ipv4_hdr_t *orig_ip = (ixn_ipv4_hdr_t *)ixn_skb_network_header(skb);
        ixn_u32 src = orig_ip ? orig_ip->src : 0u;
        ixn_u16 id = icmp->id;
        ixn_u16 seq = icmp->seq;
        ixn_u32 plen = skb->len - sizeof(ixn_icmp_hdr_t);
        const ixn_u8 *payload = skb->data + sizeof(ixn_icmp_hdr_t);
        ixn_skb_t *reply = ixn_skb_alloc(plen + sizeof(ixn_icmp_hdr_t) + 20u);
        ixn_icmp_hdr_t *r;
        if (!reply) return -2;
        ixn_skb_reserve(reply, 20u);
        ixn_skb_put(reply, sizeof(ixn_icmp_hdr_t) + plen);
        r = (ixn_icmp_hdr_t *)reply->data;
        r->type = IXN_ICMP_ECHO_REPLY;
        r->code = 0u;
        r->checksum = 0u;
        r->id = id;
        r->seq = seq;
        if (plen) ixn_memcpy(reply->data + sizeof(ixn_icmp_hdr_t), payload, plen);
        r->checksum = ixn_checksum16(reply->data, reply->len, 0u);
        return ixn_ipv4_tx(dev, reply, src, IXN_IP_PROTO_ICMP);
    }
    if (icmp->type == IXN_ICMP_ECHO_REPLY) {
        ixn_ipv4_hdr_t *orig_ip = (ixn_ipv4_hdr_t *)ixn_skb_network_header(skb);
        ixn_ping_push(orig_ip ? orig_ip->src : 0u, icmp->type, icmp->code,
                      icmp->id, icmp->seq, orig_ip ? (ixn_u32)orig_ip->ttl : 0u,
                      skb->len, ixn_ticks_ms());
        return 0;
    }
    if (icmp->type == IXN_ICMP_DEST_UNREACH || icmp->type == IXN_ICMP_TIME_EXCEED) {
        ixn_ipv4_hdr_t *orig_ip = (ixn_ipv4_hdr_t *)ixn_skb_network_header(skb);
        ixn_ping_push(orig_ip ? orig_ip->src : 0u, icmp->type, icmp->code,
                      0u, 0u, orig_ip ? (ixn_u32)orig_ip->ttl : 0u, skb->len, ixn_ticks_ms());
        return 0;
    }
    return 0;
}

ixn_u32 ixn_ipv4_parse_str(const char *s) {
    ixn_u32 ip = 0u, v = 0u, i;
    if (!s) return 0u;
    for (i = 0u; s[i]; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            v = v * 10u + (ixn_u32)(s[i] - '0');
            if (v > 255u) return 0u;
        } else if (s[i] == '.') {
            ip = (ip << 8u) | v;
            v = 0u;
        } else return 0u;
    }
    ip = (ip << 8u) | v;
    return ip; /* host order: a.b.c.d == 0xAABBCCDD, как весь ixn-стек */
}

int ixn_ipv4_format_str(ixn_u32 ip, char *out, ixn_u32 cap) {
    ixn_u32 a = (ip >> 24u) & 0xFFu, b = (ip >> 16u) & 0xFFu, c = (ip >> 8u) & 0xFFu, d = ip & 0xFFu;
    ixn_u32 n = 0u;
    char tmp[16];
    if (!out || cap < 16u) return -1;
    {
        const char *hex = "0123456789";
        ixn_u32 vals[4] = {a, b, c, d};
        ixn_u32 k;
        for (k = 0u; k < 4u; k++) {
            ixn_u32 val = vals[k], idx = 0u;
            if (val == 0u) { tmp[idx++] = '0'; }
            else { char buf[4]; ixn_u32 bi = 0u; while (val) { buf[bi++] = hex[val % 10u]; val /= 10u; } while (bi) tmp[idx++] = buf[--bi]; }
            if (k < 3u) tmp[idx++] = '.';
            ixn_u32 j; for (j = 0u; j < idx && n + 1u < cap; j++) out[n++] = tmp[j];
        }
    }
    out[n] = 0;
    return 0;
}
