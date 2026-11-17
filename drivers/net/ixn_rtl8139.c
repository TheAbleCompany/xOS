#include "ixn_rtl8139.h"
#include "ixn_compat.h"
#include "ixn_skb.h"
#include "ixn_l2l3.h"

static ixn_rtl8139_t g_ixn_rtl8139_devs[4];
static ixn_u32 g_ixn_rtl8139_count = 0u;

static ixn_u8 ixn_rtl8139_inb(ixn_rtl8139_t *r, ixn_u8 reg) { return ixn_inb((ixn_u16)(r->io_base + reg)); }
static ixn_u16 ixn_rtl8139_inw(ixn_rtl8139_t *r, ixn_u8 reg) { return ixn_inw((ixn_u16)(r->io_base + reg)); }
static ixn_u32 ixn_rtl8139_inl(ixn_rtl8139_t *r, ixn_u8 reg) { return ixn_inl((ixn_u16)(r->io_base + reg)); }
static void ixn_rtl8139_outb(ixn_rtl8139_t *r, ixn_u8 reg, ixn_u8 v) { ixn_outb((ixn_u16)(r->io_base + reg), v); }
static void ixn_rtl8139_outw(ixn_rtl8139_t *r, ixn_u8 reg, ixn_u16 v) { ixn_outw((ixn_u16)(r->io_base + reg), v); }
static void ixn_rtl8139_outl(ixn_rtl8139_t *r, ixn_u8 reg, ixn_u32 v) { ixn_outl((ixn_u16)(r->io_base + reg), v); }

static void ixn_rtl8139_reset(ixn_rtl8139_t *r) {
    ixn_rtl8139_outb(r, IXN_RTL8139_CR, IXN_RTL8139_CR_RST);
    {
        ixn_u32 spins = 1000u;
        while (spins--) {
            if ((ixn_rtl8139_inb(r, IXN_RTL8139_CR) & IXN_RTL8139_CR_RST) == 0u) break;
            ixn_pause();
        }
    }
}

static void ixn_rtl8139_read_mac(ixn_rtl8139_t *r) {
    ixn_u32 i;
    for (i = 0u; i < IXN_ETH_ALEN; i++) r->mac[i] = ixn_rtl8139_inb(r, (ixn_u8)(IXN_RTL8139_IDR0 + i));
}

static ixn_u8 ixn_rtl8139_link_up(ixn_rtl8139_t *r) {
    ixn_u16 bmsr = ixn_rtl8139_inw(r, IXN_RTL8139_BMSR);
    return (bmsr & 0x04u) ? 1u : 0u;
}

static void ixn_rtl8139_wait_link(ixn_rtl8139_t *r) {
    ixn_u32 spins = 50000u;
    while (spins--) {
        if (ixn_rtl8139_link_up(r)) return;
        ixn_pause();
    }
}

static int ixn_rtl8139_open(ixn_netdev_t *dev) {
    ixn_rtl8139_t *r = (ixn_rtl8139_t *)dev->priv;
    ixn_u32 i;
    if (!r) return -1;
    ixn_rtl8139_reset(r);
    ixn_rtl8139_read_mac(r);
    for (i = 0u; i < IXN_ETH_ALEN; i++) dev->dev_addr[i] = r->mac[i];
    for (i = 0u; i < IXN_ETH_ALEN; i++) ixn_rtl8139_outb(r, (ixn_u8)(IXN_RTL8139_IDR0 + i), r->mac[i]);
    for (i = 0u; i < 8u; i++) ixn_rtl8139_outb(r, (ixn_u8)(IXN_RTL8139_MAR0 + i), 0u);
    /* Адреса TX-буферов для DMA: без TSAD карта читает данные с физического
     * адреса 0 и передаёт мусор (размер кадра верный, содержимое -- нет). */
    for (i = 0u; i < IXN_RTL8139_TX_DESC_NUM; i++)
        ixn_rtl8139_outl(r, (ixn_u8)(IXN_RTL8139_TSAD0 + i * 4u),
                         (ixn_u32)ixn_virt_to_phys(r->tx_buffers[i]));
    ixn_rtl8139_outl(r, IXN_RTL8139_RBSTART, (ixn_u32)ixn_virt_to_phys(r->rx_buffer));
    ixn_rtl8139_outw(r, IXN_RTL8139_ERBCR, 0u);
    ixn_rtl8139_outw(r, IXN_RTL8139_IMR, 0xFFu);
    ixn_rtl8139_outl(r, IXN_RTL8139_RCR, IXN_RTL8139_RCR_AAP | IXN_RTL8139_RCR_APM | IXN_RTL8139_RCR_AM | IXN_RTL8139_RCR_AB | IXN_RTL8139_RCR_WRAP);
    ixn_rtl8139_outl(r, IXN_RTL8139_TCR, IXN_RTL8139_TCR_IFG);
    ixn_rtl8139_outb(r, IXN_RTL8139_CR, IXN_RTL8139_CR_RE | IXN_RTL8139_CR_TE);
    r->rx_buf_offset = 0u;
    r->tx_desc = 0u;
    ixn_rtl8139_wait_link(r);
    dev->flags |= IXN_NETDEV_FLAG_RUNNING;
    return 0;
}

