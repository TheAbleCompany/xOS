#include "ixn_e1000.h"
#include "ixn_compat.h"
#include "ixn_skb.h"
#include "ixn_l2l3.h"

static ixn_e1000_t g_ixn_e1000_devs[4];
static ixn_u32 g_ixn_e1000_count = 0u;

static ixn_u32 ixn_e1000_reg_read(ixn_e1000_t *e, ixn_u32 off) {
    return ixn_mmio_read32(e->mmio, off);
}
static void ixn_e1000_reg_write(ixn_e1000_t *e, ixn_u32 off, ixn_u32 val) {
    ixn_mmio_write32(e->mmio, off, val);
}

static ixn_u16 ixn_e1000_eeprom_read(ixn_e1000_t *e, ixn_u8 word) {
    ixn_u32 data;
    ixn_u32 spins = 1000u;
    if (!e->has_eeprom) return 0u;
    data = ((ixn_u32)word << 8u) | IXN_E1000_EERD_START;
    ixn_e1000_reg_write(e, IXN_E1000_REG_EERD, data);
    while (spins--) {
        data = ixn_e1000_reg_read(e, IXN_E1000_REG_EERD);
        if (data & IXN_E1000_EERD_DONE) return (ixn_u16)(data >> 16u);
        ixn_pause();
    }
    return 0u;
}

static int ixn_e1000_mac_significant(const ixn_u8 *mac) {
    ixn_u32 or_all = 0u, and_all = 0xFFu;
    ixn_u32 i;
    for (i = 0u; i < IXN_ETH_ALEN; i++) {
        or_all |= mac[i];
        and_all &= mac[i];
    }
    if (or_all == 0u) return 0;
    if (and_all == 0xFFu) return 0;
    return 1;
}

static void ixn_e1000_read_mac(ixn_e1000_t *e) {
    ixn_u16 w[3];

    ixn_memset(e->mac, 0, IXN_ETH_ALEN);

    w[0] = ixn_e1000_eeprom_read(e, 0u);
    w[1] = ixn_e1000_eeprom_read(e, 1u);
    w[2] = ixn_e1000_eeprom_read(e, 2u);

    if (ixn_e1000_mac_significant((const ixn_u8 *)w)) {
        e->mac[0] = (ixn_u8)(w[0] & 0xFFu);
        e->mac[1] = (ixn_u8)(w[0] >> 8u);
        e->mac[2] = (ixn_u8)(w[1] & 0xFFu);
        e->mac[3] = (ixn_u8)(w[1] >> 8u);
        e->mac[4] = (ixn_u8)(w[2] & 0xFFu);
        e->mac[5] = (ixn_u8)(w[2] >> 8u);
        if (ixn_e1000_mac_significant(e->mac)) return;
        ixn_memset(e->mac, 0, IXN_ETH_ALEN);
    }

    {
        ixn_u32 ral = ixn_e1000_reg_read(e, IXN_E1000_REG_RA);
        ixn_u32 rah = ixn_e1000_reg_read(e, IXN_E1000_REG_RA + 4u);
        e->mac[0] = (ixn_u8)(ral & 0xFFu);
        e->mac[1] = (ixn_u8)(ral >> 8u);
        e->mac[2] = (ixn_u8)(ral >> 16u);
        e->mac[3] = (ixn_u8)(ral >> 24u);
        e->mac[4] = (ixn_u8)(rah & 0xFFu);
        e->mac[5] = (ixn_u8)(rah >> 8u);
        if (ixn_e1000_mac_significant(e->mac)) return;
        ixn_memset(e->mac, 0, IXN_ETH_ALEN);
    }

    e->mac[0] = 0x02u;
    e->mac[1] = 0x69u;
    e->mac[2] = (ixn_u8)(0x10u + (e->slot & 0x0Fu));
    e->mac[3] = (ixn_u8)(e->device & 0xFFu);
    e->mac[4] = (ixn_u8)(e->device >> 8u);
    e->mac[5] = (ixn_u8)(0x01u + (e->func & 0x07u));
}

