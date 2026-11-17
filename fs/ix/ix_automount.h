// ix_automount.h -- автоматическое обнаружение и монтирование разделов
//                   физического диска в VFS ArtyomX64.
//
// Заменяет одиночный FAT-маунтер ix_disk_fat.c:
//
//   * таблицы разделов: MBR (включая EBR/логические диски) и GPT;
//   * superfloppy -- ФС прямо с LBA 0 (флешки без разметки);
//   * ФС: FAT12/16/32, ext2/3/4, NTFS (read-only);
//   * точки монтирования: disk0, disk1, ... diskN в корне initrd;
//   * подкаталоги, чтение, запись (FAT/ext), создание (O_CREAT),
//     усечение (O_TRUNC).

#ifndef IX_AUTOMOUNT_H
#define IX_AUTOMOUNT_H

#include "../fs.h"

/* Максимум одновременно смонтированных разделов (disk0..disk15). */
#define AUTOMOUNT_MAX_DISKS 16

/*
 * Пробная таблицу разделов, смонтировать все поддерживаемые разделы.
 * Возвращает число смонтированных дисков (0 = ничего не найдено).
 * Безопасно вызывать при отсутствии диска.
 */
int automount_init(void);

/* Число смонтированных дисков. */
int automount_count(void);

/* Корневой узел смонтированного диска (0 вне диапазона). */
fs_node_t *automount_get_root(int idx);

/* Имя точки монтирования ("disk0"). */
const char *automount_disk_name(int idx);

/* Короткое имя ФС для UI: "FAT32", "ext4", "NTFS"... */
const char *automount_fs_label(int idx);

/* Тип ФС (ix_fs_type_t) смонтированного диска. */
int automount_fs_type(int idx);

/* Поддерживает ли диск запись (NTFS и рискованные ext-тома -- нет). */
int automount_writeable(int idx);

/* 1, если том смонтирован только на чтение (NTFS, ext с recovery/quota...). */
int automount_read_only(int idx);

/*
 * Создать файл на диске idx по относительному пути (для O_CREAT).
 * Возвращает узел или 0.
 */
fs_node_t *automount_create_at(int idx, const char *relpath);

/* Обнулить файл (O_TRUNC). 0 = успех. */
int automount_truncate_at(int idx, fs_node_t *node);

/* Удалить файл по относительному пути тома idx. 0 = успех. */
int automount_unlink_at(int idx, const char *relpath);

/* Переименовать файл в томе idx (пути относительные). 0 = успех. */
int automount_rename_at(int idx, const char *from, const char *to);

/*
 * Создать подкаталог на диске idx по относительному пути.
 * Возвращает узел каталога или 0 (нет носителя, том read-only,
 * ФС без поддержки записи или родитель отсутствует).
 */
fs_node_t *automount_mkdir_at(int idx, const char *relpath);

#endif
