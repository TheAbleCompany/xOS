#pragma once

#include <stdint.h>
#include "ixstd_fs.h"

#define EXT_MAX_HANDLES      16
#define EXT_NAME_MAX         256
#define EXT_DIRECT_BLOCKS    12
#define EXT_SECTOR_SIZE      512

#pragma pack(push, 1)
typedef struct {
  uint32_t s_inodes_count;
  uint32_t s_blocks_count;
  uint32_t s_r_blocks_count;
  uint32_t s_free_blocks_count;
  uint32_t s_free_inodes_count;
  uint32_t s_first_data_block;
  uint32_t s_log_block_size;
  uint32_t s_log_frag_size;
  uint32_t s_blocks_per_group;
  uint32_t s_frags_per_group;
  uint32_t s_inodes_per_group;
  uint32_t s_mtime;
  uint32_t s_wtime;
  uint16_t s_mnt_count;
  uint16_t s_max_mnt_count;
  uint16_t s_magic;
  uint16_t s_state;
  uint16_t s_errors;
  uint16_t s_minor_rev_level;
  uint32_t s_lastcheck;
  uint32_t s_checkinterval;
  uint32_t s_creator_os;
  uint32_t s_rev_level;
  uint16_t s_def_resuid;
  uint16_t s_def_resgid;
  uint32_t s_first_ino;
  uint16_t s_inode_size;
  uint16_t s_block_group_nr;
  uint32_t s_feature_compat;
  uint32_t s_feature_incompat;
  uint32_t s_feature_ro_compat;
  uint8_t  s_uuid[16];
  uint8_t  s_volume_name[16];
  uint8_t  s_last_mounted[64];
  uint32_t s_algo_bitmap;
  uint8_t  s_prealloc_blocks;
  uint8_t  s_prealloc_dir_blocks;
  uint16_t s_reserved_gdt_blocks;
  uint8_t  s_journal_uuid[16];
  uint32_t s_journal_inum;
  uint32_t s_journal_dev;
  uint32_t s_last_orphan;
  uint32_t s_hash_seed[4];
  uint8_t  s_def_hash_version;
  uint8_t  s_jnl_backup_type;
  uint16_t s_desc_size;
  uint32_t s_default_mount_opts;
  uint32_t s_first_meta_bg;
  uint32_t s_mkfs_time;
  uint32_t s_jnl_blocks[17];
  uint32_t s_blocks_count_hi;
  uint32_t s_r_blocks_count_hi;
  uint32_t s_free_blocks_count_hi;
  uint16_t s_min_extra_isize;
  uint16_t s_want_extra_isize;
  uint32_t s_flags;
  uint16_t s_raid_stride;
  uint16_t s_mmp_interval;
  uint64_t s_mmp_block;
  uint32_t s_raid_stripe_width;
  uint8_t  s_log_groups_per_flex;
  uint8_t  s_checksum_type;
  uint16_t s_reserved_pad;
} ext_superblock_t;

typedef struct {
  uint32_t bg_block_bitmap;
  uint32_t bg_inode_bitmap;
  uint32_t bg_inode_table;
  uint16_t bg_free_blocks_count;
  uint16_t bg_free_inodes_count;
  uint16_t bg_used_dirs_count;
  uint16_t bg_pad;
  uint8_t  bg_reserved[12];
} ext_group_desc_t;

typedef struct {
  uint32_t bg_block_bitmap;
  uint32_t bg_inode_bitmap;
  uint32_t bg_inode_table;
  uint16_t bg_free_blocks_count;
  uint16_t bg_free_inodes_count;
  uint16_t bg_used_dirs_count;
  uint16_t bg_flags;
  uint32_t bg_exclude_bitmap_lo;
  uint16_t bg_block_bitmap_csum;
  uint16_t bg_inode_bitmap_csum;
  uint16_t bg_itable_unused;
  uint16_t bg_checksum;
  uint32_t bg_block_bitmap_hi;
  uint32_t bg_inode_bitmap_hi;
  uint32_t bg_inode_table_hi;
  uint16_t bg_free_blocks_count_hi;
  uint16_t bg_free_inodes_count_hi;
  uint16_t bg_used_dirs_count_hi;
  uint16_t bg_itable_unused_hi;
  uint32_t bg_exclude_bitmap_hi;
  uint16_t bg_block_bitmap_csum_hi;
  uint16_t bg_inode_bitmap_csum_hi;
  uint32_t bg_reserved;
} ext4_group_desc_t;

