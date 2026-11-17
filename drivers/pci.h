/*
 * drivers/pci.h -- minimal PCI enumeration interface.
 */

#ifndef PCI_H
#define PCI_H

/*
 * Walk PCI bus 0 and dump vendor/device/class of every device to
 * the debug serial port.  Self-contained; safe to call once
 * during init.
 */
void pci_init(void);

#endif /* PCI_H */
