#include "ixn_usbnet.h"
#include "ixn_compat.h"
#include "ixn_skb.h"
#include "ixn_l2l3.h"
#include "ixn_rtl8139.h"

static ixn_usb_device_t g_ixn_usb_devs[IXN_USB_MAX_DEVS];
static ixn_u32 g_ixn_usb_dev_count = 0u;
static ixn_usbnet_t g_ixn_usbnet_devs[IXN_USB_MAX_DEVS];
static ixn_u32 g_ixn_usbnet_count = 0u;
static const ixn_usb_hc_driver_t *g_ixn_usb_hc_drivers[4];
static ixn_u32 g_ixn_usb_hc_count = 0u;
static void *g_ixn_usb_hc_privs[4];
static ixn_u32 g_ixn_usb_hc_priv_count = 0u;

void ixn_usb_init(void) {
    ixn_memset(g_ixn_usb_devs, 0, sizeof(g_ixn_usb_devs));
    ixn_memset(g_ixn_usbnet_devs, 0, sizeof(g_ixn_usbnet_devs));
    g_ixn_usb_dev_count = 0u;
    g_ixn_usbnet_count = 0u;
    g_ixn_usb_hc_count = 0u;
    g_ixn_usb_hc_priv_count = 0u;
}

void ixn_usb_register_hc(const ixn_usb_hc_driver_t *drv) {
    if (!drv || g_ixn_usb_hc_count >= 4u) return;
    g_ixn_usb_hc_drivers[g_ixn_usb_hc_count++] = drv;
}

static const ixn_usb_hc_driver_t *ixn_usb_find_hc(ixn_u16 vendor, ixn_u16 device) {
    ixn_u32 i;
    for (i = 0u; i < g_ixn_usb_hc_count; i++) {
        if (g_ixn_usb_hc_drivers[i]->vendor == vendor && g_ixn_usb_hc_drivers[i]->device == device) return g_ixn_usb_hc_drivers[i];
    }
    for (i = 0u; i < g_ixn_usb_hc_count; i++) {
        if (g_ixn_usb_hc_drivers[i]->vendor == 0u && g_ixn_usb_hc_drivers[i]->device == 0u) return g_ixn_usb_hc_drivers[i];
    }
    return IXN_NULL;
}

static int ixn_usb_pci_cb(const ixn_pci_dev_t *pci, void *user) {
    ixn_u32 *pcount = (ixn_u32 *)user;
    ixn_u32 class_base = (pci->class_code >> 16u) & 0xFFu;
    ixn_u32 class_sub = (pci->class_code >> 8u) & 0xFFu;
    if (class_base != 0x0Cu) return 0;
    if (class_sub != 0x03u) return 0;
    {
        const ixn_usb_hc_driver_t *hc = ixn_usb_find_hc(pci->vendor, pci->device);
        if (hc && hc->init && g_ixn_usb_hc_priv_count < 4u) {
            void *priv = IXN_NULL;
            ixn_u64 bar = 0u, bar_size = 0u;
            ixn_u32 bi;
            for (bi = 0u; bi < 6u; bi++) {
                if (pci->bar[bi] && pci->bar_size[bi] >= 0x1000u) { bar = pci->bar[bi]; bar_size = pci->bar_size[bi]; break; }
            }
            ixn_pci_enable_bus_master(pci->bus, pci->slot, pci->func);
            if (hc->init(pci->bus, pci->slot, pci->func, bar, bar_size, &priv) == 0) {
                g_ixn_usb_hc_privs[g_ixn_usb_hc_priv_count++] = priv;
                if (pcount) (*pcount)++;
                return 0;
            }
        }
    }
    return 0;
}

int ixn_usb_probe(void) {
    ixn_u32 found = 0u;
    ixn_pci_scan(ixn_usb_pci_cb, &found);
    return (int)found;
}

