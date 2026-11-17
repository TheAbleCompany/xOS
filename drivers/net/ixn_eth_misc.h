#ifndef IXN_ETH_MISC_H
#define IXN_ETH_MISC_H

#include "ixn_types.h"
#include "ixn_netdev.h"
#include "ixn_pci.h"

#define IXN_ETH_MISC_MAX_DEVS 4u

typedef struct {
    ixn_netdev_t *netdev;
    ixn_u8 volatile *mmio;
    ixn_u32 io_base;
    ixn_u8 bus, slot, func;
    ixn_u16 vendor, device;
    ixn_u8 irq;
    ixn_u8 chip_type;
    ixn_u8 mac[IXN_ETH_ALEN];
    ixn_u8 *rx_buf;
    ixn_u32 rx_offset;
    ixn_u8 *tx_buf;
} ixn_eth_misc_t;

#define IXN_ETH_MISC_ATHEROS_L1  1u
#define IXN_ETH_MISC_BROADCOM_BCM572x 2u
#define IXN_ETH_MISC_MARVELL_YUKON 3u
#define IXN_ETH_MISC_VIA_RHINE 4u

int ixn_eth_misc_probe(void);
void ixn_eth_misc_poll(ixn_netdev_t *dev, ixn_u32 budget);

#endif
