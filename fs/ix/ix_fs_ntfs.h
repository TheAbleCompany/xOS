#pragma once

#include <stdint.h>
#include "ixstd_fs.h"

#define NTFS_MAX_HANDLES     16
#define NTFS_SECTOR_SIZE     512
#define NTFS_NAME_MAX        256
#define NTFS_MFT_BUF_SIZE   4096
#define NTFS_RUNLIST_MAX     128

#pragma pack(push, 1)
typedef struct {
  uint8_t  jmp[3];
  uint8_t  oem[8];
  uint16_t bytes_per_sector;
  uint8_t  sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t  unused1[3];
  uint16_t unused2;
  uint8_t  media;
  uint16_t unused3;
  uint16_t sectors_per_track;
  uint16_t num_heads;
  uint32_t hidden_sectors;
  uint32_t unused4;
  uint32_t unused5;
  uint64_t total_sectors;
  uint64_t mft_lcn;
  uint64_t mft_mirror_lcn;
  int8_t   clusters_per_mft_record;
  uint8_t  unused6[3];
  int8_t   clusters_per_index_record;
  uint8_t  unused7[3];
  uint64_t volume_serial;
  uint32_t checksum;
} ntfs_boot_sector_t;

typedef struct {
  uint32_t magic;
  uint16_t update_seq_offset;
  uint16_t update_seq_count;
  uint64_t log_seq_number;
  uint16_t sequence_number;
  uint16_t hard_link_count;
  uint16_t first_attr_offset;
  uint16_t flags;
  uint32_t used_size;
  uint32_t allocated_size;
  uint64_t base_record;
  uint16_t next_attr_id;
  uint16_t padding;
  uint32_t mft_record_number;
} ntfs_mft_entry_t;

typedef struct {
  uint32_t type;
  uint32_t length;
  uint8_t  non_resident;
  uint8_t  name_length;
  uint16_t name_offset;
  uint16_t flags;
  uint16_t instance;
  union {
    struct {
      uint32_t value_length;
      uint16_t value_offset;
      uint8_t  flags2;
      uint8_t  padding;
    } resident;
    struct {
      uint64_t lowest_vcn;
      uint64_t highest_vcn;
      uint16_t run_offset;
      uint16_t compression_unit;
      uint32_t padding;
      uint64_t allocated_size;
      uint64_t real_size;
      uint64_t initialized_size;
    } non_resident;
  } data;
} ntfs_attr_header_t;

typedef struct {
  uint64_t parent_ref;
  uint64_t creation_time;
  uint64_t modification_time;
  uint64_t mft_modification_time;
  uint64_t access_time;
  uint64_t allocated_size;
  uint64_t real_size;
  uint32_t flags;
  uint32_t reparse;
  uint8_t  name_length;
  uint8_t  name_type;
  uint16_t name[1];
} ntfs_file_name_attr_t;

typedef struct {
  uint32_t type;
  uint32_t collation_rule;
  uint32_t index_alloc_size;
  uint8_t  clusters_per_index;
  uint8_t  padding[3];
  uint32_t offset;
  uint32_t total_size;
  uint32_t allocated_size;
  uint8_t  flags;
  uint8_t  padding2[3];
} ntfs_index_root_t;

typedef struct {
  uint64_t mft_reference;
  uint16_t length;
  uint16_t content_length;
  uint32_t flags;
} ntfs_index_entry_t;
#pragma pack(pop)

#define NTFS_MFT_MAGIC       0x454C4946u
#define NTFS_ATTR_STD_INFO   0x10u
#define NTFS_ATTR_FILE_NAME  0x30u
#define NTFS_ATTR_DATA       0x80u
#define NTFS_ATTR_INDEX_ROOT 0x90u
#define NTFS_ATTR_INDEX_ALLOC 0xA0u
#define NTFS_ATTR_BITMAP     0xB0u
#define NTFS_ATTR_END        0xFFFFFFFFu

#define NTFS_FILE_MFT        0u
#define NTFS_FILE_ROOT       5u

#define NTFS_INDEX_ENTRY_NODE      0x01u
#define NTFS_INDEX_ENTRY_LAST      0x02u

#define NTFS_MFT_IN_USE      0x0001u
#define NTFS_MFT_DIRECTORY   0x0002u

typedef struct {
  int64_t  offset;
  uint64_t length;
} ntfs_run_t;

typedef struct {
  int      valid;
  uint64_t part_lba;
  uint64_t part_sectors;
  uint16_t bytes_per_sector;
  uint8_t  sectors_per_cluster;
  uint32_t cluster_size;
  uint32_t mft_record_size;
  uint64_t mft_lcn;
  uint64_t mft_lba;
} ntfs_context_t;

int  ntfs_mount(ntfs_context_t* ctx, uint64_t part_lba, uint64_t part_sectors);
int  ntfs_open(ntfs_context_t* ctx, const char* path, uint32_t flags);
int  ntfs_read_at(ntfs_context_t* ctx, const char* path, uint32_t offset, void* buffer, uint32_t size);
int  ntfs_read(ntfs_context_t* ctx, int handle, void* buffer, uint32_t size);
int  ntfs_stat(ntfs_context_t* ctx, const char* path, ix_file_info_t* out);
int  ntfs_close(ntfs_context_t* ctx, int handle);
int  ntfs_opendir(ntfs_context_t* ctx, const char* path);
int  ntfs_readdir(ntfs_context_t* ctx, int dir_handle, ix_dirent_t* out);
int  ntfs_closedir(ntfs_context_t* ctx, int dir_handle);
