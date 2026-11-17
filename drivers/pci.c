/*
 * ============================================================
 * drivers/pci.c -- minimal PCI configuration-space enumeration.
 * ============================================================
 *
 * Reconstructed module.  The root kernel image does not link
 * pci.o, but drivers/Makefile builds it as part of `make -C
 * drivers`.  The driver provides a tiny enumeration helper that
 * walks bus 0 and remembers (bus, slot, func) triples of the
 * devices it meets; it is self-contained and safe to call once
 * during kernel init or from a debug shell.
 */

#include "pci.h"
#include "../lib/common.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_MAX_BUS   4
#define PCI_MAX_SLOT  32

#define PCI_CLASS_BRIDGE_HOST 0x0600
#define PCI_CLASS_NOT_FOUND   0xFFFF

static inline u32int pci_read_dword(u8int bus, u8int slot,
                                    u8int func, u8int offset)
{
    u32int address =
        (u32int)((bus << 16) | (slot << 11) |
                 (func << 8) | (offset & 0xFC) |
                 0x80000000u);

    outl(PCI_CONFIG_ADDRESS, address);

    return inl(PCI_CONFIG_DATA);
}

static inline u16int pci_get_vendor(u8int bus, u8int slot, u8int func)
{
    return (u16int)(pci_read_dword(bus, slot, func, 0x00) & 0xFFFF);
}

static inline u16int pci_get_class(u8int bus, u8int slot, u8int func)
{
    u32int reg = pci_read_dword(bus, slot, func, 0x08);
    return (u16int)((reg >> 16) & 0xFFFF);
}

void pci_init(void)
{
    debug_puts("PCI: enumerating bus 0\n");

    int found = 0;

    for (u16int bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (u16int slot = 0; slot < PCI_MAX_SLOT; slot++) {
            u16int vendor = pci_get_vendor((u8int)bus, (u8int)slot, 0);

            if (vendor == 0xFFFF)
                continue;

            u16int dev = (u16int)
                ((pci_read_dword((u8int)bus, (u8int)slot, 0, 0x00) >> 16) & 0xFFFF);
            u16int cls = pci_get_class((u8int)bus, (u8int)slot, 0);

            debug_puts("PCI: ");
            debug_putnum(bus);
            debug_putc(':');
            debug_putnum(slot);
            debug_putc(' ');
            debug_puts("vendor=");
            debug_putnum(vendor);
            debug_puts(" device=");
            debug_putnum(dev);
            debug_puts(" class=");
            debug_putnum(cls);
            debug_putc('\n');

            found++;
        }
    }

    debug_puts("PCI: done, devices found: ");
    debug_putnum((uint64_t)found);
    debug_putc('\n');
}