static void ixn_e1000_reset(ixn_e1000_t *e) {
    ixn_e1000_reg_write(e, IXN_E1000_REG_CTRL, IXN_E1000_CTRL_RST);
    ixn_mfence();
    {
        ixn_u32 spins = 1000u;
        while (spins--) {
            ixn_u32 ctrl = ixn_e1000_reg_read(e, IXN_E1000_REG_CTRL);
            if ((ctrl & IXN_E1000_CTRL_RST) == 0u) break;
            ixn_pause();
        }
    }
    ixn_e1000_reg_write(e, IXN_E1000_REG_CTRL, IXN_E1000_CTRL_SLU | IXN_E1000_CTRL_ASDE);
}

static void ixn_e1000_program_rar(ixn_e1000_t *e) {
    ixn_u32 ral = ((ixn_u32)e->mac[3] << 24u) | ((ixn_u32)e->mac[2] << 16u) |
                  ((ixn_u32)e->mac[1] << 8u) | (ixn_u32)e->mac[0];
    ixn_u32 rah = ((ixn_u32)e->mac[5] << 8u) | (ixn_u32)e->mac[4] | 0x80000000u;
    ixn_e1000_reg_write(e, IXN_E1000_REG_RA, ral);
    ixn_e1000_reg_write(e, IXN_E1000_REG_RA + 4u, rah);
}

static void ixn_e1000_setup_rx(ixn_e1000_t *e) {
    ixn_u32 i;
    ixn_u32 rctl;
    ixn_memset(e->rx_descs, 0, IXN_E1000_RX_DESC_NUM * sizeof(ixn_e1000_rx_desc_t));
    for (i = 0u; i < IXN_E1000_RX_DESC_NUM; i++) {
        e->rx_descs[i].buffer_addr = ixn_virt_to_phys(e->rx_buffers + i * IXN_E1000_PKT_BUF_SIZE);
        e->rx_descs[i].status = 0u;
    }
    e->rx_head = 0u;
    e->rx_tail = 0u;
    ixn_e1000_reg_write(e, IXN_E1000_REG_RDBAL, (ixn_u32)(ixn_virt_to_phys(e->rx_descs) & 0xFFFFFFFFu));
    ixn_e1000_reg_write(e, IXN_E1000_REG_RDBAH, (ixn_u32)(ixn_virt_to_phys(e->rx_descs) >> 32u));
    ixn_e1000_reg_write(e, IXN_E1000_REG_RDLEN, IXN_E1000_RX_DESC_NUM * sizeof(ixn_e1000_rx_desc_t));
    ixn_e1000_reg_write(e, IXN_E1000_REG_RDH, 0u);
    ixn_e1000_reg_write(e, IXN_E1000_REG_RDT, IXN_E1000_RX_DESC_NUM - 1u);
    rctl = IXN_E1000_RCTL_EN | IXN_E1000_RCTL_BAM | IXN_E1000_RCTL_SZ_2048 | IXN_E1000_RCTL_SECRC;
    ixn_e1000_reg_write(e, IXN_E1000_REG_RCTL, rctl);
}

static void ixn_e1000_setup_tx(ixn_e1000_t *e) {
    ixn_u32 tctl;
    ixn_memset(e->tx_descs, 0, IXN_E1000_TX_DESC_NUM * sizeof(ixn_e1000_tx_desc_t));
    e->tx_head = 0u;
    e->tx_tail = 0u;
    e->tx_used = 0u;
    ixn_e1000_reg_write(e, IXN_E1000_REG_TDBAL, (ixn_u32)(ixn_virt_to_phys(e->tx_descs) & 0xFFFFFFFFu));
    ixn_e1000_reg_write(e, IXN_E1000_REG_TDBAH, (ixn_u32)(ixn_virt_to_phys(e->tx_descs) >> 32u));
    ixn_e1000_reg_write(e, IXN_E1000_REG_TDLEN, IXN_E1000_TX_DESC_NUM * sizeof(ixn_e1000_tx_desc_t));
    ixn_e1000_reg_write(e, IXN_E1000_REG_TDH, 0u);
    ixn_e1000_reg_write(e, IXN_E1000_REG_TDT, 0u);
    ixn_e1000_reg_write(e, IXN_E1000_REG_TIDV, 0u);
    ixn_e1000_reg_write(e, IXN_E1000_REG_TIPG, 0x0060200Au);
    ixn_e1000_reg_write(e, IXN_E1000_REG_PBA, 0x00000020u);
    tctl = IXN_E1000_TCTL_EN | IXN_E1000_TCTL_PSP | (0x10u << 4u) | IXN_E1000_TCTL_COLD | IXN_E1000_TCTL_RTLC;
    ixn_e1000_reg_write(e, IXN_E1000_REG_TCTL, tctl);
}

