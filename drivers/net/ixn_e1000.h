#ifndef IXN_E1000_H
#define IXN_E1000_H

#include "ixn_types.h"
#include "ixn_netdev.h"
#include "ixn_pci.h"

#define IXN_E1000_VENDOR_INTEL 0x8086u
#define IXN_E1000_DEVICE_82540EM 0x100Eu
#define IXN_E1000_DEVICE_82573L  0x109Au
#define IXN_E1000_DEVICE_82574L  0x10D3u
#define IXN_E1000_DEVICE_82577LM 0x10EAu
#define IXN_E1000_DEVICE_I219LM  0x156Fu
#define IXN_E1000_DEVICE_I219V   0x1570u
#define IXN_E1000_DEVICE_82567LM 0x10CBu

#define IXN_E1000_REG_CTRL     0x0000u
#define IXN_E1000_REG_STATUS   0x0008u
#define IXN_E1000_REG_EECD     0x0010u
#define IXN_E1000_REG_EERD     0x0014u
#define IXN_E1000_REG_FCT      0x0030u
#define IXN_E1000_REG_FCTTV    0x0170u
#define IXN_E1000_REG_VET      0x0038u
#define IXN_E1000_REG_CTRL_EXT 0x0018u
#define IXN_E1000_REG_MDIC     0x0020u
#define IXN_E1000_REG_ICR      0x00C0u
#define IXN_E1000_REG_ITR      0x00C4u
#define IXN_E1000_REG_ICS      0x00C8u
#define IXN_E1000_REG_IMS      0x00D0u
#define IXN_E1000_REG_IMC      0x00D8u
#define IXN_E1000_REG_RCTL     0x0100u
#define IXN_E1000_REG_TCTL     0x0400u
#define IXN_E1000_REG_RDBAL    0x2800u
#define IXN_E1000_REG_RDBAH    0x2804u
#define IXN_E1000_REG_RDLEN    0x2808u
#define IXN_E1000_REG_RDH      0x2810u
#define IXN_E1000_REG_RDT      0x2818u
#define IXN_E1000_REG_RDTR     0x2820u
#define IXN_E1000_REG_TDBAL    0x3800u
#define IXN_E1000_REG_TDBAH    0x3804u
#define IXN_E1000_REG_TDLEN    0x3808u
#define IXN_E1000_REG_TDH      0x3810u
#define IXN_E1000_REG_TDT      0x3818u
#define IXN_E1000_REG_TIDV     0x3820u
#define IXN_E1000_REG_RA       0x5400u
#define IXN_E1000_REG_MTA      0x5200u
#define IXN_E1000_REG_PBA      0x1000u
#define IXN_E1000_REG_TIPG     0x0410u

#define IXN_E1000_CTRL_RST     0x04000000u
#define IXN_E1000_CTRL_SLU     0x00000040u
#define IXN_E1000_CTRL_ASDE    0x00000020u
#define IXN_E1000_CTRL_FRCSPD  0x00000800u
#define IXN_E1000_CTRL_FRCDPX  0x00000400u

#define IXN_E1000_RCTL_EN      0x00000002u
#define IXN_E1000_RCTL_BAM     0x00008000u
#define IXN_E1000_RCTL_SZ_2048 0x00000000u
#define IXN_E1000_RCTL_SZ_4096 0x00030000u
#define IXN_E1000_RCTL_BSIZE_MASK 0x00030000u
#define IXN_E1000_RCTL_SECRC   0x04000000u
#define IXN_E1000_RCTL_UPE     0x00000008u
#define IXN_E1000_RCTL_MPE     0x00000010u
#define IXN_E1000_RCTL_LBM     0x00000040u

#define IXN_E1000_TCTL_EN      0x00000002u
#define IXN_E1000_TCTL_PSP     0x00000008u
#define IXN_E1000_TCTL_CT_MASK 0x00000FF0u
#define IXN_E1000_TCTL_COLD    0x003F0000u
#define IXN_E1000_TCTL_RTLC    0x01000000u

#define IXN_E1000_ICR_RXT0     0x00000040u
#define IXN_E1000_ICR_TXDW     0x00000001u
#define IXN_E1000_ICR_LSC      0x00000004u
#define IXN_E1000_ICR_RXDMT0   0x00000010u

#define IXN_E1000_EERD_START   0x00000001u
#define IXN_E1000_EERD_DONE    0x00000010u

#define IXN_E1000_RX_DESC_NUM  64u
#define IXN_E1000_TX_DESC_NUM  64u
#define IXN_E1000_PKT_BUF_SIZE 2048u

#define IXN_E1000_TXD_CMD_EOP  0x01u
#define IXN_E1000_TXD_CMD_IFCS 0x02u
#define IXN_E1000_TXD_CMD_IC   0x04u
#define IXN_E1000_TXD_CMD_RS   0x08u
#define IXN_E1000_TXD_CMD_RPS  0x10u
#define IXN_E1000_TXD_STAT_DD  0x01u

typedef struct {
    ixn_u64 buffer_addr;
    ixn_u16 length;
    ixn_u16 csum;
    ixn_u8 status;
    ixn_u8 errors;
    ixn_u16 special;
} ixn_e1000_rx_desc_t;

typedef struct {
    ixn_u64 buffer_addr;
    ixn_u16 length;
    ixn_u8 cso;
    ixn_u8 cmd;
    ixn_u8 status;
    ixn_u8 css;
    ixn_u16 special;
} ixn_e1000_tx_desc_t;

typedef struct {
    ixn_u64 buffer_addr;
    ixn_u16 length;
    ixn_u8 cso;
    ixn_u8 cmd;
    ixn_u8 status;
    ixn_u8 css;
    ixn_u16 special;
    ixn_u32 data_pad;
} ixn_e1000_ext_tx_desc_t;

typedef struct {
    ixn_netdev_t *netdev;
    ixn_u8 volatile *mmio;
    ixn_u32 io_base;
    ixn_u8 bus, slot, func;
    ixn_u16 vendor, device;
    ixn_u8 irq;
    ixn_e1000_rx_desc_t *rx_descs;
    ixn_e1000_tx_desc_t *tx_descs;
    ixn_u8 *rx_buffers;
    ixn_u8 *tx_buffers;
    ixn_u32 rx_head;
    ixn_u32 rx_tail;
    ixn_u32 tx_head;
    ixn_u32 tx_tail;
    ixn_u32 tx_used;
    ixn_u8 mac[IXN_ETH_ALEN];
    ixn_u8 has_eeprom;
} ixn_e1000_t;

int ixn_e1000_probe(void);
int ixn_e1000_init_dev(ixn_e1000_t *e, const ixn_pci_dev_t *pci);
void ixn_e1000_shutdown(ixn_e1000_t *e);
void ixn_e1000_poll(ixn_netdev_t *dev, ixn_u32 budget);

#endif
