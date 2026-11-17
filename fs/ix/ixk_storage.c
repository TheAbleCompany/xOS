// ixk_storage.c -- доступ к физическим дискам для ix-драйверов ФС.
//
// Реализует контракт kernel.h поверх drivers/blkdev.c -- унифицированного
// слоя блочных устройств:
//
//   - legacy ATA PIO (0x1F0/0x170, master+slave), LBA28 и LBA48;
//   - AHCI/SATA (PCI класс 0x0106) через drivers/ahci.c.
//
// Драйверы ФС (FAT/ext/NTFS) остаются однодисковыми: они читают
// "текущий" диск, выбранный ixk_storage_set_disk().  automount
// перебирает все диски, переключая текущий индекс.
//
// Фоллбэк: если blkdev_init() не нашёл ни одного диска, но старый
// драйвер drivers/disk.c видит primary master (ata_drive_present()),
// используем его -- так сохраняется поведение r12-bootfix на машинах,
// где старый путь уже проверен.

#include <stdint.h>

#include "../../lib/common.h"
#include "../../drivers/blkdev.h"
#include "../../drivers/disk.h"
#include "kernel.h"

/* Индекс "текущего" диска (какой диск видят ixk_storage_read/write). */
static int storage_current = 0;

int ixk_storage_disk_count(void)
{
    int n = blkdev_count();
    if (n > 0)
        return n;
    return ata_drive_present() ? 1 : 0;
}

void ixk_storage_set_disk(int idx)
{
    if (idx < 0)
        idx = 0;
    storage_current = idx;
}

const char *ixk_storage_device_name(void)
{
    const blkdev_t *d = blkdev_get(storage_current);
    return d ? d->name : "";
}

uint32_t ixk_storage_native_sector(void)
{
    return blkdev_lss(storage_current);
}

uint64_t ixk_storage_capacity(void)
{
    return blkdev_sectors(storage_current);
}

const char *ixk_storage_device_model(void)
{
    const blkdev_t *d = blkdev_get(storage_current);
    return d ? d->model : "";
}

int ixk_storage_sync(void)
{
    if (blkdev_count() > 0)
        return blkdev_flush(storage_current);
    return 1;       /* старый PIO-путь без кэша записи в нашей модели */
}

int ixk_storage_read(uint64_t lba, uint32_t count, void *buf)
{
    if (!buf || count == 0)
        return 0;

    /* Основной путь: слой blkdev (ATA LBA48 + AHCI). */
    if (blkdev_count() > 0)
        return blkdev_read(storage_current, lba, count, buf);

    /*
     * Фоллбэк: старый ATA PIO драйвер (primary master, LBA28).
     * Работает даже если blkdev по какой-то причине пуст.
     */
    if (!ata_drive_present())
        return 0;

    if (lba > 0x0FFFFFFFull || lba + count > 0x10000000ull)
        return 0;

    uint8_t *dst = (uint8_t *)buf;

    for (uint32_t i = 0; i < count; i++) {
        if (!hd_read_sector((u32int)(lba + i),
                            (u8int *)(dst + (uint64_t)i * 512u)))
            return 0;
    }

    return 1;
}

int ixk_storage_write(uint64_t lba, uint32_t count, const void *buf)
{
    if (!buf || count == 0)
        return 0;

    if (blkdev_count() > 0)
        return blkdev_write(storage_current, lba, count, buf);

    if (!ata_drive_present())
        return 0;

    if (lba > 0x0FFFFFFFull || lba + count > 0x10000000ull)
        return 0;

    const uint8_t *src = (const uint8_t *)buf;

    for (uint32_t i = 0; i < count; i++) {
        if (!hd_write_sector((u32int)(lba + i),
                             (u8int *)(src + (uint64_t)i * 512u)))
            return 0;
    }

    return 1;
}