typedef struct {
  uint16_t i_mode;
  uint16_t i_uid;
  uint32_t i_size;
  uint32_t i_atime;
  uint32_t i_ctime;
  uint32_t i_mtime;
  uint32_t i_dtime;
  uint16_t i_gid;
  uint16_t i_links_count;
  uint32_t i_blocks;
  uint32_t i_flags;
  uint32_t i_osd1;
  uint32_t i_block[15];
  uint32_t i_generation;
  uint32_t i_file_acl;
  uint32_t i_size_high;
  uint32_t i_obso_faddr;
  uint8_t  i_osd2[12];
} ext_inode_t;

typedef struct {
  uint32_t inode;
  uint16_t rec_len;
  uint8_t  name_len;
  uint8_t  file_type;
  char     name[256];
} ext_dir_entry_t;

typedef struct {
  uint16_t eh_magic;
  uint16_t eh_entries;
  uint16_t eh_max;
  uint16_t eh_depth;
  uint32_t eh_generation;
} ext4_extent_header_t;

typedef struct {
  uint32_t ei_block;
  uint32_t ei_leaf_lo;
  uint16_t ei_leaf_hi;
  uint16_t ei_unused;
} ext4_extent_idx_t;

typedef struct {
  uint32_t ee_block;
  uint16_t ee_len;
  uint16_t ee_start_hi;
  uint32_t ee_start_lo;
} ext4_extent_t;
#pragma pack(pop)

#define EXT_MAGIC            0xEF53u
#define EXT_ROOT_INO         2u
#define EXT_GOOD_OLD_INODE_SIZE 128u

#define EXT_FT_UNKNOWN       0
#define EXT_FT_REG_FILE      1
#define EXT_FT_DIR           2
#define EXT_FT_CHRDEV        3
#define EXT_FT_BLKDEV        4
#define EXT_FT_FIFO          5
#define EXT_FT_SOCK          6
#define EXT_FT_SYMLINK       7

#define EXT4_EXTENTS_FL      0x00080000u
#define EXT4_EXTENT_MAGIC    0xF30Au

#define EXT_INCOMPAT_EXTENTS 0x0040u
#define EXT_INCOMPAT_64BIT   0x0080u

typedef struct {
  int      valid;
  uint64_t part_lba;
  uint64_t part_sectors;
  uint32_t block_size;
  uint32_t block_sectors;
  uint16_t inode_size;
  uint32_t inodes_per_group;
  uint32_t blocks_per_group;
  uint32_t num_groups;
  uint32_t desc_size;
  int      has_extents;
  int      has_64bit;
  uint32_t first_data_block;
} ext_context_t;

int  ext_mount(ext_context_t* ctx, uint64_t part_lba, uint64_t part_sectors);
int  ext_open(ext_context_t* ctx, const char* path, uint32_t flags);
int  ext_read(ext_context_t* ctx, int handle, void* buffer, uint32_t size);
int  ext_stat(ext_context_t* ctx, const char* path, ix_file_info_t* out);
int  ext_close(ext_context_t* ctx, int handle);
int  ext_opendir(ext_context_t* ctx, const char* path);
int  ext_readdir(ext_context_t* ctx, int dir_handle, ix_dirent_t* out);
int  ext_closedir(ext_context_t* ctx, int dir_handle);

/* --- WRITE-слой (stateless, поверх пути) -------------------------- */
/* Драйвер EXT -- только чтение; write/create/truncate возвращают -1. */

/* Чтение size байт по абсолютному смещению. Возвращает байт или -1. */
int  ext_read_at(ext_context_t* ctx, const char* path,
                 uint32_t offset, void* buffer, uint32_t size);

int  ext_write_at(ext_context_t* ctx, const char* path,
                  uint32_t offset, const void* buffer, uint32_t size);

int  ext_create(ext_context_t* ctx, const char* path);
int  ext_truncate_path(ext_context_t* ctx, const char* path);

/* Удалить файл: блоки -> запись каталога -> inode. 0 = успех. */
int  ext_unlink(ext_context_t* ctx, const char* path);

/* Переименовать файл в пределах тома. 0 = успех. */
int  ext_rename(ext_context_t* ctx, const char* from, const char* to);
