#include "ixn_netdev.h"
#include "ixn_skb.h"
#include "ixn_compat.h"

static ixn_netdev_t *g_ixn_netdevs[IXN_NETDEV_MAX];
static ixn_u32 g_ixn_netdev_count = 0u;
static ixn_u32 g_ixn_netdev_next_index = 1u;
static ixn_netdev_t *g_ixn_default_dev = IXN_NULL;
static volatile ixn_irq_lock_t g_ixn_netdev_lock = 0u;

void ixn_netdev_init(void) {
    ixn_memset(g_ixn_netdevs, 0, sizeof(g_ixn_netdevs));
    g_ixn_netdev_count = 0u;
    g_ixn_netdev_next_index = 1u;
    g_ixn_default_dev = IXN_NULL;
}

ixn_netdev_t *ixn_netdev_alloc(const char *name, ixn_u32 type) {
    ixn_netdev_t *dev;
    ixn_u32 i;
    if (g_ixn_netdev_count >= IXN_NETDEV_MAX) return IXN_NULL;
    dev = (ixn_netdev_t *)ixn_alloc_zero(sizeof(ixn_netdev_t));
    if (!dev) return IXN_NULL;
    if (name) {
        for (i = 0u; i + 1u < IXN_NETDEV_NAME_LEN && name[i]; i++) dev->name[i] = name[i];
        dev->name[i] = 0;
    } else {
        const char *def = "ethN";
        for (i = 0u; i < 5u; i++) dev->name[i] = def[i];
    }
    dev->type = type;
    dev->mtu = IXN_NETDEV_MTU_DEFAULT;
    dev->tx_queue_len = 1000u;
    dev->ifindex = 0u;
    dev->ipv4_configured = 0u;
    for (i = 0u; i < IXN_ETH_ALEN; i++) dev->broadcast[i] = 0xFFu;
    if (type == IXN_NETDEV_TYPE_LOOPBACK) {
        dev->flags = IXN_NETDEV_FLAG_LOOPBACK;
        dev->hw_addr[0] = 0x00; dev->hw_addr[1] = 0x00; dev->hw_addr[2] = 0x00;
        dev->hw_addr[3] = 0x00; dev->hw_addr[4] = 0x00; dev->hw_addr[5] = 0x01;
        for (i = 0u; i < IXN_ETH_ALEN; i++) dev->dev_addr[i] = dev->hw_addr[i];
    } else {
        dev->flags = IXN_NETDEV_FLAG_BROADCAST | IXN_NETDEV_FLAG_MULTICAST;
    }
    return dev;
}

int ixn_netdev_register(ixn_netdev_t *dev) {
    ixn_u64 flags;
    ixn_u32 i;
    if (!dev) return -1;
    flags = ixn_irq_lock_acquire(&g_ixn_netdev_lock);
    for (i = 0u; i < IXN_NETDEV_MAX; i++) {
        if (g_ixn_netdevs[i] == IXN_NULL) {
            dev->index = i;
            dev->ifindex = g_ixn_netdev_next_index++;
            g_ixn_netdevs[i] = dev;
            g_ixn_netdev_count++;
            if (!g_ixn_default_dev && dev->type != IXN_NETDEV_TYPE_LOOPBACK) {
                g_ixn_default_dev = dev;
            }
            ixn_irq_lock_release(&g_ixn_netdev_lock, flags);
            return 0;
        }
    }
    ixn_irq_lock_release(&g_ixn_netdev_lock, flags);
    return -2;
}

int ixn_netdev_unregister(ixn_netdev_t *dev) {
    ixn_u64 flags;
    if (!dev) return -1;
    flags = ixn_irq_lock_acquire(&g_ixn_netdev_lock);
    if (dev->index < IXN_NETDEV_MAX && g_ixn_netdevs[dev->index] == dev) {
        g_ixn_netdevs[dev->index] = IXN_NULL;
        if (g_ixn_netdev_count > 0u) g_ixn_netdev_count--;
        if (g_ixn_default_dev == dev) g_ixn_default_dev = IXN_NULL;
    }
    ixn_irq_lock_release(&g_ixn_netdev_lock, flags);
    return 0;
}

int ixn_netdev_open(ixn_netdev_t *dev) {
    int rc;
    if (!dev) return -1;
    if (dev->open) {
        rc = dev->open(dev);
        if (rc != 0) return rc;
    }
    dev->flags |= IXN_NETDEV_FLAG_UP;
    return 0;
}

int ixn_netdev_stop(ixn_netdev_t *dev) {
    int rc;
    if (!dev) return -1;
    if (dev->stop) {
        rc = dev->stop(dev);
        if (rc != 0) return rc;
    }
    dev->flags &= ~IXN_NETDEV_FLAG_UP;
    dev->flags &= ~IXN_NETDEV_FLAG_RUNNING;
    return 0;
}

int ixn_netdev_up(ixn_netdev_t *dev) {
    return ixn_netdev_open(dev);
}

int ixn_netdev_down(ixn_netdev_t *dev) {
    return ixn_netdev_stop(dev);
}

