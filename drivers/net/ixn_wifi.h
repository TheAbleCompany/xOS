#ifndef IXN_WIFI_H
#define IXN_WIFI_H

#include "ixn_types.h"
#include "ixn_netdev.h"
#include "ixn_pci.h"

#define IXN_WIFI_MAX_DEVS 4u
#define IXN_WIFI_MAX_SSIDS 32u
#define IXN_WIFI_SSID_LEN 32u
#define IXN_WIFI_MAX_RATES 12u

#define IXN_WIFI_BAND_2_4GHZ 1u
#define IXN_WIFI_BAND_5GHZ   2u

#define IXN_WIFI_AUTH_OPEN   0u
#define IXN_WIFI_AUTH_WPA    1u
#define IXN_WIFI_AUTH_WPA2   2u
#define IXN_WIFI_AUTH_WPA3   3u

#define IXN_WIFI_CIPHER_NONE 0u
#define IXN_WIFI_CIPHER_WEP  1u
#define IXN_WIFI_CIPHER_TKIP 2u
#define IXN_WIFI_CIPHER_CCMP 3u

#define IXN_WIFI_STATE_DOWN      0u
#define IXN_WIFI_STATE_INIT      1u
#define IXN_WIFI_STATE_READY     2u
#define IXN_WIFI_STATE_SCANNING  3u
#define IXN_WIFI_STATE_AUTH      4u
#define IXN_WIFI_STATE_ASSOC     5u
#define IXN_WIFI_STATE_CONNECTED 6u

#define IXN_WIFI_INTEL_VENDOR 0x8086u
#define IXN_WIFI_INTEL_3945ABG 0x4222u
#define IXN_WIFI_INTEL_4965AGN 0x4230u
#define IXN_WIFI_INTEL_5100AGN 0x4237u
#define IXN_WIFI_INTEL_5300AGN 0x4236u
#define IXN_WIFI_INTEL_6200    0x422Cu
#define IXN_WIFI_INTEL_6300    0x422Bu
#define IXN_WIFI_INTEL_7260    0x08B1u
#define IXN_WIFI_INTEL_7265    0x095Au
#define IXN_WIFI_INTEL_8260    0x24F3u
#define IXN_WIFI_INTEL_8265    0x24FDu
#define IXN_WIFI_INTEL_9000    0x2520u
#define IXN_WIFI_INTEL_9500    0x2720u
#define IXN_WIFI_INTEL_AX200   0x2723u
#define IXN_WIFI_INTEL_AX210   0x51F0u
#define IXN_WIFI_INTEL_AX211   0x51F1u

#define IXN_WIFI_INTEL_CSR_HW_IF_CONFIG_REG  0x000u
#define IXN_WIFI_INTEL_CSR_INT_PERIODIC_REG  0x004u
#define IXN_WIFI_INTEL_CSR_INT               0x008u
#define IXN_WIFI_INTEL_CSR_INT_MASK          0x00Cu
#define IXN_WIFI_INTEL_CSR_FH_INT_STATUS     0x010u
#define IXN_WIFI_INTEL_CSR_RESET             0x020u
#define IXN_WIFI_INTEL_CSR_GP_CNTRL          0x024u
#define IXN_WIFI_INTEL_CSR_HW_REV            0x028u
#define IXN_WIFI_INTEL_CSR_EEPROM_REG        0x02Cu
#define IXN_WIFI_INTEL_CSR_EEPROM_GP         0x030u
#define IXN_WIFI_INTEL_CSR_GIO_REG           0x03Cu
#define IXN_WIFI_INTEL_CSR_UCODE_DRV_GP1     0x054u
#define IXN_WIFI_INTEL_CSR_UCODE_DRV_GP2     0x058u
#define IXN_WIFI_INTEL_CSR_LED_REG           0x094u
#define IXN_WIFI_INTEL_CSR_DRAM_INT_TBL      0x0A0u
#define IXN_WIFI_INTEL_CSR_FH_CBBC_CTRL      0x0D0u
#define IXN_WIFI_INTEL_CSR_FH_CBBC_BASE       0x0D4u
#define IXN_WIFI_INTEL_CSR_FH_MSG_CONFIG      0x0F8u
#define IXN_WIFI_INTEL_CSR_FH_SRVC_CTRL       0x0F0u
#define IXN_WIFI_INTEL_CSR_FH_TCSR_CONFIG     0x080u

#define IXN_WIFI_INTEL_CSR_RESET_HW_IF_CONFIG 0x80000000u
#define IXN_WIFI_INTEL_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY 0x00000002u
#define IXN_WIFI_INTEL_CSR_GP_CNTRL_REG_FLAG_INIT_DONE       0x00000004u
#define IXN_WIFI_INTEL_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ  0x00000008u

#define IXN_WIFI_INTEL_RX_QUEUE_SIZE 256u
#define IXN_WIFI_INTEL_TX_QUEUE_SIZE 256u

typedef struct {
    ixn_u8 ssid[IXN_WIFI_SSID_LEN];
    ixn_u8 ssid_len;
    ixn_u8 bssid[IXN_ETH_ALEN];
    ixn_u8 channel;
    ixn_u8 band;
    ixn_s8 rssi;
    ixn_u8 auth;
    ixn_u8 cipher;
    ixn_u8 rates[IXN_WIFI_MAX_RATES];
    ixn_u8 rate_count;
    ixn_u8 reserved[3];
} ixn_wifi_scan_result_t;

typedef struct {
    ixn_netdev_t *netdev;
    ixn_u8 volatile *mmio;
    ixn_u8 bus, slot, func;
    ixn_u16 vendor, device;
    ixn_u8 irq;
    ixn_u8 mac[IXN_ETH_ALEN];
    ixn_u8 state;
    ixn_u8 channel;
    ixn_u8 band;
    ixn_u8 connected_ssid[IXN_WIFI_SSID_LEN];
    ixn_u8 connected_ssid_len;
    ixn_u8 connected_bssid[IXN_ETH_ALEN];
    ixn_u8 connected_auth;
    ixn_u8 connected_cipher;
    char psk[64];
    ixn_wifi_scan_result_t scan_results[IXN_WIFI_MAX_SSIDS];
    ixn_u32 scan_count;
    void *rx_ring;
    void *tx_ring;
    ixn_u32 rx_idx;
    ixn_u32 tx_idx;
} ixn_wifi_t;

void ixn_wifi_init(void);
int ixn_wifi_probe(void);
int ixn_wifi_init_dev(ixn_wifi_t *w, const ixn_pci_dev_t *pci);
void ixn_wifi_shutdown(ixn_wifi_t *w);
void ixn_wifi_poll(ixn_netdev_t *dev, ixn_u32 budget);

int ixn_wifi_scan(ixn_wifi_t *w, ixn_u32 timeout_ms);
int ixn_wifi_connect(ixn_wifi_t *w, const char *ssid, const char *psk);
int ixn_wifi_disconnect(ixn_wifi_t *w);
ixn_u32 ixn_wifi_get_scan_count(ixn_wifi_t *w);
int ixn_wifi_get_scan_result(ixn_wifi_t *w, ixn_u32 index, ixn_wifi_scan_result_t *out);
int ixn_wifi_get_status(ixn_wifi_t *w, ixn_u8 *out_state, ixn_u8 *out_ssid, ixn_u32 ssid_cap);

#endif
