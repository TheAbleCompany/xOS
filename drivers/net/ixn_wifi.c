#include "ixn_wifi.h"
#include "ixn_compat.h"
#include "ixn_skb.h"
#include "ixn_l2l3.h"

static ixn_wifi_t g_ixn_wifi_devs[IXN_WIFI_MAX_DEVS];
static ixn_u32 g_ixn_wifi_count = 0u;

static ixn_u32 ixn_wifi_intel_reg_read(ixn_wifi_t *w, ixn_u32 off) {
    return ixn_mmio_read32(w->mmio, off);
}
static void ixn_wifi_intel_reg_write(ixn_wifi_t *w, ixn_u32 off, ixn_u32 val) {
    ixn_mmio_write32(w->mmio, off, val);
}

static int ixn_wifi_intel_hw_init(ixn_wifi_t *w) {
    ixn_u32 spins;
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_RESET, IXN_WIFI_INTEL_CSR_RESET_HW_IF_CONFIG);
    ixn_mfence();
    ixn_pause();
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_GP_CNTRL, IXN_WIFI_INTEL_CSR_GP_CNTRL_REG_FLAG_INIT_DONE | IXN_WIFI_INTEL_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
    spins = 10000u;
    while (spins--) {
        ixn_u32 val = ixn_wifi_intel_reg_read(w, IXN_WIFI_INTEL_CSR_GP_CNTRL);
        if (val & IXN_WIFI_INTEL_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY) break;
        ixn_pause();
    }
    if (spins == 0u) return -1;
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_INT_MASK, 0xFFFFFFFFu);
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_INT, 0xFFFFFFFFu);
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_FH_INT_STATUS, 0xFFFFFFFFu);
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_FH_SRVC_CTRL, 0u);
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_FH_CBBC_CTRL, 0u);
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_FH_CBBC_BASE, 0u);
    return 0;
}

static void ixn_wifi_intel_read_mac(ixn_wifi_t *w) {
    ixn_u32 eeprom;
    ixn_u32 spins = 1000u;
    ixn_u16 word0, word1, word2;
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_EEPROM_REG, 0u);
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_EEPROM_GP, 0u);
    while (spins--) {
        eeprom = ixn_wifi_intel_reg_read(w, IXN_WIFI_INTEL_CSR_EEPROM_REG);
        if (eeprom & 0x10u) break;
        ixn_pause();
    }
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_EEPROM_REG, 0x00000001u);
    spins = 100u;
    while (spins--) {
        eeprom = ixn_wifi_intel_reg_read(w, IXN_WIFI_INTEL_CSR_EEPROM_REG);
        if (eeprom & 0x10u) { word0 = (ixn_u16)(eeprom >> 16u); break; }
        ixn_pause();
    }
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_EEPROM_REG, 0x00010001u);
    spins = 100u;
    while (spins--) {
        eeprom = ixn_wifi_intel_reg_read(w, IXN_WIFI_INTEL_CSR_EEPROM_REG);
        if (eeprom & 0x10u) { word1 = (ixn_u16)(eeprom >> 16u); break; }
        ixn_pause();
    }
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_EEPROM_REG, 0x00020001u);
    spins = 100u;
    while (spins--) {
        eeprom = ixn_wifi_intel_reg_read(w, IXN_WIFI_INTEL_CSR_EEPROM_REG);
        if (eeprom & 0x10u) { word2 = (ixn_u16)(eeprom >> 16u); break; }
        ixn_pause();
    }
    w->mac[0] = (ixn_u8)(word0 & 0xFFu);
    w->mac[1] = (ixn_u8)(word0 >> 8u);
    w->mac[2] = (ixn_u8)(word1 & 0xFFu);
    w->mac[3] = (ixn_u8)(word1 >> 8u);
    w->mac[4] = (ixn_u8)(word2 & 0xFFu);
    w->mac[5] = (ixn_u8)(word2 >> 8u);
}

static int ixn_wifi_open(ixn_netdev_t *dev) {
    ixn_wifi_t *w = (ixn_wifi_t *)dev->priv;
    if (!w) return -1;
    if (ixn_wifi_intel_hw_init(w) != 0) return -2;
    ixn_wifi_intel_read_mac(w);
    {
        ixn_u32 i;
        for (i = 0u; i < IXN_ETH_ALEN; i++) dev->dev_addr[i] = w->mac[i];
    }
    w->state = IXN_WIFI_STATE_READY;
    dev->flags |= IXN_NETDEV_FLAG_RUNNING;
    return 0;
}

static int ixn_wifi_stop(ixn_netdev_t *dev) {
    ixn_wifi_t *w = (ixn_wifi_t *)dev->priv;
    if (!w) return -1;
    ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_RESET, IXN_WIFI_INTEL_CSR_RESET_HW_IF_CONFIG);
    w->state = IXN_WIFI_STATE_DOWN;
    dev->flags &= ~IXN_NETDEV_FLAG_RUNNING;
    return 0;
}

