#ifndef IX_DISK_FAT_H
#define IX_DISK_FAT_H

#include "../fs.h"

/* Монтирование первого FAT-раздела физического диска.
 * Возвращает 1 при успехе, 0 если диска/раздела нет. */
int        diskfat_init(void);

/* Корень смонтированного раздела (или 0). */
fs_node_t *diskfat_get_root(void);

/* Поиск файла в корне раздела (0 если нет). */
fs_node_t *diskfat_find(const char *name);

/* Создание пустого файла в корне раздела. */
fs_node_t *diskfat_create(const char *name);

/* Обрезать файл до нуля байт. */
int        diskfat_truncate(fs_node_t *node);

#endif