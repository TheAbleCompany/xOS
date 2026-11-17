#ifndef IXN_PCI_H
#define IXN_PCI_H

#include "ixn_types.h"
#include "ixn_netdev.h"

#define IXN_PCI_CLASS_NETWORK_ETHERNET 0x020000u
#define IXN_PCI_CLASS_NETWORK_OTHER    0x028000u

#define IXN_PCI_BAR_MEM        0u
#define IXN_PCI_BAR_IO         1u
#define IXN_PCI_BAR_64BIT      2u
#define IXN_PCI_BAR_PREFETCH   3u

#define IXN_PCI_CMD_IO         0x0001u
#define IXN_PCI_CMD_MEM        0x0002u
#define IXN_PCI_CMD_BUS_MASTER 0x0004u
#define IXN_PCI_CMD_INTX_DIS   0x0400u

typedef struct {
    ixn_u8 bus;
    ixn_u8 slot;
    ixn_u8 func;
    ixn_u16 vendor;
    ixn_u16 device;
    ixn_u32 class_code;
    ixn_u8 irq;
    ixn_u8 revision;
    ixn_u64 bar[6];
    ixn_u64 bar_size[6];
    ixn_u8 bar_type[6];
} ixn_pci_dev_t;

typedef int (*ixn_pci_scan_fn)(const ixn_pci_dev_t *dev, void *user);

void ixn_pci_scan(ixn_pci_scan_fn fn, void *user);
int ixn_pci_scan_network(ixn_pci_scan_fn fn, void *user);
ixn_u8 ixn_pci_read_byte(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off);
ixn_u16 ixn_pci_read_word(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off);
ixn_u32 ixn_pci_read_dword(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off);
void ixn_pci_write_byte(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off, ixn_u8 val);
void ixn_pci_write_word(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off, ixn_u16 val);
void ixn_pci_write_dword(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off, ixn_u32 val);
void ixn_pci_enable_bus_master(ixn_u8 bus, ixn_u8 slot, ixn_u8 func);
void ixn_pci_fill_bars(ixn_pci_dev_t *dev);
ixn_u8 volatile *ixn_pci_map_bar(ixn_u64 bar, ixn_u64 size);

#endif
