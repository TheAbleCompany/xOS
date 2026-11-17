/*
 * drivers/ahci.h -- polling-овый AHCI (SATA) драйвер.
 */

#ifndef AHCI_H
#define AHCI_H

#include "../lib/common.h"

/* Сколько SATA-портов (суммарно по всем HBA) отслеживает драйвер. */
#define AHCI_MAX_PORTS 32

#endif /* AHCI_H */
