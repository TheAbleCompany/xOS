// ============================================
// 🔒 БЕЗОПАСНЫЙ madt.c - с защитой от падений
// ============================================

#include "madt.h"
#include "../acpi.h"  // Для validateSDTChecksum
#include "../../lib/common.h"

MADT* madt = NULL;

// ============================================
// 🛡️ Ограничения размеров (защита от overflow)
// ============================================
#define MAX_PROCESSOR_APICS   32
#define MAX_IOAPICS           32
#define MAX_SOURCE_OVERRIDES  32
#define MAX_NMI_SOURCES       32
#define MAX_NMI_INTERRUPTS    32
#define MAX_LAPIC_OVERRIDES   16
#define MAX_X2LAPICS          32

// Массивы для хранения записей (инициализированы NULL!)
ProcessorAPIC* processor_apics[MAX_PROCESSOR_APICS];
IOAPIC* ioapics[MAX_IOAPICS];
IOAPICSourceOverride* ioapic_source_overrides[MAX_SOURCE_OVERRIDES];
IOAPICNonMaskableInterruptSource* ioapic_interrupt_sources[MAX_NMI_SOURCES];
IOAPICNonMaskableInterrupt* ioapic_interrupts[MAX_NMI_INTERRUPTS];
LAPICAddressOverride* lapic_address_overrides[MAX_LAPIC_OVERRIDES];
x2LAPIC* x2_lapics[MAX_X2LAPICS];

// Счётчики реальных элементов
static uint8_t processor_apic_count = 0;
static uint8_t ioapic_count = 0;
static uint8_t source_override_count = 0;
static uint8_t nmi_source_count = 0;
static uint8_t nmi_interrupt_count = 0;
static uint8_t lapic_override_count = 0;
static uint8_t x2lapic_count = 0;

// ============================================
// Инициализация MADT
// ============================================
void initMADT() 
{
    // Сбрасываем состояние
    madt = NULL;
    processor_apic_count = 0;
    ioapic_count = 0;
    source_override_count = 0;
    nmi_source_count = 0;
    nmi_interrupt_count = 0;
    lapic_override_count = 0;
    x2lapic_count = 0;
    
    // Очищаем массивы
    for (int i = 0; i < MAX_PROCESSOR_APICS; i++) processor_apics[i] = NULL;
    for (int i = 0; i < MAX_IOAPICS; i++) ioapics[i] = NULL;
    for (int i = 0; i < MAX_SOURCE_OVERRIDES; i++) ioapic_source_overrides[i] = NULL;
    for (int i = 0; i < MAX_NMI_SOURCES; i++) ioapic_interrupt_sources[i] = NULL;
    for (int i = 0; i < MAX_NMI_INTERRUPTS; i++) ioapic_interrupts[i] = NULL;
    for (int i = 0; i < MAX_LAPIC_OVERRIDES; i++) lapic_address_overrides[i] = NULL;
    for (int i = 0; i < MAX_X2LAPICS; i++) x2_lapics[i] = NULL;
    
    // Ищем таблицу MADT ("APIC")
    ACPISDTHeader* header = findHeader("APIC");
    
    if (header == NULL) {
        return; // MADT не найден - это нормально для UP систем
    }
    
    // Проверка размера
    if (header->length < sizeof(MADT)) {
        return; // Таблица повреждена
    }
    
    // Дополнительно проверяем чексумм
    if (!validateSDTChecksum(header)) {
        return; // Некорректная таблица
    }
    
    madt = (MADT*)header;
}

// ============================================
// 🛡️ БЕЗОПАСНЫЙ парсинг MADT
// ============================================
void parseMADT() 
{
    // Защита от вызова без инициализации
    if (madt == NULL) {
        return;
    }
    
    APICRecordHeader* head;
    uint32_t length = sizeof(MADT);
    uintptr_t madt_address = (uintptr_t) madt;
    
    // Получаем общую длину таблицы
    uint32_t total_length = madt->header.length;
    
    // Защита от некорректной длины
    if (total_length <= length || total_length > 65536) {
        return; // Некорректная длина
    }

    while (length < total_length) 
    {
        // Проверка границ чтения
        if (length + sizeof(APICRecordHeader) > total_length) {
            break; // Выход за границы
        }
        
        head = (APICRecordHeader*)(madt_address + length);
        
        // 🛡️ КРИТИЧЕСКАЯ ЗАЩИТА: проверяем record_length!
        if (head->record_length == 0) {
            break; // Избегаем бесконечного цикла!
        }
        
        // Проверка что запись не выходит за границы
        if (length + head->record_length > total_length) {
            break; // Повреждённая запись
        }
        
        switch (head->entry_type) 
        {
            case 0:  // Processor Local APIC
                if (processor_apic_count < MAX_PROCESSOR_APICS) {
                    processor_apics[processor_apic_count++] = (ProcessorAPIC*) head;
                }
                length += head->record_length;
                break;
                
            case 1:  // I/O APIC
                if (ioapic_count < MAX_IOAPICS) {
                    ioapics[ioapic_count++] = (IOAPIC*) head;
                }
                length += head->record_length;
                break;
                
            case 2:  // Interrupt Source Override
                if (source_override_count < MAX_SOURCE_OVERRIDES) {
                    ioapic_source_overrides[source_override_count++] = (IOAPICSourceOverride*) head;
                }
                length += head->record_length;
                break;
                
            case 3:  // NMI Source
                if (nmi_source_count < MAX_NMI_SOURCES) {
                    ioapic_interrupt_sources[nmi_source_count++] = (IOAPICNonMaskableInterruptSource*) head;
                }
                length += head->record_length;
                break;
                
            case 4:  // Local APIC NMI
                if (nmi_interrupt_count < MAX_NMI_INTERRUPTS) {
                    ioapic_interrupts[nmi_interrupt_count++] = (IOAPICNonMaskableInterrupt*) head;
                }
                length += head->record_length;
                break;
                
            case 5:  // LAPIC Address Override
                if (lapic_override_count < MAX_LAPIC_OVERRIDES) {
                    lapic_address_overrides[lapic_override_count++] = (LAPICAddressOverride*) head;
                }
                length += head->record_length;
                break;
                
            case 9:  // x2APIC
                if (x2lapic_count < MAX_X2LAPICS) {
                    x2_lapics[x2lapic_count++] = (x2LAPIC*) head;
                }
                length += head->record_length;
                break;
                
            default:  // 🛡️ Неизвестный тип - ПРОПУСКАЕМ с инкрементом!
                // Это критично для избежания бесконечного цикла
                length += head->record_length;
                break;
        }
    }
}

// ============================================
// 📊 Геттеры для безопасного доступа к данным
// ============================================

uint8_t getProcessorAPICCount() { return processor_apic_count; }
uint8_t getIOAPICCount() { return ioapic_count; }

ProcessorAPIC* getProcessorAPIC(uint8_t index) 
{
    if (index >= processor_apic_count) return NULL;
    return processor_apics[index];
}

IOAPIC* getIOAPIC(uint8_t index) 
{
    if (index >= ioapic_count) return NULL;
    return ioapics[index];
}

// Получить адрес Local APIC (с учётом override если есть)
uint32_t getLocalAPICAddress()
{
    if (madt == NULL) return 0;
    
    // Проверяем LAPIC Address Override (тип 5)
    for (uint8_t i = 0; i < lapic_override_count; i++) {
        if (lapic_address_overrides[i] != NULL) {
            return (uint32_t)(lapic_address_overrides[i]->LAPIC_address & 0xFFFFFFFF);
        }
    }
    
    // Стандартный адрес из заголовка MADT
    return madt->APIC_address;
}
