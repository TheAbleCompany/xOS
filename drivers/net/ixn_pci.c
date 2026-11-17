#include "ixn_pci.h"
#include "ixn_compat.h"

ixn_u8 ixn_pci_read_byte(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off) {
    ixn_u32 v;
    if (!ixn_pci_read_config_dword(bus, slot, func, (ixn_u8)(off & 0xFCu), &v)) return 0xFFu;
    return (ixn_u8)(v >> ((off & 3u) * 8u));
}
ixn_u16 ixn_pci_read_word(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off) {
    ixn_u32 v;
    if (!ixn_pci_read_config_dword(bus, slot, func, (ixn_u8)(off & 0xFCu), &v)) return 0xFFFFu;
    return (ixn_u16)(v >> ((off & 2u) * 8u));
}
ixn_u32 ixn_pci_read_dword(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off) {
    ixn_u32 v = 0xFFFFFFFFu;
    if (!ixn_pci_read_config_dword(bus, slot, func, (ixn_u8)(off & 0xFCu), &v)) return 0xFFFFFFFFu;
    return v;
}
void ixn_pci_write_byte(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off, ixn_u8 val) {
    ixn_pci_write_config_byte(bus, slot, func, off, val);
}
void ixn_pci_write_word(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off, ixn_u16 val) {
    ixn_pci_write_config_word(bus, slot, func, off, val);
}
void ixn_pci_write_dword(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u8 off, ixn_u32 val) {
    ixn_pci_write_config_dword(bus, slot, func, off, val);
}

void ixn_pci_enable_bus_master(ixn_u8 bus, ixn_u8 slot, ixn_u8 func) {
    ixn_u16 cmd = ixn_pci_read_word(bus, slot, func, 4u);
    cmd |= IXN_PCI_CMD_IO | IXN_PCI_CMD_MEM | IXN_PCI_CMD_BUS_MASTER;
    ixn_pci_write_word(bus, slot, func, 4u, cmd);
}

void ixn_pci_fill_bars(ixn_pci_dev_t *dev) {
    ixn_u8 bar_idx;
    if (!dev) return;
    for (bar_idx = 0u; bar_idx < 6u; bar_idx++) {
        ixn_u32 bar_lo, bar_hi, orig_lo;
        ixn_u8 reg = (ixn_u8)(0x10u + bar_idx * 4u);
        bar_lo = ixn_pci_read_dword(dev->bus, dev->slot, dev->func, reg);
        orig_lo = bar_lo;
        if (bar_lo == 0u || bar_lo == 0xFFFFFFFFu) { dev->bar[bar_idx] = 0u; dev->bar_size[bar_idx] = 0u; continue; }
        if (bar_lo & 1u) {
            dev->bar_type[bar_idx] = IXN_PCI_BAR_IO;
            dev->bar[bar_idx] = bar_lo & 0xFFFCu;
            dev->bar_size[bar_idx] = 32u;
        } else {
            ixn_u8 type = (bar_lo >> 1u) & 3u;
            ixn_u64 addr, size;
            if (type == 0u) {
                dev->bar_type[bar_idx] = IXN_PCI_BAR_MEM;
                ixn_pci_write_dword(dev->bus, dev->slot, dev->func, reg, 0xFFFFFFFFu);
                {
                    ixn_u32 sz_lo = ixn_pci_read_dword(dev->bus, dev->slot, dev->func, reg);
                    ixn_pci_write_dword(dev->bus, dev->slot, dev->func, reg, orig_lo);
                    if (sz_lo == 0u) { dev->bar[bar_idx] = 0u; dev->bar_size[bar_idx] = 0u; continue; }
                    size = (ixn_u64)(sz_lo & 0xFFFFFFF0u);
                    size = size & ~(size - 1u);
                    addr = (ixn_u64)(bar_lo & 0xFFFFFFF0u);
                }
                dev->bar[bar_idx] = addr;
                dev->bar_size[bar_idx] = size;
            } else if (type == 2u) {
                dev->bar_type[bar_idx] = IXN_PCI_BAR_MEM | IXN_PCI_BAR_64BIT;
                bar_hi = ixn_pci_read_dword(dev->bus, dev->slot, dev->func, (ixn_u8)(reg + 4u));
                ixn_pci_write_dword(dev->bus, dev->slot, dev->func, reg, 0xFFFFFFFFu);
                ixn_pci_write_dword(dev->bus, dev->slot, dev->func, (ixn_u8)(reg + 4u), 0xFFFFFFFFu);
                {
                    ixn_u32 sz_lo = ixn_pci_read_dword(dev->bus, dev->slot, dev->func, reg);
                    ixn_u32 sz_hi = ixn_pci_read_dword(dev->bus, dev->slot, dev->func, (ixn_u8)(reg + 4u));
                    ixn_pci_write_dword(dev->bus, dev->slot, dev->func, reg, orig_lo);
                    ixn_pci_write_dword(dev->bus, dev->slot, dev->func, (ixn_u8)(reg + 4u), bar_hi);
                    {
                        ixn_u64 sz = ((ixn_u64)sz_hi << 32u) | (ixn_u64)(sz_lo & 0xFFFFFFF0u);
                        if (sz == 0u) { dev->bar[bar_idx] = 0u; dev->bar_size[bar_idx] = 0u; continue; }
                        sz = sz & ~(sz - 1u);
                        dev->bar[bar_idx] = ((ixn_u64)bar_hi << 32u) | (ixn_u64)(bar_lo & 0xFFFFFFF0u);
                        dev->bar_size[bar_idx] = sz;
                    }
                }
                bar_idx++;
            }
        }
    }
}