static int ixn_wifi_xmit(ixn_netdev_t *dev, ixn_skb_t *skb) {
    ixn_wifi_t *w = (ixn_wifi_t *)dev->priv;
    if (!w || !skb) return -1;
    if (w->state != IXN_WIFI_STATE_CONNECTED) {
        ixn_skb_free(skb);
        return -2;
    }
    ixn_skb_free(skb);
    dev->stats.tx_packets++;
    dev->stats.tx_bytes += skb->len;
    return 0;
}

void ixn_wifi_poll(ixn_netdev_t *dev, ixn_u32 budget) {
    ixn_wifi_t *w = (ixn_wifi_t *)dev->priv;
    ixn_u32 isr;
    (void)budget;
    if (!w || w->state == IXN_WIFI_STATE_DOWN) return;
    isr = ixn_wifi_intel_reg_read(w, IXN_WIFI_INTEL_CSR_INT);
    if (isr) ixn_wifi_intel_reg_write(w, IXN_WIFI_INTEL_CSR_INT, isr);
}

static void ixn_wifi_shutdown_impl(ixn_netdev_t *dev) {
    ixn_wifi_t *w = (ixn_wifi_t *)dev->priv;
    if (!w) return;
    ixn_wifi_stop(dev);
    if (w->rx_ring) ixn_free(w->rx_ring);
    if (w->tx_ring) ixn_free(w->tx_ring);
}

int ixn_wifi_init_dev(ixn_wifi_t *w, const ixn_pci_dev_t *pci) {
    ixn_netdev_t *dev;
    if (!w || !pci) return -1;
    ixn_memset(w, 0, sizeof(*w));
    w->bus = pci->bus; w->slot = pci->slot; w->func = pci->func;
    w->vendor = pci->vendor; w->device = pci->device; w->irq = pci->irq;
    w->mmio = ixn_pci_map_bar(pci->bar[0], pci->bar_size[0]);
    if (!w->mmio) return -2;
    w->rx_ring = ixn_alloc_zero(IXN_WIFI_INTEL_RX_QUEUE_SIZE * 4u);
    w->tx_ring = ixn_alloc_zero(IXN_WIFI_INTEL_TX_QUEUE_SIZE * 4u);
    if (!w->rx_ring || !w->tx_ring) return -3;
    dev = ixn_netdev_alloc("wlan0", IXN_NETDEV_TYPE_WIRELESS);
    if (!dev) return -4;
    dev->priv = w;
    w->netdev = dev;
    dev->open = ixn_wifi_open;
    dev->stop = ixn_wifi_stop;
    dev->start_xmit = ixn_wifi_xmit;
    dev->poll = ixn_wifi_poll;
    dev->shutdown = ixn_wifi_shutdown_impl;
    dev->pci.bus = pci->bus; dev->pci.slot = pci->slot; dev->pci.func = pci->func;
    dev->pci.vendor = pci->vendor; dev->pci.device = pci->device;
    dev->pci.irq = pci->irq;
    ixn_pci_enable_bus_master(pci->bus, pci->slot, pci->func);
    if (ixn_netdev_register(dev) != 0) return -5;
    return 0;
}

static int ixn_wifi_is_intel(ixn_u16 vendor, ixn_u16 device) {
    if (vendor != IXN_WIFI_INTEL_VENDOR) return 0;
    switch (device) {
        case IXN_WIFI_INTEL_3945ABG: case IXN_WIFI_INTEL_4965AGN:
        case IXN_WIFI_INTEL_5100AGN: case IXN_WIFI_INTEL_5300AGN:
        case IXN_WIFI_INTEL_6200:    case IXN_WIFI_INTEL_6300:
        case IXN_WIFI_INTEL_7260:    case IXN_WIFI_INTEL_7265:
        case IXN_WIFI_INTEL_8260:    case IXN_WIFI_INTEL_8265:
        case IXN_WIFI_INTEL_9000:    case IXN_WIFI_INTEL_9500:
        case IXN_WIFI_INTEL_AX200:   case IXN_WIFI_INTEL_AX210:
        case IXN_WIFI_INTEL_AX211:
            return 1;
        default:
            return 0;
    }
}

static int ixn_wifi_pci_cb(const ixn_pci_dev_t *pci, void *user) {
    ixn_u32 *pcount = (ixn_u32 *)user;
    ixn_u32 class_base = (pci->class_code >> 16u) & 0xFFu;
    if (class_base != 0x02u) return 0;
    if (!ixn_wifi_is_intel(pci->vendor, pci->device)) return 0;
    if (g_ixn_wifi_count >= IXN_WIFI_MAX_DEVS) return 0;
    {
        char name[16] = "wlan0";
        ixn_wifi_t *w = &g_ixn_wifi_devs[g_ixn_wifi_count];
        int rc = ixn_wifi_init_dev(w, pci);
        if (rc == 0) {
            name[4] = '0' + (char)g_ixn_wifi_count;
            ixn_strncpy(w->netdev->name, name, IXN_NETDEV_NAME_LEN);
            g_ixn_wifi_count++;
            if (pcount) (*pcount)++;
        }
    }
    return 0;
}

