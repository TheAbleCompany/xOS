// ============================================
// 🔒 БЕЗОПАСНЫЙ acpi.c - с HHDM конвертацией адресов
// ============================================

#include "acpi.h"
#include "../lib/common.h"
#include "../boot/limine.h"
#include "descriptor_tables/fadt.h"
extern volatile struct limine_rsdp_request limine_rsdp_request;
extern void debug_msg(const char *msg);
extern void serial_putc(char c);

extern void serial_puts(const char *s);
extern void debug_hex(uint64_t val);
RSDPDescriptor20 *rsdp_descriptor = NULL;
RSDT* rsdt = NULL;
XSDT* xsdt = NULL;

// Внешнее HHDM смещение от Limine
extern uint64_t limine_hhdm_offset;

// Флаг для отладки
static int acpi_initialized = 0;

// ============================================
// 🔑 Конвертация физического адреса ACPI в виртуальный
// ============================================
void *acpi_phys_to_virt(uint64_t phys_addr)
{
    if (phys_addr == 0)
        return NULL;

    return (void *)(uintptr_t)(phys_addr + limine_hhdm_offset);
}

// ============================================
// 🛡️ ЗАЩИЩЁННАЯ валидация RSDP чексуммы
// ============================================
uint8_t validateRSDPChecksum() 
{
    // Защита от NULL
    if (rsdp_descriptor == NULL) {
        return 1; // Ошибка
    }
    
    uint64_t checksum = 0;
    
    // Используем volatile для безопасности
    volatile uint8_t* ptr = (volatile uint8_t*)rsdp_descriptor;
    
    // Читаем байты через volatile
    for (uint8_t i = 0; i < 20; i++) {  // Первые 20 байт (RSDP 1.0)
        checksum += ptr[i];
    }
    
    // Проверка: сумма должна быть 0 (mod 256)
    if ((checksum & 0xFF) != 0) {
        return 1; // Чексумм не прошёл
    }
    
    // Для ACPI 2.0+ проверяем расширенный чексумм
    if (rsdp_descriptor->descriptor10.revision >= 2) {
        checksum = 0;
        uint32_t length = rsdp_descriptor->length;
        
        // Защита от некорректной длины
        if (length < 36 || length > 4096) {
            return 1; // Некорректная длина
        }
        
        for (uint32_t i = 0; i < length; i++) {
            checksum += ptr[i];
        }
        
        if ((checksum & 0xFF) != 0) {
            return 1; // Расширенный чексумм не прошёл
        }
    }
    
    return 0; // Успех
}

// ============================================
// 🛡️ ЗАЩИЩЁННАЯ валидация SDT чексумма
// ============================================
uint8_t validateSDTChecksum(ACPISDTHeader* table_header)
{
    if (table_header == NULL)
        return 0;

    uint32_t length = table_header->length;

    if (length < sizeof(ACPISDTHeader))
        return 0;

    volatile uint8_t *bytes =
        (volatile uint8_t *)table_header;

    uint8_t sum = 0;

    for (uint32_t i = 0; i < length; i++)
        sum += bytes[i];

    return sum == 0;
}
// ============================================
// 🛡️ БЕЗОПАСНЫЙ поиск заголовка таблицы
// ============================================
ACPISDTHeader* findHeader(char* signature)
{
    if (signature == NULL)
        return NULL;

    /*
     * ========================================
     * XSDT — ACPI 2.0+
     * ========================================
     */

    if (xsdt != NULL) {

        uint32_t header_size = sizeof(ACPISDTHeader);
        uint32_t total_length = xsdt->h.length;

        if (total_length >= header_size &&
            total_length <= 4096) {

            uint32_t entries =
                (total_length - header_size) / 8;

            if (entries > 256)
                entries = 256;

            for (uint32_t i = 0; i < entries; i++) {

                uint64_t phys_addr =
                    xsdt->other_SDT[i];

                if (phys_addr == 0)
                    continue;

                ACPISDTHeader *header =
                    (ACPISDTHeader *)acpi_phys_to_virt(
                        phys_addr
                    );

                if (header == NULL)
                    continue;

                if (header->signature[0] == signature[0] &&
                    header->signature[1] == signature[1] &&
                    header->signature[2] == signature[2] &&
                    header->signature[3] == signature[3]) {

                    if (validateSDTChecksum(header))
                        return header;
                    serial_puts("ACPI: FACP length=");
debug_hex(header->length);
serial_puts("\n");

serial_puts("ACPI: FACP revision=");
debug_hex(header->revision);
serial_puts("\n");

serial_puts("ACPI: FACP checksum=");
debug_hex(header->checksum);
serial_puts("\n");
                }
            }
        }
    }

    /*
     * ========================================
     * RSDT — ACPI 1.0 fallback
     * ========================================
     */

    if (rsdt != NULL) {

        uint32_t header_size = sizeof(ACPISDTHeader);
        uint32_t total_length = rsdt->h.length;

        if (total_length >= header_size &&
            total_length <= 4096) {

            uint32_t entries =
                (total_length - header_size) / 4;

            if (entries > 256)
                entries = 256;

            for (uint32_t i = 0; i < entries; i++)
{
    uint32_t entry_addr = rsdt->other_SDT[i];

    if (entry_addr == 0)
        continue;

    ACPISDTHeader* header =
        (ACPISDTHeader*)acpi_phys_to_virt(entry_addr);

    serial_puts("ACPI: RSDT entry ");
    debug_hex(i);
    serial_puts(" phys=");
    debug_hex(entry_addr);
    serial_puts(" ");

    if (header == NULL) {
        serial_puts("NULL\n");
        continue;
    }

    serial_puts("sig=");

    serial_putc(header->signature[0]);
    serial_putc(header->signature[1]);
    serial_putc(header->signature[2]);
    serial_putc(header->signature[3]);

    serial_puts("\n");

    if (header->signature[0] == signature[0] &&
        header->signature[1] == signature[1] &&
        header->signature[2] == signature[2] &&
        header->signature[3] == signature[3]) {

        if (validateSDTChecksum(header))
            return header;
        serial_puts("ACPI: FACP length=");
debug_hex(header->length);
serial_puts("\n");

serial_puts("ACPI: FACP revision=");
debug_hex(header->revision);
serial_puts("\n");

serial_puts("ACPI: FACP checksum=");
debug_hex(header->checksum);
serial_puts("\n");
    }
}
        }
    }

    return NULL;
}
/*
 * ============================================
 * ACPI INITIALIZATION
 * ============================================
 */

