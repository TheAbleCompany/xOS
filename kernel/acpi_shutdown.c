#include "acpi.h"
#include "descriptor_tables/fadt.h"
#include "../lib/common.h"
#include "../boot/limine.h"

extern void debug_msg(const char *msg);
extern void serial_puts(const char *s);
extern void debug_hex(uint64_t val);

extern uint64_t limine_hhdm_offset;

static int find_s5(
    uint8_t *aml,
    uint32_t length,
    uint16_t *a,
    uint16_t *b)
{
    for (uint32_t i = 0; i + 10 < length; i++) {

        if (aml[i]     != 0x08 ||
            aml[i + 1] != '_'  ||
            aml[i + 2] != 'S'  ||
            aml[i + 3] != '5'  ||
            aml[i + 4] != '_')
            continue;

        /* _S5_ must contain PackageOp */
        if (aml[i + 5] != 0x12)
            continue;
        serial_puts("ACPI: _S5_ bytes: ");

for (uint32_t j = 5; j < 40 && i + j < length; j++) {
    debug_hex(aml[i + j]);
    serial_puts(" ");
}

serial_puts("\n");

        /*
         * QEMU's AML:
         *
         * 08 "_S5_" 12 06 04 00 ...
         *
         * The actual SLP_TYP values in this firmware
         * are the next two integer objects.
         */
        uint32_t p = i + 7;

        if (p + 1 >= length)
            return 0;

        *a = aml[p];
        *b = aml[p + 1];

        return 1;
    }

    return 0;
}

void acpi_shutdown(void)
{
    debug_msg("--------------------------------");
    debug_msg("ACPI SHUTDOWN");
    debug_msg("--------------------------------");

    uint32_t pm1a = getPM1aControlBlock();

    if (pm1a == 0) {
        debug_msg("ACPI: PM1a unavailable");
        return;
    }

    /*
     * QEMU DSDT:
     *
     * _S5_ -> SLP_TYP = 4
     *
     * PM1_CNT:
     *   SLP_TYP bits = 10..12
     *   SLP_EN        = bit 13
     */

    uint16_t slp_typ = 4;

    uint16_t shutdown =
        (uint16_t)((slp_typ << 10) | (1 << 13));

    serial_puts("ACPI: PM1a=");
    debug_hex(pm1a);
    serial_puts("\n");

    serial_puts("ACPI: shutdown=");
    debug_hex(shutdown);
    serial_puts("\n");

    debug_msg("ACPI: writing PM1a");

    outw((uint16_t)pm1a, shutdown);

    /*
     * Если ACPI/QEMU принял S5,
     * сюда выполнение обычно не вернётся.
     */

    debug_msg("ACPI: shutdown returned");
}