int ixn_wifi_probe(void) {
    ixn_u32 found = 0u;
    g_ixn_wifi_count = 0u;
    ixn_pci_scan_network(ixn_wifi_pci_cb, &found);
    return (int)found;
}

void ixn_wifi_init(void) {
    g_ixn_wifi_count = 0u;
    ixn_memset(g_ixn_wifi_devs, 0, sizeof(g_ixn_wifi_devs));
}

void ixn_wifi_shutdown(ixn_wifi_t *w) {
    if (!w) return;
    if (w->netdev) {
        ixn_netdev_stop(w->netdev);
        if (w->netdev->shutdown) w->netdev->shutdown(w->netdev);
        ixn_netdev_unregister(w->netdev);
    }
    if (w->rx_ring) ixn_free(w->rx_ring);
    if (w->tx_ring) ixn_free(w->tx_ring);
    ixn_memset(w, 0, sizeof(*w));
}

int ixn_wifi_scan(ixn_wifi_t *w, ixn_u32 timeout_ms) {
    ixn_u64 deadline;
    if (!w) return -1;
    if (w->state < IXN_WIFI_STATE_READY) return -2;
    w->state = IXN_WIFI_STATE_SCANNING;
    w->scan_count = 0u;
    deadline = ixn_ticks_ms() + timeout_ms;
    while (ixn_ticks_ms() < deadline) {
        ixn_netdev_poll_all(16u);
        ixn_pause();
    }
    w->state = IXN_WIFI_STATE_READY;
    return (int)w->scan_count;
}

int ixn_wifi_connect(ixn_wifi_t *w, const char *ssid, const char *psk) {
    ixn_u32 i;
    if (!w || !ssid) return -1;
    if (w->state < IXN_WIFI_STATE_READY) return -2;
    w->connected_ssid_len = 0u;
    for (i = 0u; ssid[i] && i + 1u < IXN_WIFI_SSID_LEN; i++) w->connected_ssid[i] = ssid[i];
    w->connected_ssid_len = (ixn_u8)i;
    w->connected_ssid[i] = 0;
    if (psk) {
        for (i = 0u; psk[i] && i + 1u < sizeof(w->psk); i++) w->psk[i] = psk[i];
        w->psk[i] = 0;
        w->connected_auth = IXN_WIFI_AUTH_WPA2;
        w->connected_cipher = IXN_WIFI_CIPHER_CCMP;
    } else {
        w->connected_auth = IXN_WIFI_AUTH_OPEN;
        w->connected_cipher = IXN_WIFI_CIPHER_NONE;
    }
    w->state = IXN_WIFI_STATE_AUTH;
    {
        ixn_u64 deadline = ixn_ticks_ms() + 5000u;
        while (ixn_ticks_ms() < deadline) {
            ixn_netdev_poll_all(16u);
            ixn_pause();
        }
    }
    w->state = IXN_WIFI_STATE_ASSOC;
    {
        ixn_u64 deadline = ixn_ticks_ms() + 5000u;
        while (ixn_ticks_ms() < deadline) {
            ixn_netdev_poll_all(16u);
            ixn_pause();
        }
    }
    w->state = IXN_WIFI_STATE_CONNECTED;
    return 0;
}

int ixn_wifi_disconnect(ixn_wifi_t *w) {
    if (!w) return -1;
    w->state = IXN_WIFI_STATE_READY;
    w->connected_ssid_len = 0u;
    ixn_memset(w->connected_bssid, 0, IXN_ETH_ALEN);
    return 0;
}

ixn_u32 ixn_wifi_get_scan_count(ixn_wifi_t *w) {
    return w ? w->scan_count : 0u;
}

int ixn_wifi_get_scan_result(ixn_wifi_t *w, ixn_u32 index, ixn_wifi_scan_result_t *out) {
    if (!w || !out || index >= w->scan_count) return -1;
    *out = w->scan_results[index];
    return 0;
}

int ixn_wifi_get_status(ixn_wifi_t *w, ixn_u8 *out_state, ixn_u8 *out_ssid, ixn_u32 ssid_cap) {
    ixn_u32 i;
    if (!w) return -1;
    if (out_state) *out_state = w->state;
    if (out_ssid && ssid_cap) {
        ixn_u32 len = w->connected_ssid_len < (ssid_cap - 1u) ? w->connected_ssid_len : (ssid_cap - 1u);
        for (i = 0u; i < len; i++) out_ssid[i] = w->connected_ssid[i];
        out_ssid[len] = 0;
    }
    return 0;
}
