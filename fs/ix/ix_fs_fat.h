#pragma once

#include <stdint.h>
#include "ixstd_fs.h"
#include "ix_fs_detect.h"

#define FAT_MAX_HANDLES     16
/* Максимальный "сектор ФС" (BPB.bytes_per_sector): 4Kn-тома. */
#define FAT_SECTOR_SIZE     4096
#define FAT_NAME_MAX        256
#define FAT_CLUSTER_BUF     4096
#define FAT_DIR_BUF_SECTORS 32

#pragma pack(push, 1)
typedef struct {
  uint8_t  jmp[3];
  uint8_t  oem[8];
  uint16_t bytes_per_sector;
  uint8_t  sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t  num_fats;
  uint16_t root_entry_count;
  uint16_t total_sectors_16;
  uint8_t  media;
  uint16_t fat_size_16;
  uint16_t sectors_per_track;
  uint16_t num_heads;
  uint32_t hidden_sectors;
  uint32_t total_sectors_32;
  union {
    struct {
      uint8_t  drive_number;
      uint8_t  reserved1;
      uint8_t  boot_sig;
      uint32_t volume_serial;
      uint8_t  volume_label[11];
      uint8_t  fs_type[8];
    } fat16;
    struct {
      uint32_t fat_size_32;
      uint16_t ext_flags;
      uint16_t fs_version;
      uint32_t root_cluster;
      uint16_t fs_info;
      uint16_t backup_boot;
      uint8_t  reserved0[12];
      uint8_t  drive_number;
      uint8_t  reserved1;
      uint8_t  boot_sig;
      uint32_t volume_serial;
      uint8_t  volume_label[11];
      uint8_t  fs_type[8];
    } fat32;
  } ext;
} fat_bpb_t;

typedef struct {
  uint8_t  name[11];
  uint8_t  attr;
  uint8_t  nt_res;
  uint8_t  crt_time_tenth;
  uint16_t crt_time;
  uint16_t crt_date;
  uint16_t lst_acc_date;
  uint16_t fst_clus_hi;
  uint16_t wrt_time;
  uint16_t wrt_date;
  uint16_t fst_clus_lo;
  uint32_t file_size;
} fat_dir_entry_t;

typedef struct {
  uint8_t  order;
  uint16_t name1[5];
  uint8_t  attr;
  uint8_t  type;
  uint8_t  checksum;
  uint16_t name2[6];
  uint16_t fst_clus_lo;
  uint16_t name3[2];
} fat_lfn_entry_t;
#pragma pack(pop)

#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LONG_NAME  0x0F

typedef struct {
  int          valid;
  ix_fs_type_t fat_type;
  uint64_t     part_lba;
  uint64_t     part_sectors;
  uint16_t     bytes_per_sector;
  uint8_t      sectors_per_cluster;
  uint32_t     cluster_size;
  uint16_t     reserved_sectors;
  uint8_t      num_fats;
  uint32_t     fat_size;
  uint32_t     root_entry_count;
  uint32_t     root_dir_sectors;
  uint32_t     first_data_sector;
  uint32_t     total_data_clusters;
  uint32_t     root_cluster;
  uint64_t     fat_start_lba;
  uint64_t     root_dir_lba;
  uint64_t     data_start_lba;
} fat_context_t;

int  fat_mount(fat_context_t* ctx, uint64_t part_lba, uint64_t part_sectors, ix_fs_type_t type);
int  fat_open(fat_context_t* ctx, const char* path, uint32_t flags);
int  fat_read(fat_context_t* ctx, int handle, void* buffer, uint32_t size);
int  fat_stat(fat_context_t* ctx, const char* path, ix_file_info_t* out);
int  fat_close(fat_context_t* ctx, int handle);
int  fat_opendir(fat_context_t* ctx, const char* path);
int  fat_readdir(fat_context_t* ctx, int dir_handle, ix_dirent_t* out);
int  fat_closedir(fat_context_t* ctx, int dir_handle);

/* --- WRITE-слой (stateless, поверх пути) -------------------------- */

/* Чтение size байт по абсолютному смещению. Возвращает байт или -1. */
int  fat_read_at(fat_context_t* ctx, const char* path,
                 uint32_t offset, void* buffer, uint32_t size);

/* Запись с расширением цепочки кластеров. Возвращает байт или -1. */
int  fat_write_at(fat_context_t* ctx, const char* path,
                  uint32_t offset, const void* buffer, uint32_t size);

/* Создание пустого файла (8.3 + LFN). 0 = успех. */
int  fat_create(fat_context_t* ctx, const char* path);

/* Создание подкаталога (кластер + "." и ".."). 0 = успех. */
int  fat_mkdir(fat_context_t* ctx, const char* path);

/* Обнулить файл, освободив кластеры. 0 = успех. */
int  fat_truncate_path(fat_context_t* ctx, const char* path);

/* Удалить файл (запись каталога + LFN-слоты + цепочка). 0 = успех. */
int  fat_unlink(fat_context_t* ctx, const char* path);

/* Переименовать в пределах одного каталога или с переносом. 0 = успех. */
int  fat_rename(fat_context_t* ctx, const char* from, const char* to);