static ixn_u8 ixn_e1000_link_up(ixn_e1000_t *e) {
    ixn_u32 status = ixn_e1000_reg_read(e, IXN_E1000_REG_STATUS);
    return (status & 0x02u) ? 1u : 0u;
}

static void ixn_e1000_setup_multicast(ixn_e1000_t *e) {
    ixn_u32 i;
    for (i = 0u; i < 128u; i++) ixn_e1000_reg_write(e, IXN_E1000_REG_MTA + i * 4u, 0u);
}

static void ixn_e1000_wait_link(ixn_e1000_t *e) {
    ixn_u32 spins = 100000u;
    while (spins--) {
        if (ixn_e1000_link_up(e)) return;
        ixn_pause();
    }
}

static int ixn_e1000_open(ixn_netdev_t *dev) {
    ixn_e1000_t *e = (ixn_e1000_t *)dev->priv;
    ixn_u32 ctrl;
    if (!e) return -1;
    ixn_e1000_reset(e);
    ixn_e1000_read_mac(e);
    {
        ixn_u32 i;
        for (i = 0u; i < IXN_ETH_ALEN; i++) dev->dev_addr[i] = e->mac[i];
    }
    ctrl = ixn_e1000_reg_read(e, IXN_E1000_REG_CTRL);
    ctrl |= IXN_E1000_CTRL_SLU | IXN_E1000_CTRL_FRCSPD | IXN_E1000_CTRL_FRCDPX;
    ixn_e1000_reg_write(e, IXN_E1000_REG_CTRL, ctrl);
    ixn_e1000_program_rar(e);
    ixn_e1000_setup_multicast(e);
    ixn_e1000_setup_rx(e);
    ixn_e1000_setup_tx(e);
    ixn_e1000_reg_write(e, IXN_E1000_REG_IMS, IXN_E1000_ICR_RXT0 | IXN_E1000_ICR_TXDW | IXN_E1000_ICR_LSC | IXN_E1000_ICR_RXDMT0);
    ixn_e1000_wait_link(e);
    dev->flags |= IXN_NETDEV_FLAG_RUNNING;
    return 0;
}

static int ixn_e1000_stop(ixn_netdev_t *dev) {
    ixn_e1000_t *e = (ixn_e1000_t *)dev->priv;
    if (!e) return -1;
    ixn_e1000_reg_write(e, IXN_E1000_REG_IMC, 0xFFFFFFFFu);
    ixn_e1000_reg_write(e, IXN_E1000_REG_RCTL, 0u);
    ixn_e1000_reg_write(e, IXN_E1000_REG_TCTL, 0u);
    dev->flags &= ~IXN_NETDEV_FLAG_RUNNING;
    return 0;
}

static int ixn_e1000_xmit(ixn_netdev_t *dev, ixn_skb_t *skb) {
    ixn_e1000_t *e = (ixn_e1000_t *)dev->priv;
    ixn_u32 tail;
    ixn_u32 copy_len;
    if (!e || !skb) return -1;
    if (e->tx_used >= IXN_E1000_TX_DESC_NUM) {
        /* Кольцо заполнено: прежде чем писать в дескриптор, соберём
         * уже завершённые передачи, иначе затрём неотправленный пакет. */
        while (e->tx_used > 0u && (e->tx_descs[e->tx_head].status & 0x01u)) {
            e->tx_head = (e->tx_head + 1u) % IXN_E1000_TX_DESC_NUM;
            e->tx_used--;
        }
        if (e->tx_used >= IXN_E1000_TX_DESC_NUM) {
            dev->stats.tx_dropped++;
            return -5;
        }
    }
    copy_len = skb->len > IXN_E1000_PKT_BUF_SIZE ? IXN_E1000_PKT_BUF_SIZE : skb->len;
    tail = e->tx_tail;
    ixn_memcpy(e->tx_buffers + tail * IXN_E1000_PKT_BUF_SIZE, skb->data, copy_len);
    e->tx_descs[tail].buffer_addr = ixn_virt_to_phys(e->tx_buffers + tail * IXN_E1000_PKT_BUF_SIZE);
    e->tx_descs[tail].length = (ixn_u16)copy_len;
    e->tx_descs[tail].cmd = IXN_E1000_TXD_CMD_EOP | IXN_E1000_TXD_CMD_IFCS | IXN_E1000_TXD_CMD_RS;
    e->tx_descs[tail].status = 0u;
    e->tx_tail = (tail + 1u) % IXN_E1000_TX_DESC_NUM;
    e->tx_used++;
    ixn_e1000_reg_write(e, IXN_E1000_REG_TDT, e->tx_tail);
    ixn_skb_free(skb);
    return 0;
}