static int ixn_rtl8139_stop(ixn_netdev_t *dev) {
    ixn_rtl8139_t *r = (ixn_rtl8139_t *)dev->priv;
    if (!r) return -1;
    ixn_rtl8139_outw(r, IXN_RTL8139_IMR, 0u);
    ixn_rtl8139_outb(r, IXN_RTL8139_CR, IXN_RTL8139_CR_RST);
    dev->flags &= ~IXN_NETDEV_FLAG_RUNNING;
    return 0;
}

static int ixn_rtl8139_xmit(ixn_netdev_t *dev, ixn_skb_t *skb) {
    ixn_rtl8139_t *r = (ixn_rtl8139_t *)dev->priv;
    ixn_u32 desc;
    ixn_u32 len;
    if (!r || !skb) return -1;
    len = skb->len;
    if (len > IXN_RTL8139_TX_BUF_SIZE) len = IXN_RTL8139_TX_BUF_SIZE;
    desc = r->tx_desc;
    ixn_memcpy(r->tx_buffers[desc], skb->data, len);
    ixn_rtl8139_outl(r, (ixn_u8)(IXN_RTL8139_TSAD0 + desc * 4u),
                     (ixn_u32)ixn_virt_to_phys(r->tx_buffers[desc]));
    ixn_rtl8139_outl(r, (ixn_u8)(IXN_RTL8139_TSD0 + desc * 4u), len | 0x20000000u);
    r->tx_desc = (desc + 1u) % IXN_RTL8139_TX_DESC_NUM;
    {
        ixn_u32 spins = 10000u;
        while (spins--) {
            ixn_u32 status = ixn_rtl8139_inl(r, (ixn_u8)(IXN_RTL8139_TSD0 + desc * 4u));
            if (status & 0x80000000u) break;
            ixn_pause();
        }
    }
    ixn_skb_free(skb);
    return 0;
}

static void ixn_rtl8139_rx_process(ixn_rtl8139_t *r) {
    ixn_u32 guard = 0u;
    while ((ixn_rtl8139_inb(r, IXN_RTL8139_CR) & IXN_RTL8139_CR_BUFE) == 0u) {
        /*
         * Заголовок пакета лежит в общей с DMA памяти rx-буфера:
         * 2 байта статуса + 2 байта длины, затем данные.
         * Раньше заголовок читали из регистра CAPR -- а это УКАЗАТЕЛЬ
         * чтения, а не данные: пакеты "разбирались" из мусора и
         * терялись.
         */
        ixn_u32 off = r->rx_buf_offset;
        ixn_u16 status = 0u, pkt_len = 0u;
        ixn_memcpy(&status, r->rx_buffer + off, 2u);
        ixn_memcpy(&pkt_len, r->rx_buffer + off + 2u, 2u);
        if (status & 0x20u) { /* ISE: сломанный пакет, сбрасываем указатель */
            r->rx_buf_offset = 0u;
            ixn_rtl8139_outw(r, IXN_RTL8139_CAPR, 0u);
            continue;
        }
        if (pkt_len < 4u || pkt_len > 1792u) {
            r->rx_buf_offset = 0u;
            ixn_rtl8139_outw(r, IXN_RTL8139_CAPR, 0u);
            continue;
        }
        {
            ixn_u32 real_len = (ixn_u32)pkt_len - 4u; /* pkt_len включает CRC */
            if (status & 0x01u) { /* ROK */
                ixn_skb_t *skb = ixn_skb_alloc_rx(real_len + 16u);
                if (skb) {
                    ixn_skb_put(skb, real_len);
                    ixn_memcpy(skb->data, r->rx_buffer + off + 4u, real_len);
                    skb->dev = r->netdev;
                    if (r->netdev->rx_handler) r->netdev->rx_handler(r->netdev, skb);
                    else ixn_eth_rx(r->netdev, skb);
                    ixn_skb_free(skb);
                    r->netdev->stats.rx_packets++;
                    r->netdev->stats.rx_bytes += real_len;
                } else {
                    r->netdev->stats.rx_dropped++;
                }
            }
        }
        r->rx_buf_offset = (r->rx_buf_offset + pkt_len + 4u + 3u) & ~3u;
        /* Кольцо, как его видит NIC/эмулятор -- RX_RING_SIZE (8192);
         * данные за концом кольца лежат в guard-области выделенного буфера. */
        if (r->rx_buf_offset >= IXN_RTL8139_RX_RING_SIZE) r->rx_buf_offset -= IXN_RTL8139_RX_RING_SIZE;
        ixn_rtl8139_outw(r, IXN_RTL8139_CAPR, (ixn_u16)(r->rx_buf_offset - 16u));
        if (++guard > 4096u) break; /* защита от вечного цикла на битом буфере */
    }
}