void ixn_usb_shutdown(void) {
    ixn_u32 i;
    for (i = 0u; i < g_ixn_usb_hc_priv_count; i++) {
        ixn_u32 j;
        for (j = 0u; j < g_ixn_usb_hc_count; j++) {
            if (g_ixn_usb_hc_drivers[j]->shutdown) g_ixn_usb_hc_drivers[j]->shutdown(g_ixn_usb_hc_privs[i]);
        }
    }
    g_ixn_usb_hc_priv_count = 0u;
    g_ixn_usb_dev_count = 0u;
    g_ixn_usbnet_count = 0u;
}

void ixn_usb_poll(void) {
    ixn_u32 i;
    for (i = 0u; i < g_ixn_usb_hc_priv_count; i++) {
        ixn_u32 j;
        for (j = 0u; j < g_ixn_usb_hc_count; j++) {
            if (g_ixn_usb_hc_drivers[j]->poll) g_ixn_usb_hc_drivers[j]->poll(g_ixn_usb_hc_privs[i]);
        }
    }
}

static void *ixn_usb_get_hc_priv(ixn_usb_device_t *dev) {
    if (!dev || dev->hc_priv >= (void*)(ixn_uptr)4) return IXN_NULL;
    return dev->hc_priv;
}

int ixn_usb_control_msg(ixn_usb_device_t *dev, ixn_u8 rt, ixn_u8 req, ixn_u16 val, ixn_u16 idx, ixn_u8 *buf, ixn_u16 len) {
    ixn_u32 i;
    for (i = 0u; i < g_ixn_usb_hc_count; i++) {
        if (g_ixn_usb_hc_drivers[i]->control) return g_ixn_usb_hc_drivers[i]->control(ixn_usb_get_hc_priv(dev), dev, rt, req, val, idx, buf, len);
    }
    return -1;
}

int ixn_usb_bulk_in(ixn_usb_device_t *dev, ixn_u8 ep, ixn_u8 *buf, ixn_u32 len) {
    ixn_u32 i;
    for (i = 0u; i < g_ixn_usb_hc_count; i++) {
        if (g_ixn_usb_hc_drivers[i]->bulk) return g_ixn_usb_hc_drivers[i]->bulk(ixn_usb_get_hc_priv(dev), dev, ep, IXN_USB_DIR_IN, buf, len);
    }
    return -1;
}

int ixn_usb_bulk_out(ixn_usb_device_t *dev, ixn_u8 ep, ixn_u8 *buf, ixn_u32 len) {
    ixn_u32 i;
    for (i = 0u; i < g_ixn_usb_hc_count; i++) {
        if (g_ixn_usb_hc_drivers[i]->bulk) return g_ixn_usb_hc_drivers[i]->bulk(ixn_usb_get_hc_priv(dev), dev, ep, IXN_USB_DIR_OUT, buf, len);
    }
    return -1;
}

static ixn_u8 ixn_usbnet_detect_type(ixn_usb_device_t *dev) {
    if (!dev) return 0u;
    if (dev->vendor == 0x04B4u && dev->product == 0x6560u) return IXN_USBNET_TYPE_RNDIS;
    if (dev->vendor == 0x0B95u && (dev->product == 0x7720u || dev->product == 0x772Au || dev->product == 0x772Bu)) return IXN_USBNET_TYPE_AX88772;
    if (dev->vendor == 0x0B95u && (dev->product == 0x1790u || dev->product == 0x178Au)) return IXN_USBNET_TYPE_AX88179;
    if (dev->vendor == 0x9710u && dev->product == 0x7830u) return IXN_USBNET_TYPE_MCS7830;
    if (dev->vendor == IXN_RALINK_USB_VENDOR && (dev->product == IXN_RALINK_RT5370 || dev->product == IXN_RALINK_RT5572 ||
        dev->product == IXN_RALINK_RT3070 || dev->product == IXN_RALINK_RT2870 ||
        dev->product == IXN_RALINK_RT3370 || dev->product == IXN_RALINK_RT8070)) return IXN_USBNET_TYPE_CDC_ECM;
    if (dev->vendor == IXN_REALTEK_USB_VENDOR && (dev->product == IXN_REALTEK_RTL8192CU ||
        dev->product == IXN_REALTEK_RTL8188CU || dev->product == IXN_REALTEK_RTL8188EU ||
        dev->product == IXN_REALTEK_RTL8812AU || dev->product == IXN_REALTEK_RTL8821AU)) return IXN_USBNET_TYPE_CDC_ECM;
    if (dev->vendor == IXN_ATHEROS_USB_VENDOR && (dev->product == IXN_ATHEROS_AR9271 ||
        dev->product == IXN_ATHEROS_AR7010)) return IXN_USBNET_TYPE_CDC_ECM;
    if (dev->vendor == 0x07D1u && (dev->product == 0x3303u || dev->product == 0x3C0Du)) return IXN_USBNET_TYPE_RNDIS;
    if (dev->vendor == 0x2001u && (dev->product == 0x3C15u || dev->product == 0x3C19u)) return IXN_USBNET_TYPE_RNDIS;
    if (dev->vendor == 0x13D1u && dev->product == 0x3304u) return IXN_USBNET_TYPE_RNDIS;
    return IXN_USBNET_TYPE_CDC_ECM;
}

