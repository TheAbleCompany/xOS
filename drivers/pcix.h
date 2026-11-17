/*
 * ============================================================
 * drivers/pcix.h -- общий слой для драйверов накопителей:
 *   - полный обход PCI (все шины за мостами, multifunction),
 *   - разбор BAR (32/64-bit, MMIO/IO),
 *   - отображение MMIO (UC) в окно ядра,
 *   - DMA-память (физически непрерывная, опц. ниже 4 ГиБ),
 *   - микросекундные задержки.
 * ============================================================
 *
 * Зачем: прежние драйверы сканировали только шины 0..3 и функции
 * 0..1.  На реальном железе SATA-контроллер Intel живёт в
 * 00:1f.2 (функция 2), NVMe -- за PCIe root port на произвольной
 * шине, AMD -- на шинах 0x20+.  Всё это раньше просто не находилось.
 */

#ifndef PCIX_H
#define PCIX_H

#include "../lib/common.h"

typedef struct {
    u8int  bus, dev, fn;
    u16int vendor, device;
    u8int  base_class, sub_class, prog_if, rev;
    u8int  hdr_type;            /* без бита multifunction */
} pcix_dev_t;

/* Конфигурационное пространство (механизм #1, offset < 256). */
u32int pcix_read32(const pcix_dev_t *d, u32int off);
u16int pcix_read16(const pcix_dev_t *d, u32int off);
void   pcix_write32(const pcix_dev_t *d, u32int off, u32int v);
void   pcix_write16(const pcix_dev_t *d, u32int off, u16int v);

/* Колбэк обхода: вернуть != 0, чтобы прекратить обход. */
typedef int (*pcix_visit_fn)(const pcix_dev_t *d, void *ctx);

/*
 * Обойти ВСЕ функции всех достижимых шин: корневая шина 0, затем
 * рекурсивно secondary-шины PCI-PCI мостов (класс 0x0604/0x0609).
 * Учитывается бит multifunction (функции 0..7).
 * Возвращает число посещённых функций.
 */
int pcix_enum(pcix_visit_fn fn, void *ctx);

/* Описание BAR n (0..5).  Возвращает 1, если BAR задан (не 0). */
int pcix_bar(const pcix_dev_t *d, int n, u64int *base, int *is_io, int *is64);

/* Включить Memory Space / I/O Space / Bus Master; DIS_INTX -- запретить
 * INTx (драйверы, работающие опросом, не должны получать шторм IRQ). */
#define PCIX_EN_MEM   0x2u
#define PCIX_EN_IO    0x1u
#define PCIX_EN_BM    0x4u
#define PCIX_DIS_INTX 0x400u
void pcix_enable(const pcix_dev_t *d, u16int flags);

/* Отобразить MMIO [phys, phys+size) как uncached.  0 при ошибке. */
void *pcix_map_mmio(u64int phys, u64int size);

/*
 * Выделить `pages` физически непрерывных страниц, обнулённых.
 * below4g != 0 -- гарантировать адрес + размер <= 4 ГиБ.
 * Возвращает виртуальный адрес (или 0), физический -- в *phys.
 */
void *pcix_dma_alloc(u64int pages, int below4g, u64int *phys);
void  pcix_dma_free(void *virt, u64int phys, u64int pages);

/* Печать hex-числа в debug-порт (debug_putnum печатает десятичное). */
void pcix_puthex(u64int v);
/* hex без префикса, дополненный нулями до width цифр. */
void pcix_puthexw(u64int v, int width);

/* Задержки (калиброваны по обращению к порту 0x80, ~1 мкс). */
void pcix_udelay(u32int us);
void pcix_mdelay(u32int ms);

#endif /* PCIX_H */