ixn_u8 volatile *ixn_pci_map_bar(ixn_u64 bar, ixn_u64 size) {
    (void)size;
    if (!bar) return IXN_NULL;
    return (ixn_u8 volatile *)ixn_phys_to_virt(bar);
}

void ixn_pci_scan(ixn_pci_scan_fn fn, void *user) {
    ixn_u16 bus, slot, func;
    for (bus = 0u; bus < 256u; bus++) {
        for (slot = 0u; slot < 32u; slot++) {
            ixn_u32 id = ixn_pci_read_dword((ixn_u8)bus, (ixn_u8)slot, 0u, 0u);
            if (id == 0xFFFFFFFFu) continue;
            {
                ixn_u32 header = ixn_pci_read_dword((ixn_u8)bus, (ixn_u8)slot, 0u, 0x0Cu);
                ixn_u8 hdr_type = (header >> 16u) & 0x7Fu;
                ixn_u8 func_max = (hdr_type & 0x80u) ? 8u : 1u;
                for (func = 0u; func < func_max; func++) {
                    ixn_u32 fid = ixn_pci_read_dword((ixn_u8)bus, (ixn_u8)slot, (ixn_u8)func, 0u);
                    if (fid == 0xFFFFFFFFu) continue;
                    {
                        ixn_pci_dev_t dev;
                        ixn_memset(&dev, 0, sizeof(dev));
                        dev.bus = (ixn_u8)bus;
                        dev.slot = (ixn_u8)slot;
                        dev.func = (ixn_u8)func;
                        dev.vendor = (ixn_u16)(fid & 0xFFFFu);
                        dev.device = (ixn_u16)(fid >> 16u);
                        dev.class_code = ixn_pci_read_dword((ixn_u8)bus, (ixn_u8)slot, (ixn_u8)func, 0x08u) >> 8u;
                        dev.irq = ixn_pci_read_byte((ixn_u8)bus, (ixn_u8)slot, (ixn_u8)func, 0x3Cu);
                        dev.revision = ixn_pci_read_byte((ixn_u8)bus, (ixn_u8)slot, (ixn_u8)func, 0x08u);
                        ixn_pci_fill_bars(&dev);
                        if (fn) fn(&dev, user);
                    }
                }
            }
        }
    }
}

int ixn_pci_scan_network(ixn_pci_scan_fn fn, void *user) {
    ixn_u16 bus, slot, func;
    int found = 0;
    for (bus = 0u; bus < 256u; bus++) {
        for (slot = 0u; slot < 32u; slot++) {
            ixn_u32 id = ixn_pci_read_dword((ixn_u8)bus, (ixn_u8)slot, 0u, 0u);
            if (id == 0xFFFFFFFFu) continue;
            {
                ixn_u32 header = ixn_pci_read_dword((ixn_u8)bus, (ixn_u8)slot, 0u, 0x0Cu);
                ixn_u8 hdr_type = (header >> 16u) & 0x7Fu;
                ixn_u8 func_max = (hdr_type & 0x80u) ? 8u : 1u;
                for (func = 0u; func < func_max; func++) {
                    ixn_u32 fid = ixn_pci_read_dword((ixn_u8)bus, (ixn_u8)slot, (ixn_u8)func, 0u);
                    if (fid == 0xFFFFFFFFu) continue;
                    {
                        ixn_u32 class_code = ixn_pci_read_dword((ixn_u8)bus, (ixn_u8)slot, (ixn_u8)func, 0x08u) >> 8u;
                        ixn_u8 class_base = (ixn_u8)(class_code >> 16u);
                        if (class_base != 0x02u) continue;
                        {
                            ixn_pci_dev_t dev;
                            ixn_memset(&dev, 0, sizeof(dev));
                            dev.bus = (ixn_u8)bus;
                            dev.slot = (ixn_u8)slot;
                            dev.func = (ixn_u8)func;
                            dev.vendor = (ixn_u16)(fid & 0xFFFFu);
                            dev.device = (ixn_u16)(fid >> 16u);
                            dev.class_code = class_code;
                            dev.irq = ixn_pci_read_byte((ixn_u8)bus, (ixn_u8)slot, (ixn_u8)func, 0x3Cu);
                            dev.revision = ixn_pci_read_byte((ixn_u8)bus, (ixn_u8)slot, (ixn_u8)func, 0x08u);
                            ixn_pci_fill_bars(&dev);
                            found++;
                            if (fn) fn(&dev, user);
                        }
                    }
                }
            }
        }
    }
    return found;
}
