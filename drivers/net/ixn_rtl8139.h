#ifndef IXN_RTL8139_H
#define IXN_RTL8139_H

#include "ixn_types.h"
#include "ixn_netdev.h"
#include "ixn_pci.h"

#define IXN_RTL8139_VENDOR_REALTEK 0x10ECu
#define IXN_RTL8139_DEVICE_8139    0x8139u
#define IXN_RTL8139_DEVICE_8129    0x8129u
#define IXN_RTL810X_DEVICE_8101    0x8136u
#define IXN_RTL8168_DEVICE_8168    0x8168u
#define IXN_RTL8168_DEVICE_8161    0x8161u
#define IXN_RTL8111_DEVICE_8168    0x8168u
#define IXN_RTL8125_DEVICE_8125    0x8125u
#define IXN_RTL8111E_DEVICE        0x8168u
#define IXN_RTL8111F_DEVICE        0x8168u
#define IXN_RTL8169_DEVICE_8169    0x8169u

#define IXN_RTL8168_VENDOR_REALTEK 0x10ECu
#define IXN_RTL8169_DEVICE_8169    0x8169u

#define IXN_ATHEROS_VENDOR         0x1969u
#define IXN_ATHEROS_AR8161         0x1091u
#define IXN_ATHEROS_AR8162         0x1090u
#define IXN_ATHEROS_AR8171         0x10A1u
#define IXN_ATHEROS_AR8172         0x10A0u
#define IXN_ATHEROS_AR8131         0x1063u
#define IXN_ATHEROS_AR8132         0x1062u
#define IXN_ATHEROS_L1E            0x1026u
#define IXN_ATHEROS_L2E            0x2048u

#define IXN_BROADCOM_NET_VENDOR    0x14E4u
#define IXN_BROADCOM_BCM57780      0x1692u
#define IXN_BROADCOM_BCM5720       0x165Fu
#define IXN_BROADCOM_BCM5722       0x165Au
#define IXN_BROADCOM_BCM5709       0x1639u
#define IXN_BROADCOM_BCM5716       0x163Bu

#define IXN_MARVELL_VENDOR         0x11ABu
#define IXN_MARVELL_YUKON_8053     0x4363u
#define IXN_MARVELL_YUKON_8071     0x4364u
#define IXN_MARVELL_YUKON_8072     0x4365u
#define IXN_MARVELL_YUKON_8055     0x4366u
#define IXN_MARVELL_YUKON_8050     0x4367u

#define IXN_VIA_NET_VENDOR         0x1106u
#define IXN_VIA_RHINE_3043         0x3043u
#define IXN_VIA_RHINE_3053         0x3053u
#define IXN_VIA_RHINE_3106         0x3106u

#define IXN_RALINK_USB_VENDOR      0x148Fu
#define IXN_RALINK_RT5370          0x5370u
#define IXN_RALINK_RT5572          0x5572u
#define IXN_RALINK_RT3070          0x3070u
#define IXN_RALINK_RT2870          0x2870u
#define IXN_RALINK_RT3370          0x3370u
#define IXN_RALINK_RT8070          0x8070u

#define IXN_REALTEK_USB_VENDOR     0x0BDAu
#define IXN_REALTEK_RTL8192CU      0x8176u
#define IXN_REALTEK_RTL8188CU      0x8176u
#define IXN_REALTEK_RTL8188EU      0x8179u
#define IXN_REALTEK_RTL8812AU      0x8812u
#define IXN_REALTEK_RTL8821AU      0x0811u

#define IXN_ATHEROS_USB_VENDOR     0x0CF3u
#define IXN_ATHEROS_AR9271         0x9271u
#define IXN_ATHEROS_AR7010         0x7010u