int init_acpi_system(void)
{
    debug_msg("--------------------------------");
    debug_msg("INITIALIZING ACPI");
    debug_msg("--------------------------------");

    /*
     * Limine должен передать нам RSDP.
     */
    if (limine_rsdp_request.response == NULL) {
        debug_msg("ACPI: no RSDP response");
        return 0;
    }

    if (limine_rsdp_request.response->address == NULL) {
        debug_msg("ACPI: RSDP address is NULL");
        return 0;
    }

    /*
     * RSDP уже доступен ядру через Limine.
     */
    rsdp_descriptor =
        (RSDPDescriptor20 *)
        limine_rsdp_request.response->address;

    /*
     * Проверяем сигнатуру "RSD PTR ".
     */
    if (rsdp_descriptor->descriptor10.signature[0] != 'R' ||
        rsdp_descriptor->descriptor10.signature[1] != 'S' ||
        rsdp_descriptor->descriptor10.signature[2] != 'D' ||
        rsdp_descriptor->descriptor10.signature[3] != ' ') {

        debug_msg("ACPI: invalid RSDP signature");
        return 0;
    }

    debug_msg("ACPI: RSDP found");

    /*
     * Проверка checksum.
     */
    if (validateRSDPChecksum() != 0) {
        debug_msg("ACPI: RSDP checksum failed");
        return 0;
    }

    debug_msg("ACPI: RSDP checksum OK");

    uint64_t hhdm = limine_hhdm_offset;

    /*
     * XSDT доступен на ACPI 2.0+.
     */
    if (rsdp_descriptor->descriptor10.revision >= 2 &&
        rsdp_descriptor->xsdt_address != 0) {

        xsdt = (XSDT *)(uintptr_t)(
            rsdp_descriptor->xsdt_address + hhdm
        );

        debug_msg("ACPI: XSDT found");
    }

    /*
     * Пока твой findHeader() работает через RSDT,
     * поэтому RSDT обязательно инициализируем.
     */
    if (rsdp_descriptor->descriptor10.rsdt_address == 0) {
        debug_msg("ACPI: RSDT address is NULL");
        return 0;
    }

    rsdt = (RSDT *)(uintptr_t)(
        (uint64_t)rsdp_descriptor->descriptor10.rsdt_address
        + hhdm
    );
    if (validateSDTChecksum(&rsdt->h))
    debug_msg("ACPI: RSDT checksum OK");
else
    debug_msg("ACPI: RSDT checksum FAILED");
serial_puts("ACPI: RSDT phys=");
debug_hex(rsdp_descriptor->descriptor10.rsdt_address);
serial_puts("\n");

serial_puts("ACPI: RSDT virt=");
debug_hex((uint64_t)(uintptr_t)rsdt);
serial_puts("\n");

serial_puts("ACPI: RSDT length=");
debug_hex(rsdt->h.length);
serial_puts("\n");
    debug_msg("ACPI: RSDT initialized");

    /*
     * Инициализируем FADT.
     *
     * initFADT() использует findHeader("FACP"),
     * который теперь сможет работать, потому что
     * rsdt уже установлен.
     */
    initFADT();

    if (!isFADTAvailable()) {
        debug_msg("ACPI: FADT not found");
        return 0;
    }

    debug_msg("ACPI: FADT initialized");
 enableACPI();

    return 1;
}
