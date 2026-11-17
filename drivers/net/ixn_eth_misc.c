#include "ixn_eth_misc.h"
#include "ixn_rtl8139.h"
#include "ixn_compat.h"
#include "ixn_skb.h"
#include "ixn_l2l3.h"

static ixn_eth_misc_t g_ixn_eth_misc_devs[IXN_ETH_MISC_MAX_DEVS];
static ixn_u32 g_ixn_eth_misc_count = 0u;

static ixn_u8 ixn_eth_misc_detect_type(ixn_u16 vendor, ixn_u16 device) {
    if (vendor == IXN_ATHEROS_VENDOR) {
        if (device == IXN_ATHEROS_L1E || device == IXN_ATHEROS_L2E ||
            device == IXN_ATHEROS_AR8161 || device == IXN_ATHEROS_AR8162 ||
            device == IXN_ATHEROS_AR8171 || device == IXN_ATHEROS_AR8172 ||
            device == IXN_ATHEROS_AR8131 || device == IXN_ATHEROS_AR8132)
            return IXN_ETH_MISC_ATHEROS_L1;
    }
    if (vendor == IXN_BROADCOM_NET_VENDOR) {
        if (device == IXN_BROADCOM_BCM57780 || device == IXN_BROADCOM_BCM5720 ||
            device == IXN_BROADCOM_BCM5722 || device == IXN_BROADCOM_BCM5709 ||
            device == IXN_BROADCOM_BCM5716)
            return IXN_ETH_MISC_BROADCOM_BCM572x;
    }
    if (vendor == IXN_MARVELL_VENDOR) {
        if (device == IXN_MARVELL_YUKON_8053 || device == IXN_MARVELL_YUKON_8071 ||
            device == IXN_MARVELL_YUKON_8072 || device == IXN_MARVELL_YUKON_8055 ||
            device == IXN_MARVELL_YUKON_8050)
            return IXN_ETH_MISC_MARVELL_YUKON;
    }
    if (vendor == IXN_VIA_NET_VENDOR) {
        if (device == IXN_VIA_RHINE_3043 || device == IXN_VIA_RHINE_3053 ||
            device == IXN_VIA_RHINE_3106)
            return IXN_ETH_MISC_VIA_RHINE;
    }
    return 0u;
}

static int ixn_eth_misc_open(ixn_netdev_t *dev) {
    ixn_eth_misc_t *m = (ixn_eth_misc_t *)dev->priv;
    if (!m) return -1;
    dev->flags |= IXN_NETDEV_FLAG_RUNNING;
    return 0;
}

static int ixn_eth_misc_stop(ixn_netdev_t *dev) {
    dev->flags &= ~IXN_NETDEV_FLAG_RUNNING;
    return 0;
}

static int ixn_eth_misc_xmit(ixn_netdev_t *dev, ixn_skb_t *skb) {
    ixn_skb_free(skb);
    dev->stats.tx_packets++;
    dev->stats.tx_bytes += skb->len;
    return 0;
}

void ixn_eth_misc_poll(ixn_netdev_t *dev, ixn_u32 budget) {
    (void)dev;
    (void)budget;
}

static void ixn_eth_misc_shutdown_impl(ixn_netdev_t *dev) {
    ixn_eth_misc_t *m = (ixn_eth_misc_t *)dev->priv;
    if (!m) return;
    ixn_eth_misc_stop(dev);
    if (m->rx_buf) ixn_free(m->rx_buf);
    if (m->tx_buf) ixn_free(m->tx_buf);
}

static int ixn_eth_misc_init_dev(ixn_eth_misc_t *m, const ixn_pci_dev_t *pci, ixn_u8 chip_type) {
    ixn_netdev_t *dev;
    if (!m || !pci) return -1;
    ixn_memset(m, 0, sizeof(*m));
    m->bus = pci->bus; m->slot = pci->slot; m->func = pci->func;
    m->vendor = pci->vendor; m->device = pci->device; m->irq = pci->irq;
    m->chip_type = chip_type;
    if (pci->bar[0] && !(pci->bar[0] & 1u)) {
        m->mmio = ixn_pci_map_bar(pci->bar[0], pci->bar_size[0]);
    } else if (pci->bar[0]) {
        m->io_base = (ixn_u32)pci->bar[0];
    }
    m->rx_buf = (ixn_u8 *)ixn_alloc(4096u);
    m->tx_buf = (ixn_u8 *)ixn_alloc(4096u);
    if (!m->rx_buf || !m->tx_buf) return -2;
    dev = ixn_netdev_alloc("ethN", IXN_NETDEV_TYPE_ETHER);
    if (!dev) return -3;
    dev->priv = m;
    m->netdev = dev;
    dev->open = ixn_eth_misc_open;
    dev->stop = ixn_eth_misc_stop;
    dev->start_xmit = ixn_eth_misc_xmit;
    dev->poll = ixn_eth_misc_poll;
    dev->shutdown = ixn_eth_misc_shutdown_impl;
    dev->pci.bus = pci->bus; dev->pci.slot = pci->slot; dev->pci.func = pci->func;
    dev->pci.vendor = pci->vendor; dev->pci.device = pci->device;
    dev->pci.irq = pci->irq;
    ixn_pci_enable_bus_master(pci->bus, pci->slot, pci->func);
    if (ixn_netdev_register(dev) != 0) return -4;
    return 0;
}

static int ixn_eth_misc_pci_cb(const ixn_pci_dev_t *pci, void *user) {
    ixn_u32 *pcount = (ixn_u32 *)user;
    ixn_u8 chip_type = ixn_eth_misc_detect_type(pci->vendor, pci->device);
    if (chip_type == 0u) return 0;
    if (g_ixn_eth_misc_count >= IXN_ETH_MISC_MAX_DEVS) return 0;
    {
        ixn_eth_misc_t *m = &g_ixn_eth_misc_devs[g_ixn_eth_misc_count];
        if (ixn_eth_misc_init_dev(m, pci, chip_type) == 0) {
            char name[16] = "ethN";
            name[3] = '0' + (char)g_ixn_eth_misc_count;
            ixn_strncpy(m->netdev->name, name, IXN_NETDEV_NAME_LEN);
            g_ixn_eth_misc_count++;
            if (pcount) (*pcount)++;
        }
    }
    return 0;
}

int ixn_eth_misc_probe(void) {
    ixn_u32 found = 0u;
    g_ixn_eth_misc_count = 0u;
    ixn_pci_scan_network(ixn_eth_misc_pci_cb, &found);
    return (int)found;
}