static int ixn_usbnet_open(ixn_netdev_t *netdev) {
    ixn_usbnet_t *u = (ixn_usbnet_t *)netdev->priv;
    if (!u) return -1;
    netdev->flags |= IXN_NETDEV_FLAG_RUNNING;
    return 0;
}

static int ixn_usbnet_stop(ixn_netdev_t *netdev) {
    ixn_usbnet_t *u = (ixn_usbnet_t *)netdev->priv;
    if (!u) return -1;
    netdev->flags &= ~IXN_NETDEV_FLAG_RUNNING;
    return 0;
}

static int ixn_usbnet_xmit(ixn_netdev_t *netdev, ixn_skb_t *skb) {
    ixn_usbnet_t *u = (ixn_usbnet_t *)netdev->priv;
    ixn_u32 len;
    if (!u || !u->usbdev || !skb) return -1;
    len = skb->len;
    if (len > IXN_USB_MAX_PACKET) len = IXN_USB_MAX_PACKET;
    if (u->type == IXN_USBNET_TYPE_RNDIS) {
        ixn_u32 hdr[2] = { 0x00000001u, len };
        ixn_memcpy(u->tx_buf, hdr, 8u);
        ixn_memcpy(u->tx_buf + 8u, skb->data, len);
        ixn_usb_bulk_out(u->usbdev, u->usbdev->ep_out_bulk, u->tx_buf, len + 8u);
    } else {
        ixn_memcpy(u->tx_buf, skb->data, len);
        if (len < 60u) {
            ixn_memset(u->tx_buf + len, 0, 60u - len);
            len = 60u;
        }
        ixn_usb_bulk_out(u->usbdev, u->usbdev->ep_out_bulk, u->tx_buf, len);
    }
    ixn_skb_free(skb);
    return 0;
}

static void ixn_usbnet_rx_process(ixn_usbnet_t *u) {
    int rlen;
    if (!u || !u->usbdev) return;
    rlen = ixn_usb_bulk_in(u->usbdev, u->usbdev->ep_in_bulk, u->rx_buf, IXN_USB_MAX_PACKET);
    if (rlen <= 0) return;
    {
        ixn_u32 off = 0u;
        ixn_u32 plen = (ixn_u32)rlen;
        if (u->type == IXN_USBNET_TYPE_RNDIS) {
            if (plen < 44u) return;
            off = 44u;
            plen -= 44u;
        }
        if (plen > 0u && plen <= IXN_USB_MAX_PACKET) {
            ixn_skb_t *skb = ixn_skb_alloc_rx(plen + 16u);
            if (skb) {
                ixn_skb_put(skb, plen);
                ixn_memcpy(skb->data, u->rx_buf + off, plen);
                skb->dev = u->netdev;
                if (u->netdev->rx_handler) u->netdev->rx_handler(u->netdev, skb);
                else ixn_eth_rx(u->netdev, skb);
                ixn_skb_free(skb);
                u->netdev->stats.rx_packets++;
                u->netdev->stats.rx_bytes += plen;
            }
        }
    }
}