static void ixn_e1000_rx_process(ixn_e1000_t *e, ixn_u32 budget) {
    ixn_u32 processed = 0u;
    (void)budget;
    while (processed < IXN_E1000_RX_DESC_NUM) {
        ixn_e1000_rx_desc_t *desc = &e->rx_descs[e->rx_head];
        if ((desc->status & 0x01u) == 0u) break;
        {
            ixn_u32 len = desc->length;
            ixn_skb_t *skb;
            if (len > 0u && len <= IXN_E1000_PKT_BUF_SIZE) {
                skb = ixn_skb_alloc_rx(len + 16u);
                if (skb) {
                    ixn_skb_put(skb, len);
                    ixn_memcpy(skb->data, e->rx_buffers + e->rx_head * IXN_E1000_PKT_BUF_SIZE, len);
                    skb->dev = e->netdev;
                    if (e->netdev->rx_handler) e->netdev->rx_handler(e->netdev, skb);
                    else ixn_eth_rx(e->netdev, skb);
                    ixn_skb_free(skb);
                }
                e->netdev->stats.rx_packets++;
                e->netdev->stats.rx_bytes += len;
            }
        }
        desc->status = 0u;
        e->rx_head = (e->rx_head + 1u) % IXN_E1000_RX_DESC_NUM;
        e->rx_tail = (e->rx_tail + 1u) % IXN_E1000_RX_DESC_NUM;
        /*
         * Возвращаем буфер железу: RDT -- индекс ПОСЛЕДНЕГО дескриптора,
         * который NIC имеет право заполнить, а не следующего свободного.
         * Раньше писали RDT = rx_tail; при RDH == RDT контроллер считал
         * кольцо пустым и прекращал приём -- все пакеты после первой
         * пачки (ответы ping, TCP и т.д.) терялись.
         */
        ixn_e1000_reg_write(e, IXN_E1000_REG_RDT,
                            (e->rx_tail + IXN_E1000_RX_DESC_NUM - 1u) % IXN_E1000_RX_DESC_NUM);
        processed++;
    }
}

void ixn_e1000_poll(ixn_netdev_t *dev, ixn_u32 budget) {
    ixn_e1000_t *e = (ixn_e1000_t *)dev->priv;
    ixn_u32 icr;
    if (!e) return;
    icr = ixn_e1000_reg_read(e, IXN_E1000_REG_ICR);
    ixn_e1000_rx_process(e, budget);
    if (icr & IXN_E1000_ICR_TXDW) {
        while (e->tx_used > 0u) {
            ixn_u32 prev = (e->tx_head + IXN_E1000_TX_DESC_NUM - 1u) % IXN_E1000_TX_DESC_NUM;
            if (e->tx_descs[prev].status & 0x01u) {
                e->tx_head = (e->tx_head + 1u) % IXN_E1000_TX_DESC_NUM;
                e->tx_used--;
            } else break;
        }
    }
    (void)icr;
}

static void ixn_e1000_shutdown_impl(ixn_netdev_t *dev) {
    ixn_e1000_t *e = (ixn_e1000_t *)dev->priv;
    if (!e) return;
    ixn_e1000_stop(dev);
    if (e->rx_descs) ixn_free(e->rx_descs);
    if (e->tx_descs) ixn_free(e->tx_descs);
    if (e->rx_buffers) ixn_free(e->rx_buffers);
    if (e->tx_buffers) ixn_free(e->tx_buffers);
}

