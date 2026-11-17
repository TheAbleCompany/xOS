// ixstd_fs.h -- общие типы файловой прослойки "ix" (FAT/NTFS/EXT драйверы).
//
// Восстановлено под ArtyomX64: оригинальных заголовков проекта-источника
// не было в присланном архиве, только сами ix_fs_fat.c/ix_fs_ntfs.c/ix_fs_ext.c.
// Состав структур/констант выведен из фактического использования полей
// в этих .c файлах (out->size, out->attrs, out->modified_ns, out->created_ns,
// out->name, out->is_dir, out->info.*).

#pragma once

#include <stdint.h>

// Атрибуты файла (битовая маска ix_file_info_t.attrs).
#define IX_FS_ATTR_DIR      0x01u
#define IX_FS_ATTR_HIDDEN   0x02u
#define IX_FS_ATTR_SYSTEM   0x04u
#define IX_FS_ATTR_READONLY 0x08u

typedef struct {
    uint64_t size;
    uint32_t attrs;
    uint64_t modified_ns;
    uint64_t accessed_ns;   /* использует ntfs_stat()/ext_stat() */
    uint64_t created_ns;
} ix_file_info_t;

typedef struct {
    char           name[256];
    int            is_dir;
    uint32_t       size;
    ix_file_info_t info;
} ix_dirent_t;
