// ============================================
// 🔒 БЕЗОПАСНЫЙ fadt.c - с защитой от падений
// ============================================

#include "fadt.h"
#include "../../lib/common.h"
extern void debug_msg(const char *msg);
extern void serial_puts(const char *s);
extern void debug_hex(uint64_t val);
FADT* fadt = NULL;

void initFADT()
{
    fadt = NULL;

    ACPISDTHeader *header = findHeader("FACP");

    if (header == NULL) {
        debug_msg("FADT: FACP not found");
        return;
    }

    serial_puts("FADT: header found, length=");
    debug_hex(header->length);
    serial_puts("\n");

    if (header->length < FADT_LEGACY_SIZE) {
        debug_msg("FADT: table too small");
        return;
    }

    fadt = (FADT *)header;

    debug_msg("FADT: initialized successfully");
    serial_puts("FADT: PM1a legacy=");
debug_hex(fadt->PM1a_control_block);
serial_puts("\n");

serial_puts("FADT: X_PM1a address=");
debug_hex(fadt->X_PM1a_control_block.address);
serial_puts("\n");

serial_puts("FADT: X_PM1a space=");
debug_hex(fadt->X_PM1a_control_block.address_space);
serial_puts("\n");

serial_puts("FADT: X_PM1a width=");
debug_hex(fadt->X_PM1a_control_block.bit_width);
serial_puts("\n");

serial_puts("FADT: X_PM1a access=");
debug_hex(fadt->X_PM1a_control_block.access_size);
serial_puts("\n");
}
// ============================================
// 🛡️ БЕЗОПАСНОЕ включение ACPI (с таймаутом!)
// ============================================
void enableACPI()
{
    if (fadt == NULL) {
        debug_msg("ACPI: FADT is NULL");
        return;
    }

    uint32_t smi = fadt->SMI_command_port;
    uint8_t enable = fadt->ACPI_enable;
    uint32_t pm1a = fadt->PM1a_control_block;

    if (smi == 0) {
        debug_msg("ACPI: SMI command port is NULL");
        return;
    }

    if (enable == 0) {
        debug_msg("ACPI: ACPI enable command is NULL");
        return;
    }

    if (pm1a == 0) {
        debug_msg("ACPI: PM1a control block is NULL");
        return;
    }

    uint16_t before = inw(pm1a);

    serial_puts("ACPI: PM1a before enable=");
    debug_hex(before);
    serial_puts("\n");

    /*
     * SCI_EN already set.
     */
    if (before & 1) {
        debug_msg("ACPI: SCI_EN already enabled");
        return;
    }

    debug_msg("ACPI: sending enable command");

    outb((uint16_t)smi, enable);

    /*
     * Wait until SCI_EN becomes 1.
     */
    for (uint32_t i = 0; i < 1000000; i++) {

        uint16_t value = inw(pm1a);

        if (value & 1) {
            debug_msg("ACPI: SCI_EN enabled");
            return;
        }
    }

    debug_msg("ACPI: SCI_EN enable timeout");
}
// ============================================
// 📊 Геттеры для безопасного доступа к полям FADT
// ============================================
int isFADTAvailable()
{
    return (fadt != NULL);
}

uint32_t getSMIPort()
{
    if (fadt == NULL) return 0;
    return fadt->SMI_command_port;
}

uint32_t getPM1aControlBlock()
{
    if (fadt == NULL)
        return 0;

    return fadt->PM1a_control_block;
}

uint32_t getPM1bControlBlock()
{
    if (fadt == NULL)
        return 0;

    return fadt->PM1b_control_block;
}

uint16_t getSCIInterrupt()
{
    if (fadt == NULL) return 0;
    return fadt->SCI_interrupt;
}
uint32_t getDSDTAddress(void)
{
    if (fadt == NULL)
        return 0;

    if (fadt->h.length <
        offsetof(FADT, DSDT_address) +
        sizeof(fadt->DSDT_address))
        return 0;

    return fadt->DSDT_address;
}
