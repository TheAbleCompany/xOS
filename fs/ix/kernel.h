// kernel.h -- минимальный контракт, который ix_fs_*.c ожидают от ядра.
//
// Восстановлено под ArtyomX64 (см. примечание в ixstd_fs.h). Из всего
// "kernel.h" исходного проекта драйверам реально нужен только доступ
// к секторам диска.

#pragma once

#include <stdint.h>

// Мульти-дисковый слой: ixk_storage_read/write работают с "текущим"
// диском, который выбирается ixk_storage_set_disk(). Так драйверы ФС
// остаются однодисковыми, а automount перебирает все накопители.

// Число накопителей, найденных blkdev_init() (ATA legacy + AHCI).
int ixk_storage_disk_count(void);

// Выбрать "текущий" диск для последующих read/write (индекс blkdev).
void ixk_storage_set_disk(int idx);

// Имя текущего диска ("ata0m", "sata0") или "".
const char *ixk_storage_device_name(void);

// Родной логический сектор текущего диска (512/1024/2048/4096).  Нужен
// только разбору таблиц разделов (GPT хранит LBA в секторах диска);
// ixk_storage_read/write всегда адресуются 512-байтными секторами.
uint32_t ixk_storage_native_sector(void);

// Ёмкость текущего диска в 512-байтных секторах (0 -- неизвестна).
uint64_t ixk_storage_capacity(void);

// Модель текущего диска (может быть пустой строкой).
const char *ixk_storage_device_model(void);

// Сбросить кэш записи текущего диска на носитель (FLUSH CACHE / NVMe
// Flush).  Вызывается один раз после операции записи ФС-слоя.
int ixk_storage_sync(void);

// Читает `count` секторов по 512 байт начиная с абсолютного LBA `lba`
// (считая от начала физического диска, НЕ от начала раздела) в `buf`.
// Возвращает ненулевое значение при успехе, 0 при ошибке чтения.
// Реализация: fs/ix/ixk_storage.c (поверх drivers/blkdev.c).
int ixk_storage_read(uint64_t lba, uint32_t count, void *buf);

// Записывает `count` секторов по 512 байт начиная с абсолютного LBA `lba`.
// Возвращает ненулевое значение при успехе, 0 при ошибке записи.
// Реализация: fs/ix/ixk_storage.c (поверх drivers/blkdev.c).
int ixk_storage_write(uint64_t lba, uint32_t count, const void *buf);
