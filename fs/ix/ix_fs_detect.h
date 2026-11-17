// ix_fs_detect.h -- определение файловой системы раздела по сигнатурам.
//
// Определяет не только то, что ядро умеет монтировать (FAT12/16/32,
// NTFS, ext2/3/4), но и "чужие" форматы, которые встречаются на реальных
// дисках (exFAT, BitLocker, LUKS, swap, XFS, Btrfs, LVM, ...): их надо
// ОПОЗНАТЬ, чтобы не принять за мусор и объяснить, почему раздел не
// смонтирован.  Все сигнатуры валидируются (не только magic), чтобы
// остаточные данные от старого форматирования не давали ложных срабатываний.

#pragma once

#include <stdint.h>

typedef enum {
    IX_FS_TYPE_UNKNOWN = 0,
    IX_FS_TYPE_FAT12,
    IX_FS_TYPE_FAT16,
    IX_FS_TYPE_FAT32,
    IX_FS_TYPE_NTFS,
    IX_FS_TYPE_EXT,        // ext2/ext3/ext4 (ext_ver в ix_fs_info_t)

    // Опознаются, но драйвера нет:
    IX_FS_TYPE_EXFAT,
    IX_FS_TYPE_BITLOCKER,
    IX_FS_TYPE_LUKS,
    IX_FS_TYPE_SWAP,
    IX_FS_TYPE_XFS,
    IX_FS_TYPE_BTRFS,
    IX_FS_TYPE_F2FS,
    IX_FS_TYPE_LVM,
    IX_FS_TYPE_MDRAID,
    IX_FS_TYPE_ISO9660,
    IX_FS_TYPE_HFSPLUS,
    IX_FS_TYPE_APFS,
    IX_FS_TYPE_REFS,
    IX_FS_TYPE_EMPTY,      // раздел заполнен нулями
} ix_fs_type_t;

typedef struct {
    ix_fs_type_t type;
    const char  *name;        // для UI/лога: "FAT32", "ext4", "BitLocker"...
    int          mountable;   // есть драйвер и том не противоречит ему
    int          read_only;   // монтировать только на чтение (рискованные фичи)
    int          ext_ver;     // 2/3/4 для ext, иначе 0
    char         label[20];   // метка тома, если удалось прочитать
    const char  *note;        // причина read_only/unmountable (или NULL)
} ix_fs_info_t;

// Полная проверка раздела.  part_lba -- начало раздела в 512-байтных
// секторах диска; part_sectors -- его размер (0 = неизвестен, границы не
// проверяются).  Возвращает 1, если сектор прочитан (даже при UNKNOWN).
int ix_fs_probe(uint64_t part_lba, uint64_t part_sectors, ix_fs_info_t *out);

// Совместимость: тип МОНТИРУЕМОЙ ФС либо IX_FS_TYPE_UNKNOWN.
ix_fs_type_t ix_fs_detect(uint64_t part_lba);

// 1, если для типа есть драйвер (FAT*/NTFS/ext).
int ix_fs_type_mountable(ix_fs_type_t t);

const char *ix_fs_type_name(ix_fs_type_t t);