#define IXN_RTL8139_IDR0    0x00u
#define IXN_RTL8139_MAR0    0x08u
#define IXN_RTL8139_TSD0    0x10u
#define IXN_RTL8139_TSAD0   0x20u
#define IXN_RTL8139_RBSTART 0x30u
#define IXN_RTL8139_ERBCR   0x34u
#define IXN_RTL8139_ERSR    0x36u
#define IXN_RTL8139_CR      0x37u
#define IXN_RTL8139_CAPR    0x38u
#define IXN_RTL8139_CBR     0x3Au
#define IXN_RTL8139_IMR     0x3Cu
#define IXN_RTL8139_ISR     0x3Eu
#define IXN_RTL8139_TCR     0x40u
#define IXN_RTL8139_RCR     0x44u
#define IXN_RTL8139_TCTR    0x48u
#define IXN_RTL8139_MPC     0x4Cu
#define IXN_RTL8139_9346CR  0x50u
#define IXN_RTL8139_CONFIG1 0x52u
#define IXN_RTL8139_CONFIG3 0x59u
#define IXN_RTL8139_CONFIG4 0x5Au
#define IXN_RTL8139_BMCR    0x62u
#define IXN_RTL8139_BMSR    0x64u

/* ChipCmd (0x37): биты как у настоящего RTL8139 и в QEMU:
 * RST=0x10, RxEnb=0x08, TxEnb=0x04; bit0 -- read-only "буфер пуст". */
#define IXN_RTL8139_CR_RST   0x10u
#define IXN_RTL8139_CR_RE    0x08u
#define IXN_RTL8139_CR_TE    0x04u
#define IXN_RTL8139_CR_BUFE  0x01u

#define IXN_RTL8139_RCR_AAP    0x01u
#define IXN_RTL8139_RCR_APM    0x02u
#define IXN_RTL8139_RCR_AM     0x04u
#define IXN_RTL8139_RCR_AB     0x08u
#define IXN_RTL8139_RCR_AR     0x10u
#define IXN_RTL8139_RCR_WRAP   0x80u
#define IXN_RTL8139_RCR_RCV32K 0x08u

#define IXN_RTL8139_TCR_IFG    0x03000000u
#define IXN_RTL8139_TCR_TXRR   0x00000F00u

#define IXN_RTL8139_ISR_ROK   0x01u
#define IXN_RTL8139_ISR_RER   0x02u
#define IXN_RTL8139_ISR_TOK   0x04u
#define IXN_RTL8139_ISR_TER   0x08u
#define IXN_RTL8139_ISR_RXOVW 0x10u
#define IXN_RTL8139_ISR_PUN   0x20u
#define IXN_RTL8139_ISR_FOV   0x40u
#define IXN_RTL8139_ISR_LEN   0x80u

/* Размер RX-кольца, о котором "договорились" с контроллером:
 * NIC/эмулятор делают кольцо 8192 << ((RCR >> 11) & 3) -- при нашем
 * RCR (без битов 11-12) это ровно 8192.  Арифметика CAPR обязана
 * считаться по ЭТОМУ модулю, иначе указатели рассинхронизируются. */
#define IXN_RTL8139_RX_RING_SIZE 8192u
/* Сколько байт выделяем под RX-буфер: кольцо + 16-байтовый guard + место
 * для одного кадра: при установленном RCR_WRAP карта пишет кадр,
 * пересекающий конец кольца, НЕПОРЫВНО за его концом. */
#define IXN_RTL8139_RX_BUF_SIZE  (IXN_RTL8139_RX_RING_SIZE + 16u + 1500u)
#define IXN_RTL8139_TX_BUF_SIZE 1792u
#define IXN_RTL8139_TX_DESC_NUM 4u

typedef struct {
    ixn_netdev_t *netdev;
    ixn_u32 io_base;
    ixn_u8 bus, slot, func;
    ixn_u16 vendor, device;
    ixn_u8 irq;
    ixn_u8 *rx_buffer;
    ixn_u32 rx_buf_offset;
    ixn_u8 *tx_buffers[IXN_RTL8139_TX_DESC_NUM];
    ixn_u32 tx_desc;
    ixn_u8 mac[IXN_ETH_ALEN];
} ixn_rtl8139_t;

int ixn_rtl8139_probe(void);
int ixn_rtl8139_init_dev(ixn_rtl8139_t *r, const ixn_pci_dev_t *pci);
void ixn_rtl8139_shutdown(ixn_rtl8139_t *r);
void ixn_rtl8139_poll(ixn_netdev_t *dev, ixn_u32 budget);

#endif