void ixn_usbnet_poll(ixn_netdev_t *netdev, ixn_u32 budget) {
    ixn_usbnet_t *u = (ixn_usbnet_t *)netdev->priv;
    ixn_u32 n = 0u;
    (void)budget;
    if (!u) return;
    while (n < 8u) {
        ixn_usbnet_rx_process(u);
        n++;
    }
}

static void ixn_usbnet_shutdown_impl(ixn_netdev_t *netdev) {
    ixn_usbnet_t *u = (ixn_usbnet_t *)netdev->priv;
    if (!u) return;
    ixn_usbnet_stop(netdev);
    if (u->rx_buf) ixn_free(u->rx_buf);
    if (u->tx_buf) ixn_free(u->tx_buf);
}

int ixn_usbnet_init_dev(ixn_usbnet_t *u, ixn_usb_device_t *usbdev, ixn_u8 type) {
    ixn_netdev_t *netdev;
    if (!u || !usbdev) return -1;
    ixn_memset(u, 0, sizeof(*u));
    u->usbdev = usbdev;
    u->type = type;
    u->rx_buf = (ixn_u8 *)ixn_alloc(IXN_USB_MAX_PACKET * 2);
    u->tx_buf = (ixn_u8 *)ixn_alloc(IXN_USB_MAX_PACKET * 2);
    if (!u->rx_buf || !u->tx_buf) return -2;
    netdev = ixn_netdev_alloc("usb0", IXN_NETDEV_TYPE_USBNET);
    if (!netdev) return -3;
    netdev->priv = u;
    u->netdev = netdev;
    netdev->open = ixn_usbnet_open;
    netdev->stop = ixn_usbnet_stop;
    netdev->start_xmit = ixn_usbnet_xmit;
    netdev->poll = ixn_usbnet_poll;
    netdev->shutdown = ixn_usbnet_shutdown_impl;
    if (type == IXN_USBNET_TYPE_AX88772) {
        ixn_u8 mac_buf[6] = {0};
        if (ixn_usb_control_msg(usbdev, 0xC0u, 0x07u, 0x0200u, 0u, mac_buf, 6u) == 0) {
            ixn_u32 i;
            for (i = 0u; i < IXN_ETH_ALEN; i++) { u->mac[i] = mac_buf[i]; netdev->dev_addr[i] = mac_buf[i]; }
        }
    }
    if (ixn_netdev_register(netdev) != 0) return -4;
    return 0;
}

int ixn_usbnet_probe(void) {
    ixn_u32 i;
    ixn_u32 found = 0u;
    for (i = 0u; i < g_ixn_usb_dev_count && g_ixn_usbnet_count < IXN_USB_MAX_DEVS; i++) {
        ixn_usb_device_t *dev = &g_ixn_usb_devs[i];
        ixn_u8 type = ixn_usbnet_detect_type(dev);
        if (type != 0u) {
            ixn_usbnet_t *u = &g_ixn_usbnet_devs[g_ixn_usbnet_count];
            if (ixn_usbnet_init_dev(u, dev, type) == 0) {
                char name[16] = "usb0";
                name[3] = '0' + (char)g_ixn_usbnet_count;
                ixn_strncpy(u->netdev->name, name, IXN_NETDEV_NAME_LEN);
                g_ixn_usbnet_count++;
                found++;
            }
        }
    }
    return (int)found;
}

void ixn_usbnet_shutdown(ixn_usbnet_t *u) {
    if (!u) return;
    if (u->netdev) {
        ixn_netdev_stop(u->netdev);
        if (u->netdev->shutdown) u->netdev->shutdown(u->netdev);
        ixn_netdev_unregister(u->netdev);
    }
    if (u->rx_buf) ixn_free(u->rx_buf);
    if (u->tx_buf) ixn_free(u->tx_buf);
    ixn_memset(u, 0, sizeof(*u));
}