void ixn_rtl8139_poll(ixn_netdev_t *dev, ixn_u32 budget) {
    ixn_rtl8139_t *r = (ixn_rtl8139_t *)dev->priv;
    ixn_u16 isr;
    (void)budget;
    if (!r) return;
    isr = ixn_rtl8139_inw(r, IXN_RTL8139_ISR);
    if (isr & (IXN_RTL8139_ISR_ROK | IXN_RTL8139_ISR_FOV | IXN_RTL8139_ISR_RXOVW)) {
        ixn_rtl8139_rx_process(r);
    }
    ixn_rtl8139_outw(r, IXN_RTL8139_ISR, isr);
}

static void ixn_rtl8139_shutdown_impl(ixn_netdev_t *dev) {
    ixn_rtl8139_t *r = (ixn_rtl8139_t *)dev->priv;
    ixn_u32 i;
    if (!r) return;
    ixn_rtl8139_stop(dev);
    if (r->rx_buffer) ixn_free(r->rx_buffer);
    for (i = 0u; i < IXN_RTL8139_TX_DESC_NUM; i++) if (r->tx_buffers[i]) ixn_free(r->tx_buffers[i]);
}

int ixn_rtl8139_init_dev(ixn_rtl8139_t *r, const ixn_pci_dev_t *pci) {
    ixn_netdev_t *dev;
    ixn_u32 i;
    if (!r || !pci) return -1;
    ixn_memset(r, 0, sizeof(*r));
    r->bus = pci->bus; r->slot = pci->slot; r->func = pci->func;
    r->vendor = pci->vendor; r->device = pci->device; r->irq = pci->irq;
    r->io_base = (ixn_u32)pci->bar[0];
    if (r->io_base == 0u) return -2;
    r->rx_buffer = (ixn_u8 *)ixn_alloc(IXN_RTL8139_RX_BUF_SIZE);
    if (!r->rx_buffer) return -3;
    for (i = 0u; i < IXN_RTL8139_TX_DESC_NUM; i++) {
        r->tx_buffers[i] = (ixn_u8 *)ixn_alloc(IXN_RTL8139_TX_BUF_SIZE);
        if (!r->tx_buffers[i]) return -4;
    }
    dev = ixn_netdev_alloc("eth0", IXN_NETDEV_TYPE_ETHER);
    if (!dev) return -5;
    dev->priv = r;
    r->netdev = dev;
    dev->open = ixn_rtl8139_open;
    dev->stop = ixn_rtl8139_stop;
    dev->start_xmit = ixn_rtl8139_xmit;
    dev->poll = ixn_rtl8139_poll;
    dev->shutdown = ixn_rtl8139_shutdown_impl;
    dev->pci.bus = pci->bus; dev->pci.slot = pci->slot; dev->pci.func = pci->func;
    dev->pci.vendor = pci->vendor; dev->pci.device = pci->device;
    dev->pci.irq = pci->irq;
    ixn_pci_enable_bus_master(pci->bus, pci->slot, pci->func);
    if (ixn_netdev_register(dev) != 0) return -6;
    return 0;
}

static int ixn_rtl8139_pci_cb(const ixn_pci_dev_t *pci, void *user) {
    ixn_u32 *pcount = (ixn_u32 *)user;
    if (pci->vendor != IXN_RTL8139_VENDOR_REALTEK) return 0;
    if (pci->device != IXN_RTL8139_DEVICE_8139 && pci->device != IXN_RTL8139_DEVICE_8129) return 0;
    if (g_ixn_rtl8139_count >= 4u) return 0;
    {
        char name[16] = "eth0";
        ixn_rtl8139_t *r = &g_ixn_rtl8139_devs[g_ixn_rtl8139_count];
        int rc = ixn_rtl8139_init_dev(r, pci);
        if (rc == 0) {
            name[3] = '0' + (char)g_ixn_rtl8139_count;
            ixn_strncpy(r->netdev->name, name, IXN_NETDEV_NAME_LEN);
            g_ixn_rtl8139_count++;
            if (pcount) (*pcount)++;
        }
    }
    return 0;
}

int ixn_rtl8139_probe(void) {
    ixn_u32 found = 0u;
    g_ixn_rtl8139_count = 0u;
    ixn_pci_scan_network(ixn_rtl8139_pci_cb, &found);
    return (int)found;
}

void ixn_rtl8139_shutdown(ixn_rtl8139_t *r) {
    if (!r) return;
    if (r->netdev) {
        ixn_netdev_stop(r->netdev);
        if (r->netdev->shutdown) r->netdev->shutdown(r->netdev);
        ixn_netdev_unregister(r->netdev);
    }
    if (r->rx_buffer) ixn_free(r->rx_buffer);
    { ixn_u32 i; for (i = 0u; i < IXN_RTL8139_TX_DESC_NUM; i++) if (r->tx_buffers[i]) ixn_free(r->tx_buffers[i]); }
    ixn_memset(r, 0, sizeof(*r));
}