int ixn_netdev_xmit(ixn_netdev_t *dev, ixn_skb_t *skb) {
    int rc;
    if (!dev || !skb) return -1;
    if (!ixn_netdev_is_up(dev)) {
        dev->stats.tx_dropped++;
        return -2;
    }
    if (skb->len > dev->mtu + IXN_ETH_HLEN) {
        dev->stats.tx_dropped++;
        return -3;
    }
    skb->flags |= IXN_SKB_FLAG_TX;
    skb->dev = dev;
    if (dev->start_xmit) {
        rc = dev->start_xmit(dev, skb);
        if (rc == 0) {
            dev->stats.tx_packets++;
            dev->stats.tx_bytes += skb->len;
        } else {
            dev->stats.tx_errors++;
        }
        return rc;
    }
    dev->stats.tx_dropped++;
    return -4;
}

ixn_netdev_t *ixn_netdev_get_by_index(ixn_u32 index) {
    if (index >= IXN_NETDEV_MAX) return IXN_NULL;
    return g_ixn_netdevs[index];
}

ixn_netdev_t *ixn_netdev_get_by_name(const char *name) {
    ixn_u32 i;
    if (!name) return IXN_NULL;
    for (i = 0u; i < IXN_NETDEV_MAX; i++) {
        ixn_netdev_t *d = g_ixn_netdevs[i];
        if (d && ixn_strcmp(d->name, name) == 0) return d;
    }
    return IXN_NULL;
}

ixn_netdev_t *ixn_netdev_get_default(void) {
    return g_ixn_default_dev;
}

ixn_netdev_t *ixn_netdev_get_loopback(void) {
    ixn_u32 i;
    for (i = 0u; i < IXN_NETDEV_MAX; i++) {
        ixn_netdev_t *d = g_ixn_netdevs[i];
        if (d && d->type == IXN_NETDEV_TYPE_LOOPBACK) return d;
    }
    return IXN_NULL;
}

ixn_netdev_t *ixn_netdev_get_first_up(void) {
    ixn_u32 i;
    for (i = 0u; i < IXN_NETDEV_MAX; i++) {
        ixn_netdev_t *d = g_ixn_netdevs[i];
        if (d && (d->flags & IXN_NETDEV_FLAG_UP) && d->type != IXN_NETDEV_TYPE_LOOPBACK) return d;
    }
    for (i = 0u; i < IXN_NETDEV_MAX; i++) {
        ixn_netdev_t *d = g_ixn_netdevs[i];
        if (d && (d->flags & IXN_NETDEV_FLAG_UP)) return d;
    }
    return IXN_NULL;
}

ixn_u32 ixn_netdev_count(void) {
    return g_ixn_netdev_count;
}

void ixn_netdev_set_default(ixn_netdev_t *dev) {
    g_ixn_default_dev = dev;
}

void ixn_netdev_set_rx_handler(ixn_netdev_t *dev, ixn_netdev_rx_fn fn) {
    if (dev) dev->rx_handler = fn;
}

int ixn_netdev_set_mac(ixn_netdev_t *dev, const ixn_u8 *mac) {
    ixn_u32 i;
    if (!dev || !mac) return -1;
    if (dev->set_mac_address) {
        int rc = dev->set_mac_address(dev, mac);
        if (rc != 0) return rc;
    }
    for (i = 0u; i < IXN_ETH_ALEN; i++) {
        dev->hw_addr[i] = mac[i];
        dev->dev_addr[i] = mac[i];
    }
    return 0;
}

int ixn_netdev_set_mtu(ixn_netdev_t *dev, ixn_u32 mtu) {
    if (!dev) return -1;
    if (mtu < 68u || mtu > IXN_NETDEV_MTU_JUMBO) return -2;
    if (dev->set_mtu) {
        int rc = dev->set_mtu(dev, mtu);
        if (rc != 0) return rc;
    }
    dev->mtu = mtu;
    return 0;
}

void ixn_netdev_set_ipv4(ixn_netdev_t *dev, ixn_u32 addr, ixn_u32 mask, ixn_u32 gw) {
    if (!dev) return;
    dev->ipv4_addr = addr;
    dev->ipv4_netmask = mask;
    dev->ipv4_gateway = gw;
    dev->ipv4_broadcast = addr | (~mask);
    dev->ipv4_configured = 1u;
}

void ixn_netdev_poll_all(ixn_u32 budget) {
    ixn_u32 i;
    ixn_u32 b = budget ? budget : IXN_NETDEV_RX_BUDGET;
    for (i = 0u; i < IXN_NETDEV_MAX; i++) {
        ixn_netdev_t *d = g_ixn_netdevs[i];
        if (d && (d->flags & IXN_NETDEV_FLAG_UP) && d->poll) d->poll(d, b);
    }
}

void ixn_netdev_shutdown_all(void) {
    ixn_u32 i;
    for (i = 0u; i < IXN_NETDEV_MAX; i++) {
        ixn_netdev_t *d = g_ixn_netdevs[i];
        if (d) {
            ixn_netdev_stop(d);
            if (d->shutdown) d->shutdown(d);
            g_ixn_netdevs[i] = IXN_NULL;
        }
    }
    g_ixn_netdev_count = 0u;
    g_ixn_default_dev = IXN_NULL;
}

void ixn_netdev_stats_get(ixn_netdev_t *dev, ixn_netdev_stats_t *out) {
    if (!dev || !out) return;
    *out = dev->stats;
}
