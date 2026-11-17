// ix_part.h -- определение типа ФС и сканер таблиц разделов.
//
// r13-storage: раньше ix_disk_fat.c искал ТОЛЬКО FAT по байту типа в
// MBR у primary-разделов master-диска.  Реальное железо разнообразнее:
// GPT, расширенные разделы, секторы без таблицы, NTFS, EXT2/3/4.
//
// Всё работает на "активном устройстве" слоя ixk_storage (см.
// ixk_storage.h), т.е. перед вызовом ix_part_scan() надо сделать
// ixk_storage_set_dev(d).

#ifndef IX_PART_H
#define IX_PART_H

#include <stdint.h>
#include "ix_fs_detect.h"

typedef struct {
    uint64_t     lba;       /* абсолютный LBA начала раздела           */
    uint64_t     sectors;   /* длина раздела в секторах (0 -- весь диск)*/
    ix_fs_type_t type;      /* предположение по байту типа (может UNKNOWN) */
    uint8_t      bootable;  /* MBR-флаг 0x80 (прилетает из primary)     */
} ix_part_t;

/* Максимум выводимых записей при сканировании. */
#define IX_PART_MAX 32

/*
 * Классифицировать файловую систему, начинающуюся с сектора lba.
 * FAT12/16/32 -- по валидному BPB; NTFS -- по OEM "NTFS    " + MFT-полям;
 * EXT2/3/4 -- по magic 0xEF53 в суперблоке (lba + 2).
 */
ix_fs_type_t ix_probe_fs(uint64_t lba);

/*
 * Отсканировать таблицу разделов активного устройства.
 * Выводит до max записей в out, возвращает их число.
 *
 * Понимает:
 *   - GPT (заголовок "EFI PART" на LBA1, до 128 записей);
 *   - MBR: все 4 primary + цепочка расширенных (types 05/0F/85 -> EBR);
 *   - суперфлоппи: сектор с ФС, но без таблицы разделов; добавляется
 *     одной записью {lba 0, sectors = размер устройства}.
 * Защитные записи MBR (type 0xEE) пропускаются -- их содержимое
 * уже отсканировал GPT-путь.
 */
int ix_part_scan(ix_part_t *out, int max);

#endif