int ixn_e1000_init_dev(ixn_e1000_t *e, const ixn_pci_dev_t *pci) {
    ixn_netdev_t *dev;
    ixn_u32 eecd;
    if (!e || !pci) return -1;
    ixn_memset(e, 0, sizeof(*e));
    e->bus = pci->bus; e->slot = pci->slot; e->func = pci->func;
    e->vendor = pci->vendor; e->device = pci->device; e->irq = pci->irq;
    e->mmio = ixn_pci_map_bar(pci->bar[0], pci->bar_size[0]);
    if (!e->mmio) return -2;
    eecd = ixn_e1000_reg_read(e, IXN_E1000_REG_EECD);
    e->has_eeprom = (eecd & 0x10u) ? 1u : 0u;
    ixn_e1000_reset(e);
    ixn_e1000_read_mac(e);
    e->rx_descs = (ixn_e1000_rx_desc_t *)ixn_alloc_zero(IXN_E1000_RX_DESC_NUM * sizeof(ixn_e1000_rx_desc_t));
    e->tx_descs = (ixn_e1000_tx_desc_t *)ixn_alloc_zero(IXN_E1000_TX_DESC_NUM * sizeof(ixn_e1000_tx_desc_t));
    e->rx_buffers = (ixn_u8 *)ixn_alloc(IXN_E1000_RX_DESC_NUM * IXN_E1000_PKT_BUF_SIZE);
    e->tx_buffers = (ixn_u8 *)ixn_alloc(IXN_E1000_TX_DESC_NUM * IXN_E1000_PKT_BUF_SIZE);
    if (!e->rx_descs || !e->tx_descs || !e->rx_buffers || !e->tx_buffers) return -3;
    dev = ixn_netdev_alloc("eth0", IXN_NETDEV_TYPE_ETHER);
    if (!dev) return -4;
    dev->priv = e;
    e->netdev = dev;
    dev->open = ixn_e1000_open;
    dev->stop = ixn_e1000_stop;
    dev->start_xmit = ixn_e1000_xmit;
    dev->poll = ixn_e1000_poll;
    dev->shutdown = ixn_e1000_shutdown_impl;
    dev->pci.bus = pci->bus; dev->pci.slot = pci->slot; dev->pci.func = pci->func;
    dev->pci.vendor = pci->vendor; dev->pci.device = pci->device;
    dev->pci.irq = pci->irq;
    ixn_pci_enable_bus_master(pci->bus, pci->slot, pci->func);
    if (ixn_netdev_register(dev) != 0) return -5;
    return 0;
}

static int ixn_e1000_pci_cb(const ixn_pci_dev_t *pci, void *user) {
    ixn_u32 *pcount = (ixn_u32 *)user;
    if (pci->vendor != IXN_E1000_VENDOR_INTEL) return 0;
    switch (pci->device) {
        case IXN_E1000_DEVICE_82540EM:
        case IXN_E1000_DEVICE_82573L:
        case IXN_E1000_DEVICE_82574L:
        case IXN_E1000_DEVICE_82577LM:
        case IXN_E1000_DEVICE_I219LM:
        case IXN_E1000_DEVICE_I219V:
        case IXN_E1000_DEVICE_82567LM:
            break;
        default:
            return 0;
    }
    if (g_ixn_e1000_count >= 4u) return 0;
    {
        char name[16] = "eth0";
        ixn_e1000_t *e = &g_ixn_e1000_devs[g_ixn_e1000_count];
        int rc = ixn_e1000_init_dev(e, pci);
        if (rc == 0) {
            name[3] = '0' + (char)g_ixn_e1000_count;
            ixn_strncpy(e->netdev->name, name, IXN_NETDEV_NAME_LEN);
            g_ixn_e1000_count++;
            if (pcount) (*pcount)++;
        }
    }
    return 0;
}

int ixn_e1000_probe(void) {
    ixn_u32 found = 0u;
    g_ixn_e1000_count = 0u;
    ixn_pci_scan_network(ixn_e1000_pci_cb, &found);
    return (int)found;
}

void ixn_e1000_shutdown(ixn_e1000_t *e) {
    if (!e) return;
    if (e->netdev) {
        ixn_netdev_stop(e->netdev);
        if (e->netdev->shutdown) e->netdev->shutdown(e->netdev);
        ixn_netdev_unregister(e->netdev);
    }
    if (e->rx_descs) ixn_free(e->rx_descs);
    if (e->tx_descs) ixn_free(e->tx_descs);
    if (e->rx_buffers) ixn_free(e->rx_buffers);
    if (e->tx_buffers) ixn_free(e->tx_buffers);
    ixn_memset(e, 0, sizeof(*e));
}
