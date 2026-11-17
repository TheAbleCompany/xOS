#include "ix_fs_ext.h"
#include "kernel.h"
#include "ixk_string.h"
#include "ix_utf8.h"
#include "ixk_video.h"

#define EXT_SECBUF_SIZE 4096

typedef struct {
  int      active;
  int      is_dir;
  uint32_t inode_num;
  uint64_t file_size;
  uint64_t cur_offset;
  uint32_t i_block[15];
  uint32_t i_flags;
  uint16_t i_mode;
} ext_file_handle_t;

typedef struct {
  int      active;
  uint32_t inode_num;
  uint32_t i_block[15];
  uint32_t i_flags;
  uint64_t dir_size;
  uint64_t cur_offset;
} ext_dir_handle_t;

static ext_file_handle_t g_ext_files[EXT_MAX_HANDLES];
static ext_dir_handle_t  g_ext_dirs[EXT_MAX_HANDLES];
static uint8_t g_ext_secbuf[EXT_SECBUF_SIZE];
static uint8_t g_ext_blkbuf[EXT_SECBUF_SIZE];
static uint8_t g_ext_indbuf[EXT_SECBUF_SIZE];
static uint8_t g_ext_ind2buf[EXT_SECBUF_SIZE];
static uint8_t g_ext_sbbuf[2048];

static uint16_t erd16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t erd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t erd64(const uint8_t* p) {
  return (uint64_t)erd32(p) | ((uint64_t)erd32(p + 4) << 32);
}

static void ext_rc_invalidate(void);   /* кэш поиска пути для ext_read_at */

static int ext_read_sectors(ext_context_t* ctx, uint64_t sector_offset, uint32_t count, void* buf) {
  return ixk_storage_read(ctx->part_lba + sector_offset, count, buf);
}

static int ext_read_block_raw(ext_context_t* ctx, uint64_t block, void* buf) {
  if (block == 0) {
    ix_memset(buf, 0, ctx->block_size);
    return 1;
  }
  uint64_t lba = block * ctx->block_sectors;
  return ext_read_sectors(ctx, lba, ctx->block_sectors, buf);
}

/* Флаг "была ошибка чтения блока" -- отличает дыру в файле (нули) от
 * сбоя ввода-вывода при разрешении блоков (ext_resolve_* возвращают 0 в
 * обоих случаях). */
static int g_ext_io_err = 0;

static int ext_read_block(ext_context_t* ctx, uint64_t block, void* buf) {
  int ok = ext_read_block_raw(ctx, block, buf);
  if (!ok) g_ext_io_err = 1;
  return ok;
}

int ext_mount(ext_context_t* ctx, uint64_t part_lba, uint64_t part_sectors) {
  ix_memset(ctx, 0, sizeof(ext_context_t));
  ix_memset(g_ext_files, 0, sizeof(g_ext_files));
  ix_memset(g_ext_dirs, 0, sizeof(g_ext_dirs));

  if (!ixk_storage_read(part_lba + 2, 2, g_ext_sbbuf))
    return 0;

  const ext_superblock_t* sb = (const ext_superblock_t*)g_ext_sbbuf;

  if (erd16((const uint8_t*)&sb->s_magic) != EXT_MAGIC)
    return 0;

  uint32_t log_bs = erd32((const uint8_t*)&sb->s_log_block_size);
  if (log_bs > 2u) log_bs = 2u;
  ctx->block_size = 1024u << log_bs;
  ctx->block_sectors = ctx->block_size / EXT_SECTOR_SIZE;

  ctx->inode_size = erd16((const uint8_t*)&sb->s_inode_size);
  if (ctx->inode_size == 0) ctx->inode_size = EXT_GOOD_OLD_INODE_SIZE;

  ctx->inodes_per_group = erd32((const uint8_t*)&sb->s_inodes_per_group);
  ctx->blocks_per_group = erd32((const uint8_t*)&sb->s_blocks_per_group);
  ctx->first_data_block = erd32((const uint8_t*)&sb->s_first_data_block);

  uint32_t incompat = erd32((const uint8_t*)&sb->s_feature_incompat);
  ctx->has_extents = (incompat & EXT_INCOMPAT_EXTENTS) ? 1 : 0;
  ctx->has_64bit = (incompat & EXT_INCOMPAT_64BIT) ? 1 : 0;

  ctx->desc_size = 32;
  if (ctx->has_64bit) {
    uint16_t ds = erd16((const uint8_t*)&sb->s_desc_size);
    if (ds > 32) ctx->desc_size = ds;
  }

  uint32_t total_blocks = erd32((const uint8_t*)&sb->s_blocks_count);
  if (ctx->blocks_per_group > 0)
    ctx->num_groups = (total_blocks + ctx->blocks_per_group - 1) / ctx->blocks_per_group;
  else
    ctx->num_groups = 1;

  ctx->part_lba = part_lba;
  ctx->part_sectors = part_sectors;
  ctx->valid = 1;
  return 1;
}

static int ext_read_inode(ext_context_t* ctx, uint32_t inode_num, ext_inode_t* out) {
  if (inode_num == 0) return 0;
  uint32_t group = (inode_num - 1) / ctx->inodes_per_group;
  uint32_t index = (inode_num - 1) % ctx->inodes_per_group;

  uint64_t gd_block = (uint64_t)ctx->first_data_block + 1;
  uint64_t gd_byte = (uint64_t)group * (uint64_t)ctx->desc_size;
  uint64_t gd_block_off = gd_block + gd_byte / ctx->block_size;
  uint32_t gd_in_block = (uint32_t)(gd_byte % ctx->block_size);

  if (!ext_read_block(ctx, gd_block_off, g_ext_secbuf))
    return 0;

  uint64_t inode_table;
  if (ctx->has_64bit && ctx->desc_size >= 64) {
    const ext4_group_desc_t* gd = (const ext4_group_desc_t*)(g_ext_secbuf + gd_in_block);
    inode_table = (uint64_t)erd32((const uint8_t*)&gd->bg_inode_table) |
                  ((uint64_t)erd32((const uint8_t*)&gd->bg_inode_table_hi) << 32);
  } else {
    const ext_group_desc_t* gd = (const ext_group_desc_t*)(g_ext_secbuf + gd_in_block);
    inode_table = (uint64_t)erd32((const uint8_t*)&gd->bg_inode_table);
  }

  uint64_t inode_byte = (uint64_t)index * (uint64_t)ctx->inode_size;
  uint64_t inode_block = inode_table + inode_byte / ctx->block_size;
  uint32_t inode_off = (uint32_t)(inode_byte % ctx->block_size);

  if (!ext_read_block(ctx, inode_block, g_ext_secbuf))
    return 0;

  ix_memcpy(out, g_ext_secbuf + inode_off, sizeof(ext_inode_t));
  return 1;
}

static uint64_t ext_inode_size(const ext_inode_t* inode) {
  uint64_t size = (uint64_t)erd32((const uint8_t*)&inode->i_size);
  uint32_t hi = erd32((const uint8_t*)&inode->i_size_high);
  size |= ((uint64_t)hi << 32);
  return size;
}

static uint64_t ext_resolve_extent_block(ext_context_t* ctx, const uint8_t* extent_data,
                                         uint64_t logical_block) {
  const ext4_extent_header_t* hdr = (const ext4_extent_header_t*)extent_data;
  uint16_t magic = erd16((const uint8_t*)&hdr->eh_magic);
  if (magic != EXT4_EXTENT_MAGIC) return 0;

  uint16_t entries = erd16((const uint8_t*)&hdr->eh_entries);
  uint16_t depth = erd16((const uint8_t*)&hdr->eh_depth);
  if (depth > 5) return 0;

  if (depth == 0) {
    const ext4_extent_t* ext = (const ext4_extent_t*)(extent_data + 12);
    for (uint16_t i = 0; i < entries; i++) {
      uint32_t ee_block = erd32((const uint8_t*)&ext[i].ee_block);
      uint32_t ee_len_raw = erd16((const uint8_t*)&ext[i].ee_len);
      /*
       * ext4: ee_len <= 32768 -- инициализированный extent (32768 -- норма
       * для больших файлов!), ee_len > 32768 -- unwritten (fallocate):
       * реальная длина ee_len-32768, данные читаются как нули.
       */
      int      unwritten = ee_len_raw > 32768u;
      uint32_t ee_len = unwritten ? ee_len_raw - 32768u : ee_len_raw;
      uint64_t ee_start = (uint64_t)erd32((const uint8_t*)&ext[i].ee_start_lo) |
                           ((uint64_t)erd16((const uint8_t*)&ext[i].ee_start_hi) << 32);

      if (logical_block >= ee_block && logical_block < (uint64_t)ee_block + ee_len) {
        if (unwritten) return 0;           /* читается как дыра (нули) */
        return ee_start + (logical_block - ee_block);
      }
    }
    return 0;
  }

  const ext4_extent_idx_t* idx = (const ext4_extent_idx_t*)(extent_data + 12);
  int chosen = -1;
  for (uint16_t i = 0; i < entries; i++) {
    uint32_t ei_block = erd32((const uint8_t*)&idx[i].ei_block);
    if (logical_block >= ei_block) chosen = (int)i;
  }

  if (chosen < 0) return 0;

  uint64_t leaf_block = (uint64_t)erd32((const uint8_t*)&idx[chosen].ei_leaf_lo) |
                         ((uint64_t)erd16((const uint8_t*)&idx[chosen].ei_leaf_hi) << 32);

  if (!ext_read_block(ctx, leaf_block, g_ext_ind2buf))
    return 0;

  return ext_resolve_extent_block(ctx, g_ext_ind2buf, logical_block);
}

static uint64_t ext_resolve_block(ext_context_t* ctx, const ext_inode_t* inode, uint64_t logical_block) {
  uint32_t flags = erd32((const uint8_t*)&inode->i_flags);

  if (flags & EXT4_EXTENTS_FL) {
    return ext_resolve_extent_block(ctx, (const uint8_t*)inode->i_block, logical_block);
  }

  uint32_t ptrs_per_block = ctx->block_size / 4u;

  if (logical_block < EXT_DIRECT_BLOCKS) {
    return (uint64_t)erd32((const uint8_t*)&inode->i_block[logical_block]);
  }

  logical_block -= EXT_DIRECT_BLOCKS;

  if (logical_block < ptrs_per_block) {
    uint32_t ind_block = erd32((const uint8_t*)&inode->i_block[12]);
    if (ind_block == 0) return 0;
    if (!ext_read_block(ctx, ind_block, g_ext_indbuf)) return 0;
    return (uint64_t)erd32(g_ext_indbuf + logical_block * 4);
  }
  logical_block -= ptrs_per_block;

  if (logical_block < (uint64_t)ptrs_per_block * ptrs_per_block) {
    uint32_t dind_block = erd32((const uint8_t*)&inode->i_block[13]);
    if (dind_block == 0) return 0;
    if (!ext_read_block(ctx, dind_block, g_ext_indbuf)) return 0;

    uint64_t idx1 = logical_block / ptrs_per_block;
    uint64_t idx2 = logical_block % ptrs_per_block;

    uint32_t ind_block = erd32(g_ext_indbuf + idx1 * 4);
    if (ind_block == 0) return 0;
    if (!ext_read_block(ctx, ind_block, g_ext_ind2buf)) return 0;
    return (uint64_t)erd32(g_ext_ind2buf + idx2 * 4);
  }
  logical_block -= (uint64_t)ptrs_per_block * ptrs_per_block;

  {
    uint32_t tind_block = erd32((const uint8_t*)&inode->i_block[14]);
    if (tind_block == 0) return 0;
    if (!ext_read_block(ctx, tind_block, g_ext_indbuf)) return 0;

    uint64_t ppb2 = (uint64_t)ptrs_per_block * ptrs_per_block;
    uint64_t idx1 = logical_block / ppb2;
    uint64_t rem = logical_block % ppb2;
    uint64_t idx2 = rem / ptrs_per_block;
    uint64_t idx3 = rem % ptrs_per_block;

    uint32_t dind = erd32(g_ext_indbuf + idx1 * 4);
    if (dind == 0) return 0;
    if (!ext_read_block(ctx, dind, g_ext_ind2buf)) return 0;

    uint32_t ind = erd32(g_ext_ind2buf + idx2 * 4);
    if (ind == 0) return 0;
    if (!ext_read_block(ctx, ind, g_ext_indbuf)) return 0;

    return (uint64_t)erd32(g_ext_indbuf + idx3 * 4);
  }
}

static int ext_strcmp_ci(const char* a, const char* b) {
  return ix_utf8_casecmp(a, b);
}

static uint32_t ext_lookup_in_dir(ext_context_t* ctx, uint32_t dir_inode, const char* name) {
  ext_inode_t inode;
  if (!ext_read_inode(ctx, dir_inode, &inode)) return 0;

  uint64_t dir_size = ext_inode_size(&inode);
  uint64_t offset = 0;

  while (offset < dir_size) {
    uint64_t logical_block = offset / ctx->block_size;
    uint64_t phys_block = ext_resolve_block(ctx, &inode, logical_block);
    if (phys_block == 0) break;

    if (!ext_read_block(ctx, phys_block, g_ext_blkbuf)) break;

    uint32_t in_block = (uint32_t)(offset % ctx->block_size);
    while (in_block + 8 <= ctx->block_size) {
      const uint8_t* de = g_ext_blkbuf + in_block;
      uint32_t de_inode = erd32(de);
      uint16_t rec_len = erd16(de + 4);
      uint8_t name_len = de[6];

      if (rec_len < 8 || in_block + rec_len > ctx->block_size) break;

      if (de_inode != 0 && name_len > 0) {
        char entry_name[EXT_NAME_MAX];
        uint8_t copy_len = name_len;
        if (copy_len >= EXT_NAME_MAX) copy_len = EXT_NAME_MAX - 1;
        ix_memcpy(entry_name, de + 8, copy_len);
        entry_name[copy_len] = 0;

        if (ext_strcmp_ci(entry_name, name) == 0)
          return de_inode;
      }

      in_block += rec_len;
      offset = (offset / ctx->block_size) * ctx->block_size + in_block;
    }

    offset = ((offset / ctx->block_size) + 1) * ctx->block_size;
  }

  return 0;
}

static int ext_lookup_path(ext_context_t* ctx, const char* path, uint32_t* out_inode) {
  uint32_t cur = EXT_ROOT_INO;

  if (!path || !path[0] || (path[0] == '/' && !path[1])) {
    *out_inode = EXT_ROOT_INO;
    return 1;
  }

  char component[EXT_NAME_MAX];
  int pi = 0;

  while (path[pi]) {
    if (path[pi] == '/' || path[pi] == '\\') { pi++; continue; }

    int ci = 0;
    while (path[pi] && path[pi] != '/' && path[pi] != '\\' && ci < EXT_NAME_MAX - 1)
      component[ci++] = path[pi++];
    component[ci] = 0;
    if (!component[0]) continue;

    uint32_t found = ext_lookup_in_dir(ctx, cur, component);
    if (found == 0) return 0;
    cur = found;
  }

  *out_inode = cur;
  return 1;
}

int ext_open(ext_context_t* ctx, const char* path, uint32_t flags) {
  (void)flags;
  if (!ctx || !ctx->valid) return -1;

  uint32_t ino;
  if (!ext_lookup_path(ctx, path, &ino)) return -1;

  ext_inode_t inode;
  if (!ext_read_inode(ctx, ino, &inode)) return -1;

  uint16_t mode = erd16((const uint8_t*)&inode.i_mode);
  if ((mode & 0xF000u) == 0x4000u) return -1;

  int h = -1;
  for (int i = 0; i < EXT_MAX_HANDLES; i++) {
    if (!g_ext_files[i].active) { h = i; break; }
  }
  if (h < 0) return -1;

  ix_memset(&g_ext_files[h], 0, sizeof(ext_file_handle_t));
  g_ext_files[h].active = 1;
  g_ext_files[h].inode_num = ino;
  g_ext_files[h].file_size = ext_inode_size(&inode);
  g_ext_files[h].cur_offset = 0;
  g_ext_files[h].is_dir = 0;
  g_ext_files[h].i_flags = erd32((const uint8_t*)&inode.i_flags);
  g_ext_files[h].i_mode = mode;
  ix_memcpy(g_ext_files[h].i_block, inode.i_block, sizeof(inode.i_block));
  return h;
}

int ext_read(ext_context_t* ctx, int handle, void* buffer, uint32_t size) {
  if (handle < 0 || handle >= EXT_MAX_HANDLES) return -1;
  ext_file_handle_t* fh = &g_ext_files[handle];
  if (!fh->active || fh->is_dir || !ctx || !ctx->valid) return -1;

  if (fh->cur_offset >= fh->file_size) return 0;

  uint32_t remaining = size;
  if (fh->cur_offset + remaining > fh->file_size)
    remaining = (uint32_t)(fh->file_size - fh->cur_offset);

  ext_inode_t inode;
  ix_memset(&inode, 0, sizeof(inode));
  ix_memcpy(inode.i_block, fh->i_block, sizeof(inode.i_block));
  inode.i_flags = fh->i_flags;

  uint8_t* dst = (uint8_t*)buffer;
  uint32_t done = 0;

  while (remaining > 0) {
    uint64_t logical_block = fh->cur_offset / ctx->block_size;
    uint32_t in_block_off = (uint32_t)(fh->cur_offset % ctx->block_size);

    g_ext_io_err = 0;
    uint64_t phys = ext_resolve_block(ctx, &inode, logical_block);
    if (g_ext_io_err) break;                 /* сбой чтения индекса -- не нули! */

    uint32_t avail = ctx->block_size - in_block_off;
    if (avail > remaining) avail = remaining;

    if (phys == 0) {
      /* Дыра (sparse) или unwritten extent: внутри размера файла -- нули. */
      ix_memset(dst + done, 0, avail);
    } else {
      if (!ext_read_block(ctx, phys, g_ext_blkbuf)) break;
      ix_memcpy(dst + done, g_ext_blkbuf + in_block_off, avail);
    }
    done += avail;
    remaining -= avail;
    fh->cur_offset += avail;
  }

  return (int)done;
}

int ext_stat(ext_context_t* ctx, const char* path, ix_file_info_t* out) {
  if (!ctx || !ctx->valid || !out) return -1;
  ix_memset(out, 0, sizeof(*out));

  uint32_t ino;
  if (!ext_lookup_path(ctx, path, &ino)) return -1;

  ext_inode_t inode;
  if (!ext_read_inode(ctx, ino, &inode)) return -1;

  uint16_t mode = erd16((const uint8_t*)&inode.i_mode);
  out->size = ext_inode_size(&inode);
  if ((mode & 0xF000u) == 0x4000u) out->attrs |= IX_FS_ATTR_DIR;

  out->created_ns = (uint64_t)erd32((const uint8_t*)&inode.i_ctime) * 1000000000ull;
  out->modified_ns = (uint64_t)erd32((const uint8_t*)&inode.i_mtime) * 1000000000ull;
  out->accessed_ns = (uint64_t)erd32((const uint8_t*)&inode.i_atime) * 1000000000ull;
  return 0;
}

int ext_close(ext_context_t* ctx, int handle) {
  (void)ctx;
  if (handle < 0 || handle >= EXT_MAX_HANDLES) return -1;
  g_ext_files[handle].active = 0;
  return 0;
}

int ext_opendir(ext_context_t* ctx, const char* path) {
  if (!ctx || !ctx->valid) return -1;

  uint32_t ino;
  if (!ext_lookup_path(ctx, path, &ino)) return -1;

  ext_inode_t inode;
  if (!ext_read_inode(ctx, ino, &inode)) return -1;

  uint16_t mode = erd16((const uint8_t*)&inode.i_mode);
  if ((mode & 0xF000u) != 0x4000u) return -1;

  int h = -1;
  for (int i = 0; i < EXT_MAX_HANDLES; i++) {
    if (!g_ext_dirs[i].active) { h = i; break; }
  }
  if (h < 0) return -1;

  ix_memset(&g_ext_dirs[h], 0, sizeof(ext_dir_handle_t));
  g_ext_dirs[h].active = 1;
  g_ext_dirs[h].inode_num = ino;
  g_ext_dirs[h].dir_size = ext_inode_size(&inode);
  g_ext_dirs[h].cur_offset = 0;
  g_ext_dirs[h].i_flags = erd32((const uint8_t*)&inode.i_flags);
  ix_memcpy(g_ext_dirs[h].i_block, inode.i_block, sizeof(inode.i_block));
  return h;
}

int ext_readdir(ext_context_t* ctx, int dir_handle, ix_dirent_t* out) {
  if (dir_handle < 0 || dir_handle >= EXT_MAX_HANDLES) return -1;
  ext_dir_handle_t* dh = &g_ext_dirs[dir_handle];
  if (!dh->active || !ctx || !ctx->valid) return -1;
  ix_memset(out, 0, sizeof(*out));

  ext_inode_t inode;
  ix_memset(&inode, 0, sizeof(inode));
  ix_memcpy(inode.i_block, dh->i_block, sizeof(inode.i_block));
  inode.i_flags = dh->i_flags;

  uint64_t loaded_lb = (uint64_t)-1;

  while (dh->cur_offset < dh->dir_size) {
    uint64_t logical_block = dh->cur_offset / ctx->block_size;
    uint32_t in_block_off = (uint32_t)(dh->cur_offset % ctx->block_size);

    /* Общий g_ext_blkbuf могли затереть между вызовами (чтение файла,
     * поиск пути): блок каталога подгружаем заново, если он не загружен
     * в ЭТОМ вызове. */
    if (logical_block != loaded_lb) {
      uint64_t phys = ext_resolve_block(ctx, &inode, logical_block);
      if (phys == 0) return 0;
      if (!ext_read_block(ctx, phys, g_ext_blkbuf)) return -1;
      loaded_lb = logical_block;
    }

    if (in_block_off + 8 > ctx->block_size) {
      dh->cur_offset = (dh->cur_offset / ctx->block_size + 1) * ctx->block_size;
      continue;
    }

    const uint8_t* de = g_ext_blkbuf + in_block_off;
    uint32_t de_inode = erd32(de);
    uint16_t rec_len = erd16(de + 4);
    uint8_t name_len = de[6];
    uint8_t file_type = de[7];

    if (rec_len < 8 || in_block_off + rec_len > ctx->block_size) {
      dh->cur_offset = (dh->cur_offset / ctx->block_size + 1) * ctx->block_size;
      continue;
    }

    dh->cur_offset += rec_len;

    if (de_inode == 0 || name_len == 0) continue;

    char name[EXT_NAME_MAX];
    uint8_t copy_len = name_len;
    if (copy_len >= EXT_NAME_MAX) copy_len = EXT_NAME_MAX - 1;
    ix_memcpy(name, de + 8, copy_len);
    name[copy_len] = 0;

    if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
      continue;

    ixk_string_copy_n(out->name, name, 256);
    out->is_dir = (file_type == EXT_FT_DIR) ? 1 : 0;

    if (de_inode != 0) {
      ext_inode_t child_inode;
      if (ext_read_inode(ctx, de_inode, &child_inode)) {
        out->size = ext_inode_size(&child_inode);
        out->info.size = out->size;
        uint16_t child_mode = erd16((const uint8_t*)&child_inode.i_mode);
        if ((child_mode & 0xF000u) == 0x4000u) {
          out->is_dir = 1;
          out->info.attrs = IX_FS_ATTR_DIR;
        }
        out->info.modified_ns = (uint64_t)erd32((const uint8_t*)&child_inode.i_mtime) * 1000000000ull;
        out->info.created_ns = (uint64_t)erd32((const uint8_t*)&child_inode.i_ctime) * 1000000000ull;
      }
    }

    return 1;
  }

  return 0;
}

int ext_closedir(ext_context_t* ctx, int dir_handle) {
  (void)ctx;
  if (dir_handle < 0 || dir_handle >= EXT_MAX_HANDLES) return -1;
  g_ext_dirs[dir_handle].active = 0;
  return 0;
}

/* =====================================================================
 *  WRITE-ПОДДЕРЖКА (ArtyomX64 fs-autodetect iteration)
 * =====================================================================
 *
 *  Stateless-операции над путём:
 *
 *    ext_read_at       -- чтение по абсолютному смещению (дыры = нули);
 *    ext_write_at      -- запись с аллокацией блоков (indirect + extents);
 *    ext_create        -- создание файла (alloc inode + запись в каталог);
 *    ext_truncate_path -- обнуление с освобождением блоков.
 *
 *  ЖУРНАЛ (ext3/ext4) НЕ пишется -- как ext2 поверх ext3/4-тома.
 *  metadata_csum/64bit НЕ обновляются: тома с этими фичами требуют
 *  e2fsck после записи (для тестов создаём тома без них).
 *  При модификации каталога сбрасывается флаг htree-индекса (EXT4_INDEX_FL).
 * =================================================================== */

static uint8_t g_ext_bmbuf[EXT_SECBUF_SIZE];
static uint8_t g_ext_dirbuf[EXT_SECBUF_SIZE];

static void ewr16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void ewr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static int ext_write_block(ext_context_t* ctx, uint64_t block, const void* buf) {
  ext_rc_invalidate();
  return ixk_storage_write(ctx->part_lba + block * ctx->block_sectors,
                           ctx->block_sectors, buf);
}

/*
 * Поправить счётчики свободных блоков/инодов в суперблоке.
 * Суперблок -- по байтовому смещению 1024, т.е. сектор 2 от начала
 * раздела.  Смещения ПОЛЕЙ суперблока (не сектора!):
 *   s_free_blocks_count  -- +12,  s_free_inodes_count -- +16.
 * (32/40 -- это s_blocks_per_group / s_inodes_per_group, их трогать
 *  нельзя: раньше код портил геометрию тома.)
 */
static int ext_sb_adjust(ext_context_t* ctx, int free_blocks_delta,
                         int free_inodes_delta) {
  uint8_t sec[512];
  if (!ixk_storage_read(ctx->part_lba + 2, 1, sec)) return 0;

  uint32_t fb = (uint32_t)sec[12] | ((uint32_t)sec[13] << 8) |
                ((uint32_t)sec[14] << 16) | ((uint32_t)sec[15] << 24);
  uint32_t fi = (uint32_t)sec[16] | ((uint32_t)sec[17] << 8) |
                ((uint32_t)sec[18] << 16) | ((uint32_t)sec[19] << 24);

  fb = (int)fb + free_blocks_delta >= 0 ? fb + (uint32_t)free_blocks_delta : 0;
  fi = (int)fi + free_inodes_delta >= 0 ? fi + (uint32_t)free_inodes_delta : 0;

  sec[12] = (uint8_t)(fb & 0xFFu);
  sec[13] = (uint8_t)((fb >> 8) & 0xFFu);
  sec[14] = (uint8_t)((fb >> 16) & 0xFFu);
  sec[15] = (uint8_t)((fb >> 24) & 0xFFu);
  sec[16] = (uint8_t)(fi & 0xFFu);
  sec[17] = (uint8_t)((fi >> 8) & 0xFFu);
  sec[18] = (uint8_t)((fi >> 16) & 0xFFu);
  sec[19] = (uint8_t)((fi >> 24) & 0xFFu);

  ext_rc_invalidate();
  return ixk_storage_write(ctx->part_lba + 2, 1, sec);
}

/* Прочитать дескриптор группы (16-битные поля нам и нужны). */
static int ext_read_group_desc(ext_context_t* ctx, uint32_t group,
                               uint32_t* bb, uint32_t* ib,
                               uint16_t* fb_cnt, uint16_t* fi_cnt) {
  uint64_t gd_block = (uint64_t)ctx->first_data_block + 1;
  uint64_t gd_byte  = (uint64_t)group * ctx->desc_size;
  uint64_t blk      = gd_block + gd_byte / ctx->block_size;
  uint32_t off      = (uint32_t)(gd_byte % ctx->block_size);

  if (!ext_read_block(ctx, blk, g_ext_secbuf)) return 0;

  const uint8_t* gd = g_ext_secbuf + off;
  *bb = erd32(gd + 0);
  *ib = erd32(gd + 4);
  if (fb_cnt) *fb_cnt = erd16(gd + 12);
  if (fi_cnt) *fi_cnt = erd16(gd + 14);
  return 1;
}

/* Записать обратно счётчики дескриптора группы. */
static int ext_write_group_counts(ext_context_t* ctx, uint32_t group,
                                  int blocks_delta, int inodes_delta) {
  uint64_t gd_block = (uint64_t)ctx->first_data_block + 1;
  uint64_t gd_byte  = (uint64_t)group * ctx->desc_size;
  uint64_t blk      = gd_block + gd_byte / ctx->block_size;
  uint32_t off      = (uint32_t)(gd_byte % ctx->block_size);

  if (!ext_read_block(ctx, blk, g_ext_secbuf)) return 0;

  uint8_t* gd = g_ext_secbuf + off;

  int fb = (int)erd16(gd + 12) + blocks_delta;
  int fi = (int)erd16(gd + 14) + inodes_delta;
  if (fb < 0) fb = 0;
  if (fi < 0) fi = 0;

  ewr16(gd + 12, (uint16_t)fb);
  ewr16(gd + 14, (uint16_t)fi);

  return ext_write_block(ctx, blk, g_ext_secbuf);
}

/*
 * Аллокация блока.  hint -- желаемый соседний блок (0 = без подсказки):
 * сначала ищем свободный бит в той же группе после hint, затем общий
 * скан с группы 0.  Возвращает номер блока или 0.
 */
static uint32_t ext_balloc(ext_context_t* ctx, uint64_t hint) {
  for (int pass = 0; pass < 2; pass++) {
    uint32_t g_begin = (pass == 0 && hint) ? (uint32_t)((hint - ctx->first_data_block) / ctx->blocks_per_group) : 0;
    if (g_begin >= ctx->num_groups) g_begin = 0;

    for (uint32_t gk = 0; gk < ctx->num_groups; gk++) {
      uint32_t g = (pass == 0) ? g_begin + gk : gk;
      if (g >= ctx->num_groups) continue;

      uint32_t bb, ib;
      if (!ext_read_group_desc(ctx, g, &bb, &ib, 0, 0)) continue;
      if (bb == 0) continue;

      if (!ext_read_block(ctx, bb, g_ext_bmbuf)) continue;

      uint32_t first_bit = 0;
      if (pass == 0 && hint) {
        uint64_t hb = (hint > ctx->first_data_block) ? (hint - ctx->first_data_block) : 0;
        if (hb / ctx->blocks_per_group == g)
          first_bit = (uint32_t)(hb % ctx->blocks_per_group);
      }

      for (uint32_t bit = first_bit; bit < ctx->blocks_per_group; bit++) {
        uint32_t byte = bit >> 3;
        uint8_t  mask = (uint8_t)(1u << (bit & 7u));
        if (g_ext_bmbuf[byte] & mask) continue;

        uint64_t block = (uint64_t)ctx->first_data_block +
                         (uint64_t)g * ctx->blocks_per_group + bit;

        g_ext_bmbuf[byte] |= mask;
        if (!ext_write_block(ctx, bb, g_ext_bmbuf)) return 0;

        ext_write_group_counts(ctx, g, -1, 0);
        ext_sb_adjust(ctx, -1, 0);

        /* Обнулить выданный блок. */
        static uint8_t zeros[EXT_SECBUF_SIZE];
        if (!ext_write_block(ctx, block, zeros)) return 0;

        return (uint32_t)block;
      }
    }
  }
  return 0;
}

static int ext_bfree(ext_context_t* ctx, uint32_t block) {
  if (block < ctx->first_data_block) return 0;

  uint32_t g   = (block - ctx->first_data_block) / ctx->blocks_per_group;
  uint32_t bit = (block - ctx->first_data_block) % ctx->blocks_per_group;

  uint32_t bb, ib;
  if (!ext_read_group_desc(ctx, g, &bb, &ib, 0, 0)) return 0;
  if (!ext_read_block(ctx, bb, g_ext_bmbuf)) return 0;

  uint8_t mask = (uint8_t)(1u << (bit & 7u));
  if (!(g_ext_bmbuf[bit >> 3] & mask)) return 1;   /* уже свободен */

  g_ext_bmbuf[bit >> 3] &= (uint8_t)~mask;
  if (!ext_write_block(ctx, bb, g_ext_bmbuf)) return 0;

  ext_write_group_counts(ctx, g, 1, 0);
  ext_sb_adjust(ctx, 1, 0);
  return 1;
}

/* Запись inode обратно в таблицу (RMW блока таблицы). */
static int ext_write_inode(ext_context_t* ctx, uint32_t inode_num,
                           const ext_inode_t* in) {
  if (inode_num == 0) return 0;

  uint32_t group = (inode_num - 1) / ctx->inodes_per_group;
  uint32_t index = (inode_num - 1) % ctx->inodes_per_group;

  uint64_t gd_block = (uint64_t)ctx->first_data_block + 1;
  uint64_t gd_byte  = (uint64_t)group * ctx->desc_size;
  uint64_t blk      = gd_block + gd_byte / ctx->block_size;
  uint32_t off      = (uint32_t)(gd_byte % ctx->block_size);

  if (!ext_read_block(ctx, blk, g_ext_secbuf)) return 0;

  uint64_t itab;
  if (ctx->has_64bit && ctx->desc_size >= 64) {
    const ext4_group_desc_t* gd = (const ext4_group_desc_t*)(g_ext_secbuf + off);
    itab = (uint64_t)erd32((const uint8_t*)&gd->bg_inode_table) |
           ((uint64_t)erd32((const uint8_t*)&gd->bg_inode_table_hi) << 32);
  } else {
    const ext_group_desc_t* gd = (const ext_group_desc_t*)(g_ext_secbuf + off);
    itab = (uint64_t)erd32((const uint8_t*)&gd->bg_inode_table);
  }

  uint64_t ibyte = (uint64_t)index * ctx->inode_size;
  uint64_t iblk  = itab + ibyte / ctx->block_size;
  uint32_t ioff  = (uint32_t)(ibyte % ctx->block_size);

  if (!ext_read_block(ctx, iblk, g_ext_secbuf)) return 0;
  ix_memcpy(g_ext_secbuf + ioff, in, sizeof(ext_inode_t));
  return ext_write_block(ctx, iblk, g_ext_secbuf);
}

/* Аллокация inode. Возвращает номер или 0. */
static uint32_t ext_ialloc(ext_context_t* ctx) {
  for (uint32_t g = 0; g < ctx->num_groups; g++) {
    uint32_t bb, ib;
    if (!ext_read_group_desc(ctx, g, &bb, &ib, 0, 0)) continue;
    if (ib == 0) continue;

    if (!ext_read_block(ctx, ib, g_ext_bmbuf)) continue;

    uint32_t bits = ctx->inodes_per_group;
    for (uint32_t bit = 0; bit < bits; bit++) {
      uint8_t mask = (uint8_t)(1u << (bit & 7u));
      if (g_ext_bmbuf[bit >> 3] & mask) continue;

      g_ext_bmbuf[bit >> 3] |= mask;
      if (!ext_write_block(ctx, ib, g_ext_bmbuf)) return 0;

      ext_write_group_counts(ctx, g, 0, -1);
      ext_sb_adjust(ctx, 0, -1);

      uint32_t ino = g * ctx->inodes_per_group + bit + 1u;

      /* Обнулить inode на диске. */
      ext_inode_t zero;
      ix_memset(&zero, 0, sizeof(zero));
      ext_write_inode(ctx, ino, &zero);

      return ino;
    }
  }
  return 0;
}

/*
 * Обеспечить отображение логического блока lb для inode (indirect-схема).
 * Выделяет и связывает недостающие блоки.  inode в памяти ДОЛЖЕН быть
 * записан вызывателем после серии вызовов.
 * Возвращает физический блок или 0.
 */
static uint32_t ext_ensure_indirect(ext_context_t* ctx, ext_inode_t* ino,
                                    uint64_t lb) {
  uint32_t ppb = ctx->block_size / 4u;
  uint32_t phys;

  if (lb < EXT_DIRECT_BLOCKS) {
    phys = erd32((const uint8_t*)&ino->i_block[lb]);
    if (phys == 0) {
      phys = ext_balloc(ctx, lb ? erd32((const uint8_t*)&ino->i_block[lb - 1]) : 0);
      if (!phys) return 0;
      ewr32((uint8_t*)&ino->i_block[lb], phys);
      ino->i_blocks += ctx->block_size / 512u;
    }
    return phys;
  }

  uint64_t lb2 = lb - EXT_DIRECT_BLOCKS;

  if (lb2 < ppb) {
    uint32_t ind = erd32((const uint8_t*)&ino->i_block[12]);
    if (ind == 0) {
      ind = ext_balloc(ctx, 0);
      if (!ind) return 0;
      ewr32((uint8_t*)&ino->i_block[12], ind);
      ino->i_blocks += ctx->block_size / 512u;
    }
    if (!ext_read_block(ctx, ind, g_ext_indbuf)) return 0;
    phys = erd32(g_ext_indbuf + lb2 * 4);
    if (phys == 0) {
      phys = ext_balloc(ctx, 0);
      if (!phys) return 0;
      ewr32(g_ext_indbuf + lb2 * 4, phys);
      if (!ext_write_block(ctx, ind, g_ext_indbuf)) return 0;
      ino->i_blocks += ctx->block_size / 512u;
    }
    return phys;
  }

  lb2 -= ppb;

  if (lb2 < (uint64_t)ppb * ppb) {
    uint32_t dind = erd32((const uint8_t*)&ino->i_block[13]);
    if (dind == 0) {
      dind = ext_balloc(ctx, 0);
      if (!dind) return 0;
      ewr32((uint8_t*)&ino->i_block[13], dind);
      ino->i_blocks += ctx->block_size / 512u;
    }
    if (!ext_read_block(ctx, dind, g_ext_indbuf)) return 0;

    uint64_t i1 = lb2 / ppb;
    uint64_t i2 = lb2 % ppb;

    uint32_t ind = erd32(g_ext_indbuf + i1 * 4);
    if (ind == 0) {
      ind = ext_balloc(ctx, 0);
      if (!ind) return 0;
      ewr32(g_ext_indbuf + i1 * 4, ind);
      if (!ext_write_block(ctx, dind, g_ext_indbuf)) return 0;
      ino->i_blocks += ctx->block_size / 512u;
    }
    if (!ext_read_block(ctx, ind, g_ext_ind2buf)) return 0;

    phys = erd32(g_ext_ind2buf + i2 * 4);
    if (phys == 0) {
      phys = ext_balloc(ctx, 0);
      if (!phys) return 0;
      ewr32(g_ext_ind2buf + i2 * 4, phys);
      if (!ext_write_block(ctx, ind, g_ext_ind2buf)) return 0;
      ino->i_blocks += ctx->block_size / 512u;
    }
    return phys;
  }

  /* Тройная косвенность не поддерживается для записи (файлы > ~4 ГиБ). */
  return 0;
}

/*
 * Поиск ПОСЛЕДНЕГО extent в дереве (правый крайний лист).
 * out_leaf_block = 0, если лист -- корень в inode.
 * Возвращает индекс последней записи в листе или -1.
 */
static int ext_find_last_extent(ext_context_t* ctx, const uint8_t* node,
                                uint64_t* out_leaf_block) {
  const ext4_extent_header_t* hdr = (const ext4_extent_header_t*)node;
  if (erd16((const uint8_t*)&hdr->eh_magic) != EXT4_EXTENT_MAGIC) return -1;

  uint16_t entries = erd16((const uint8_t*)&hdr->eh_entries);
  uint16_t depth   = erd16((const uint8_t*)&hdr->eh_depth);

  if (entries == 0) return -1;

  if (depth == 0) {
    *out_leaf_block = 0;
    return (int)(entries - 1);
  }

  const ext4_extent_idx_t* idx = (const ext4_extent_idx_t*)(node + 12);
  uint64_t leaf = (uint64_t)erd32((const uint8_t*)&idx[entries - 1].ei_leaf_lo) |
                  ((uint64_t)erd16((const uint8_t*)&idx[entries - 1].ei_leaf_hi) << 32);

  for (;;) {
    if (!ext_read_block(ctx, leaf, g_ext_ind2buf)) return -1;
    const ext4_extent_header_t* lh = (const ext4_extent_header_t*)g_ext_ind2buf;
    uint16_t le = erd16((const uint8_t*)&lh->eh_entries);
    uint16_t ld = erd16((const uint8_t*)&lh->eh_depth);
    if (le == 0) return -1;

    if (ld == 0) {
      *out_leaf_block = leaf;
      return (int)(le - 1);
    }

    const ext4_extent_idx_t* lidx = (const ext4_extent_idx_t*)(g_ext_ind2buf + 12);
    leaf = (uint64_t)erd32((const uint8_t*)&lidx[le - 1].ei_leaf_lo) |
           ((uint64_t)erd16((const uint8_t*)&lidx[le - 1].ei_leaf_hi) << 32);
  }
}

/*
 * Присоединить один новый логический блок lb к extent-дереву.
 * Возвращает физический блок или 0.
 */
static uint32_t ext_extent_append_block(ext_context_t* ctx, ext_inode_t* ino,
                                        uint32_t lb) {
  uint64_t leaf_block = 0;
  int      last_idx   = ext_find_last_extent(ctx, (const uint8_t*)ino->i_block,
                                             &leaf_block);

  uint32_t new_phys;

  if (last_idx >= 0) {
    /* Прочитать последний extent. */
    ext4_extent_t last;
    if (leaf_block == 0) {
      ix_memcpy(&last, (const uint8_t*)ino->i_block + 12 + (uint32_t)last_idx * 12u,
                sizeof(last));
    } else {
      ix_memcpy(&last, g_ext_ind2buf + 12 + (uint32_t)last_idx * 12u,
                sizeof(last));
    }

    uint32_t ee_block = erd32((const uint8_t*)&last.ee_block);
    uint16_t ee_len   = erd16((const uint8_t*)&last.ee_len) & 0x7FFFu;
    uint64_t ee_start = (uint64_t)erd32((const uint8_t*)&last.ee_start_lo) |
                        ((uint64_t)erd16((const uint8_t*)&last.ee_start_hi) << 32);

    if (lb == ee_block + ee_len && ee_len < 0x8000u) {
      /* Идеальный случай: продолжить последний extent. */
      new_phys = ext_balloc(ctx, ee_start + ee_len - 1u);
      if (!new_phys) return 0;

      if (new_phys == ee_start + ee_len) {
        ewr16((uint8_t*)&last.ee_len, (uint16_t)(ee_len + 1u));
        ino->i_blocks += ctx->block_size / 512u;

        if (leaf_block == 0) {
          ix_memcpy((uint8_t*)ino->i_block + 12 + (uint32_t)last_idx * 12u,
                    &last, sizeof(last));
        } else {
          ix_memcpy(g_ext_ind2buf + 12 + (uint32_t)last_idx * 12u,
                    &last, sizeof(last));
          if (!ext_write_block(ctx, leaf_block, g_ext_ind2buf)) return 0;
        }
        return new_phys;
      }
      /* Не смежный -- упадём ниже на обычное добавление. */
    }

    /*
     * Новый extent: нужен свободный слот в заголовке листа.
     */
    uint16_t eh_entries, eh_max;
    if (leaf_block == 0) {
      eh_entries = erd16((const uint8_t*)ino->i_block + 2);
      /* eh_max -- по смещению +4; +6 -- это eh_depth (у листа 0)! */
      eh_max     = erd16((const uint8_t*)ino->i_block + 4);
    } else {
      eh_entries = erd16(g_ext_ind2buf + 2);
      eh_max     = erd16(g_ext_ind2buf + 4);
    }

    if (eh_entries >= eh_max) {
      /* Рост дерева (depth+1) не реализован в этой итерации. */
      return 0;
    }

    new_phys = ext_balloc(ctx, ee_start + ee_len - 1u);
    if (!new_phys) return 0;

    /* Вставка в конец списка записей (ee_block монотонно растут). */
    if (leaf_block == 0) {
      ewr16((uint8_t*)ino->i_block + 2, (uint16_t)(eh_entries + 1u));
      ext4_extent_t* slot = (ext4_extent_t*)((uint8_t*)ino->i_block + 12 +
                                             (uint32_t)eh_entries * 12u);
      ewr32((uint8_t*)&slot->ee_block, lb);
      ewr16((uint8_t*)&slot->ee_len, 1);
      ewr16((uint8_t*)&slot->ee_start_hi, (uint16_t)(((uint64_t)new_phys) >> 32));
      ewr32((uint8_t*)&slot->ee_start_lo, (uint32_t)(new_phys & 0xFFFFFFFFu));
    } else {
      ewr16(g_ext_ind2buf + 2, (uint16_t)(eh_entries + 1u));
      uint8_t* slot = g_ext_ind2buf + 12 + (uint32_t)eh_entries * 12u;
      ewr32(slot + 0, lb);
      ewr16(slot + 4, 1);
      ewr16(slot + 6, (uint16_t)(((uint64_t)new_phys) >> 32));
      ewr32(slot + 8, (uint32_t)(new_phys & 0xFFFFFFFFu));
      if (!ext_write_block(ctx, leaf_block, g_ext_ind2buf)) return 0;
    }

    ino->i_blocks += ctx->block_size / 512u;
    return new_phys;
  }

  /* Дерево пустое -- создаём первый extent в корне. */
  new_phys = ext_balloc(ctx, 0);
  if (!new_phys) return 0;

  ewr16((uint8_t*)ino->i_block + 0, EXT4_EXTENT_MAGIC);
  ewr16((uint8_t*)ino->i_block + 2, 1);      /* eh_entries */
  ewr16((uint8_t*)ino->i_block + 4, 4);      /* eh_max (60 байт корня) */
  ewr16((uint8_t*)ino->i_block + 6, 0);      /* eh_depth */
  ewr32((uint8_t*)ino->i_block + 8, 0);      /* eh_generation */

  ext4_extent_t* slot = (ext4_extent_t*)((uint8_t*)ino->i_block + 12);
  ewr32((uint8_t*)&slot->ee_block, lb);
  ewr16((uint8_t*)&slot->ee_len, 1);
  ewr16((uint8_t*)&slot->ee_start_hi, (uint16_t)(((uint64_t)new_phys) >> 32));
  ewr32((uint8_t*)&slot->ee_start_lo, (uint32_t)(new_phys & 0xFFFFFFFFu));

  ino->i_blocks += ctx->block_size / 512u;
  ino->i_flags |= EXT4_EXTENTS_FL;
  return new_phys;
}

/*
 * Кэш поиска пути -> inode для ext_read_at().  VFS читает файл кусками,
 * и без кэша каждый кусок заново разбирал путь (линейный просмотр каталогов
 * -- на каталоге в тысячи записей это секунды на кусок).  Любая запись
 * на том сбрасывает кэш.
 */
#define EXT_RC_N        4
#define EXT_RC_PATH_MAX 256

typedef struct {
  int                  valid;
  const ext_context_t* ctx;
  char                 path[EXT_RC_PATH_MAX];
  ext_inode_t          ino;
  uint32_t             stamp;
} ext_rc_t;

static ext_rc_t g_ext_rc[EXT_RC_N];
static uint32_t g_ext_rc_tick = 0;

static void ext_rc_invalidate(void) {
  for (int i = 0; i < EXT_RC_N; i++) g_ext_rc[i].valid = 0;
}

/* inode файла/каталога по пути (через кэш).  0 = не найден. */
static const ext_inode_t* ext_rc_get(ext_context_t* ctx, const char* path) {
  int plen = 0;
  while (path[plen]) plen++;

  if (plen < EXT_RC_PATH_MAX) {
    ext_rc_t* victim = &g_ext_rc[0];
    for (int i = 0; i < EXT_RC_N; i++) {
      ext_rc_t* e = &g_ext_rc[i];
      if (e->valid && e->ctx == ctx && ix_memcmp(e->path, path, (uint32_t)plen + 1u) == 0) {
        e->stamp = ++g_ext_rc_tick;
        return &e->ino;
      }
      if (!e->valid) victim = e;
      else if (victim->valid && e->stamp < victim->stamp) victim = e;
    }

    uint32_t ino_num;
    if (!ext_lookup_path(ctx, path, &ino_num)) return 0;
    if (!ext_read_inode(ctx, ino_num, &victim->ino)) return 0;
    victim->valid = 1;
    victim->ctx = ctx;
    ix_memcpy(victim->path, path, (uint32_t)plen + 1u);
    victim->stamp = ++g_ext_rc_tick;
    return &victim->ino;
  }

  /* Очень длинный путь: без кэша (статический буфер на один вызов). */
  static ext_inode_t tmp;
  uint32_t ino_num;
  if (!ext_lookup_path(ctx, path, &ino_num)) return 0;
  if (!ext_read_inode(ctx, ino_num, &tmp)) return 0;
  return &tmp;
}

int ext_read_at(ext_context_t* ctx, const char* path,
                uint32_t offset, void* buffer, uint32_t size) {
  if (!ctx || !ctx->valid || !path || !buffer) return -1;

  const ext_inode_t* inp = ext_rc_get(ctx, path);
  if (!inp) return -1;
  ext_inode_t ino = *inp;       /* копия: ext_resolve_block берёт указатель */

  uint16_t mode = erd16((const uint8_t*)&ino.i_mode);
  if ((mode & 0xF000u) == 0x4000u) return -1;

  uint64_t fsize = ext_inode_size(&ino);
  if ((uint64_t)offset >= fsize || size == 0) return 0;
  if ((uint64_t)offset + size > fsize) size = (uint32_t)(fsize - offset);

  uint8_t* dst = (uint8_t*)buffer;
  uint32_t done = 0;

  while (done < size) {
    uint64_t lb  = ((uint64_t)offset + done) / ctx->block_size;
    uint32_t off = (uint32_t)(((uint64_t)offset + done) % ctx->block_size);

    g_ext_io_err = 0;
    uint64_t phys = ext_resolve_block(ctx, &ino, lb);
    if (g_ext_io_err) break;            /* сбой чтения индекса -- не нули */

    uint32_t avail = ctx->block_size - off;
    if (avail > size - done) avail = size - done;

    if (phys == 0) {
      /* Дыра / unwritten-extent -- читается как нули. */
      ix_memset(dst + done, 0, avail);
    } else {
      if (!ext_read_block(ctx, phys, g_ext_blkbuf)) break;
      ix_memcpy(dst + done, g_ext_blkbuf + off, avail);
    }

    done += avail;
  }

  return (int)done;
}

int ext_write_at(ext_context_t* ctx, const char* path,
                 uint32_t offset, const void* buffer, uint32_t size) {
  if (!ctx || !ctx->valid || !path || !buffer || size == 0) return -1;

  uint32_t ino_num;
  if (!ext_lookup_path(ctx, path, &ino_num)) return -1;

  ext_inode_t ino;
  if (!ext_read_inode(ctx, ino_num, &ino)) return -1;

  uint16_t mode = erd16((const uint8_t*)&ino.i_mode);
  if ((mode & 0xF000u) == 0x4000u) return -1;

  uint64_t old_size = ext_inode_size(&ino);
  int      is_extent = (erd32((const uint8_t*)&ino.i_flags) & EXT4_EXTENTS_FL) ? 1 : 0;

  const uint8_t* src = (const uint8_t*)buffer;
  uint32_t done = 0;

  while (done < size) {
    uint64_t lb  = ((uint64_t)offset + done) / ctx->block_size;
    uint32_t off = (uint32_t)(((uint64_t)offset + done) % ctx->block_size);

    uint32_t avail = ctx->block_size - off;
    if (avail > size - done) avail = size - done;

    uint64_t phys;

    if (is_extent) {
      phys = ext_resolve_extent_block(ctx, (const uint8_t*)ino.i_block, lb);
      if (phys == 0) {
        phys = ext_extent_append_block(ctx, &ino, (uint32_t)lb);
        if (phys == 0) { ext_write_inode(ctx, ino_num, &ino); return (int)done; }
      }
    } else {
      phys = ext_ensure_indirect(ctx, &ino, lb);
      if (phys == 0) { ext_write_inode(ctx, ino_num, &ino); return (int)done; }
    }

    if (off == 0 && avail == ctx->block_size) {
      ix_memcpy(g_ext_blkbuf, src + done, ctx->block_size);
      if (!ext_write_block(ctx, phys, g_ext_blkbuf)) {
        ext_write_inode(ctx, ino_num, &ino);
        return (int)done;
      }
    } else {
      if (!ext_read_block(ctx, phys, g_ext_blkbuf)) {
        ext_write_inode(ctx, ino_num, &ino);
        return (int)done;
      }
      ix_memcpy(g_ext_blkbuf + off, src + done, avail);
      if (!ext_write_block(ctx, phys, g_ext_blkbuf)) {
        ext_write_inode(ctx, ino_num, &ino);
        return (int)done;
      }
    }

    done += avail;
  }

  uint64_t new_size = (uint64_t)offset + size;
  if (new_size > old_size) {
    ewr32((uint8_t*)&ino.i_size, (uint32_t)(new_size & 0xFFFFFFFFu));
    ewr32((uint8_t*)&ino.i_size_high, (uint32_t)(new_size >> 32));
  }

  ext_write_inode(ctx, ino_num, &ino);
  return (int)done;
}

/*
 * Добавить запись о файле в каталог.
 * Не хватает места в существующих блоках -- каталог растёт новым блоком.
 */
static int ext_add_dir_entry(ext_context_t* ctx, uint32_t dir_ino,
                             const char* name, uint32_t child_ino,
                             uint8_t file_type) {
  ext_inode_t dino;
  if (!ext_read_inode(ctx, dir_ino, &dino)) return -1;

  uint16_t dmode = erd16((const uint8_t*)&dino.i_mode);
  if ((dmode & 0xF000u) != 0x4000u) return -1;

  uint32_t nl = 0;
  while (name[nl]) nl++;
  if (nl == 0 || nl > 255) return -1;

  uint32_t needed = (8u + nl + 3u) & ~3u;

  uint64_t dsize = ext_inode_size(&dino);
  int is_extent = (erd32((const uint8_t*)&dino.i_flags) & EXT4_EXTENTS_FL) ? 1 : 0;

  /* 1. Ищем место в существующих блоках. */
  for (uint64_t lb = 0; (lb + 1) * ctx->block_size <= dsize; lb++) {
    uint64_t phys = ext_resolve_block(ctx, &dino, lb);
    if (phys == 0) break;

    if (!ext_read_block(ctx, phys, g_ext_dirbuf)) return -1;

    uint32_t pos = 0;
    /* Последняя РЕАЛЬНАЯ запись -- цель вставки, если блок кончается
     * фейковой checksum-записью (metadata_csum: inode==0, name_len==0,
     * file_type==0xDE).  На томах без metadata_csum она не встречается. */
    uint32_t prev_pos = 0;
    uint16_t prev_rlen = 0;
    uint8_t  prev_nl = 0;
    int      have_prev = 0;

    while (pos + 8 <= ctx->block_size) {
      uint8_t* de   = g_ext_dirbuf + pos;
      uint16_t rlen = erd16(de + 4);
      if (rlen < 8 || pos + rlen > ctx->block_size) break;

      uint32_t de_ino  = erd32(de);
      uint8_t  last_nl = de[6];

      int is_fake_csum =
        (de_ino == 0 && last_nl == 0 && de[7] == 0xDE);

      if (is_fake_csum && have_prev) {
        /* Вставить в хвост предыдущей реальной записи; новая запись
         * поглощает пространство до конца блока (включая бывшую
         * checksum-запись).  На metadata_csum-томах после записи
         * нужен e2fsck -- как и для прочих модификаций. */
        uint32_t pmin = ((8u + (uint32_t)prev_nl + 3u) & ~3u);

        if ((uint32_t)prev_rlen - pmin >= needed) {
          uint8_t* pde = g_ext_dirbuf + prev_pos;
          ewr16(pde + 4, (uint16_t)pmin);

          uint8_t* nd = pde + pmin;
          ewr32(nd + 0, child_ino);
          ewr16(nd + 4, (uint16_t)(ctx->block_size - (prev_pos + pmin)));
          nd[6] = (uint8_t)nl;
          nd[7] = file_type;
          ix_memcpy(nd + 8, name, nl);

          if (!ext_write_block(ctx, phys, g_ext_dirbuf)) return -1;
          return 0;
        }
        break;   /* места в блоке нет -- рост каталога */
      }

      if (pos + rlen >= ctx->block_size) {
        uint32_t last_min = (de_ino != 0)
                              ? ((8u + (uint32_t)last_nl + 3u) & ~3u)
                              : 8u;

        if (rlen - last_min >= needed) {
          uint32_t new_rlen = last_min;

          /* Обрезать rec_len последней записи, иначе новая запись
           * остаётся "спрятанной" за её хвостом и lookup её не видит. */
          ewr16(de + 4, (uint16_t)new_rlen);

          uint8_t* nd = g_ext_dirbuf + pos + new_rlen;
          ewr32(nd + 0, child_ino);
          ewr16(nd + 4, (uint16_t)(rlen - new_rlen));
          nd[6] = (uint8_t)nl;
          nd[7] = file_type;
          ix_memcpy(nd + 8, name, nl);

          if (!ext_write_block(ctx, phys, g_ext_dirbuf)) return -1;
          return 0;
        }
        break;
      }

      prev_pos = pos;
      prev_rlen = rlen;
      prev_nl  = last_nl;
      have_prev = 1;
      pos += rlen;
    }
  }

  /* 2. Новый блок в конце каталога. */
  uint64_t new_lb = dsize / ctx->block_size;

  uint32_t phys;
  if (is_extent) {
    phys = ext_extent_append_block(ctx, &dino, (uint32_t)new_lb);
  } else {
    phys = ext_ensure_indirect(ctx, &dino, new_lb);
  }
  if (phys == 0) return -1;

  static uint8_t zeros[EXT_SECBUF_SIZE];
  if (!ext_write_block(ctx, phys, zeros)) return -1;

  uint8_t* de = g_ext_dirbuf;
  ewr32(de + 0, child_ino);
  ewr16(de + 4, (uint16_t)ctx->block_size);
  de[6] = (uint8_t)nl;
  de[7] = file_type;
  ix_memcpy(de + 8, name, nl);
  if (!ext_write_block(ctx, phys, g_ext_dirbuf)) return -1;

  /* Размер каталога + сброс htree-индекса. */
  ewr32((uint8_t*)&dino.i_size, (uint32_t)(dsize + ctx->block_size));
  uint32_t dflags = erd32((const uint8_t*)&dino.i_flags);
  dflags &= ~0x1000u;   /* EXT4_INDEX_FL */
  ewr32((uint8_t*)&dino.i_flags, dflags);

  return ext_write_inode(ctx, dir_ino, &dino) ? 0 : -1;
}

int ext_create(ext_context_t* ctx, const char* path) {
  if (!ctx || !ctx->valid || !path || !path[0]) return -1;

  /* Родитель + лист. */
  char parent[EXT_NAME_MAX];
  const char* leaf;
  int plen = 0, last_slash = -1;
  while (path[plen]) {
    if (path[plen] == '/') last_slash = plen;
    plen++;
  }
  leaf = (last_slash >= 0) ? path + last_slash + 1 : path;
  if (!leaf[0]) return -1;

  if (last_slash >= 0) {
    int n = last_slash;
    if (n >= (int)sizeof(parent)) n = sizeof(parent) - 1;
    ix_memcpy(parent, path, n);
    parent[n] = 0;
  } else {
    parent[0] = 0;
  }

  uint32_t parent_ino = EXT_ROOT_INO;
  if (parent[0]) {
    if (!ext_lookup_path(ctx, parent, &parent_ino)) return -1;
  }

  ext_inode_t pino;
  if (!ext_read_inode(ctx, parent_ino, &pino)) return -1;
  {
    uint16_t pm = erd16((const uint8_t*)&pino.i_mode);
    if ((pm & 0xF000u) != 0x4000u) return -1;
  }

  /* Уже существует? */
  {
    uint32_t existing = ext_lookup_in_dir(ctx, parent_ino, leaf);
    if (existing) return -1;
  }

  uint32_t child = ext_ialloc(ctx);
  if (!child) return -1;

  ext_inode_t ci;
  ix_memset(&ci, 0, sizeof(ci));
  ewr16((uint8_t*)&ci.i_mode, 0x81A4u);   /* regular 0644 */
  ewr16((uint8_t*)&ci.i_uid, 0);
  ewr32((uint8_t*)&ci.i_size, 0);
  ewr16((uint8_t*)&ci.i_gid, 0);
  ewr16((uint8_t*)&ci.i_links_count, 1);
  ewr32((uint8_t*)&ci.i_blocks, 0);

  if (!ext_write_inode(ctx, child, &ci)) return -1;

  if (ext_add_dir_entry(ctx, parent_ino, leaf, child, EXT_FT_REG_FILE) != 0) {
    /* TODO: откатить ialloc. Пока оставляем inode занятым. */
    return -1;
  }

  return 0;
}

static void ext_free_ind_tree_level(ext_context_t* ctx, uint32_t blk, int level);

/* Освободить содержимое одиночного косвенного блока. */
static void ext_free_ind_block(ext_context_t* ctx, uint32_t ind_blk,
                               int level) {
  if (ind_blk == 0 || level <= 0) return;

  if (!ext_read_block(ctx, ind_blk, g_ext_bmbuf)) return;

  uint32_t ppb = ctx->block_size / 4u;
  for (uint32_t i = 0; i < ppb; i++) {
    uint32_t child = erd32(g_ext_bmbuf + i * 4);
    if (child == 0) continue;
    if (level > 1) ext_free_ind_block(ctx, child, level - 1);
    ext_bfree(ctx, child);
  }
}

static void ext_free_ind_tree_level(ext_context_t* ctx, uint32_t blk, int level) {
  ext_free_ind_block(ctx, blk, level);
}


/*
 * Освободить inode: снять бит в bitmap inode-ов группы, вернуть
 * счётчики группе и суперблоку.  Ссылки на блоки к этому моменту уже
 * должны быть освобождены.
 */
static int ext_ifree(ext_context_t* ctx, uint32_t inode_num) {
  if (inode_num == 0) return 0;

  uint32_t g   = (inode_num - 1) / ctx->inodes_per_group;
  uint32_t bit = (inode_num - 1) % ctx->inodes_per_group;

  uint32_t bb, ib;
  if (!ext_read_group_desc(ctx, g, &bb, &ib, 0, 0)) return 0;
  if (ib == 0) return 0;
  if (!ext_read_block(ctx, ib, g_ext_bmbuf)) return 0;

  uint8_t mask = (uint8_t)(1u << (bit & 7u));
  if (!(g_ext_bmbuf[bit >> 3] & mask)) return 1;

  g_ext_bmbuf[bit >> 3] &= (uint8_t)~mask;
  if (!ext_write_block(ctx, ib, g_ext_bmbuf)) return 0;

  ext_write_group_counts(ctx, g, 0, 1);
  ext_sb_adjust(ctx, 0, 1);
  return 1;
}

/*
 * Убрать запись из каталога.  Пространство не теряется: rec_len
 * предыдущей записи расширяется на всю исчезающую, что для записи в
 * середине блока сохраняет раскладку блока байт-в-байт и не требует
 * перестройки.
 */
static int ext_remove_dir_entry(ext_context_t* ctx, uint32_t dir_ino,
                                const char* name) {
  ext_inode_t dino;
  if (!ext_read_inode(ctx, dir_ino, &dino)) return -1;

  uint16_t dmode = erd16((const uint8_t*)&dino.i_mode);
  if ((dmode & 0xF000u) != 0x4000u) return -1;

  uint32_t nl = 0;
  while (name[nl]) nl++;
  if (nl == 0 || nl > 255) return -1;

  uint64_t dsize = ext_inode_size(&dino);

  for (uint64_t lb = 0; (lb + 1) * ctx->block_size <= dsize; lb++) {
    uint64_t phys = ext_resolve_block(ctx, &dino, lb);
    if (phys == 0) break;
    if (!ext_read_block(ctx, phys, g_ext_dirbuf)) return -1;

    uint32_t pos = 0;
    uint32_t prev_pos = 0;
    int      have_prev = 0;

    while (pos + 8 <= ctx->block_size) {
      uint8_t* de = g_ext_dirbuf + pos;
      uint16_t rlen = erd16(de + 4);

      if (rlen < 8 || pos + rlen > ctx->block_size) break;

      if (erd32(de) != 0 && de[6] == nl && ix_memcmp(de + 8, name, nl) == 0) {
        if (have_prev) {
          uint8_t* pde = g_ext_dirbuf + prev_pos;
          ewr16(pde + 4, (uint16_t)(erd16(pde + 4) + rlen));
        } else {
          ewr32(de, 0);
          de[6] = 0;
          de[7] = 0;
        }

        return ext_write_block(ctx, phys, g_ext_dirbuf) ? 1 : -1;
      }

      prev_pos = pos;
      have_prev = 1;
      pos += rlen;
    }
  }

  return 0;
}

int ext_truncate_path(ext_context_t* ctx, const char* path) {
  if (!ctx || !ctx->valid || !path) return -1;

  uint32_t ino_num;
  if (!ext_lookup_path(ctx, path, &ino_num)) return -1;

  ext_inode_t ino;
  if (!ext_read_inode(ctx, ino_num, &ino)) return -1;

  uint16_t mode = erd16((const uint8_t*)&ino.i_mode);
  if ((mode & 0xF000u) == 0x4000u) return -1;

  int is_extent = (erd32((const uint8_t*)&ino.i_flags) & EXT4_EXTENTS_FL) ? 1 : 0;

  if (is_extent) {
    /* Рекурсивно освободить все листья. */
    for (int depth_guard = 0; depth_guard < 6; depth_guard++) {
      uint64_t leaf_block = 0;
      int last = ext_find_last_extent(ctx, (const uint8_t*)ino.i_block, &leaf_block);
      if (last < 0) break;

      const ext4_extent_t* ext;
      uint8_t* leaf_ptr = 0;

      if (leaf_block == 0) {
        ext = (const ext4_extent_t*)((const uint8_t*)ino.i_block + 12);
      } else {
        if (!ext_read_block(ctx, leaf_block, g_ext_ind2buf)) break;
        leaf_ptr = g_ext_ind2buf;
        ext = (const ext4_extent_t*)(leaf_ptr + 12);
      }

      uint16_t ee_len = erd16((const uint8_t*)&ext[last].ee_len) & 0x7FFFu;
      uint64_t start  = (uint64_t)erd32((const uint8_t*)&ext[last].ee_start_lo) |
                        ((uint64_t)erd16((const uint8_t*)&ext[last].ee_start_hi) << 32);

      for (uint16_t b = 0; b < ee_len; b++)
        ext_bfree(ctx, (uint32_t)(start + b));

      /* Убрать запись. */
      uint16_t eh_entries;
      if (leaf_block == 0) {
        eh_entries = erd16((const uint8_t*)ino.i_block + 2);
        ewr16((uint8_t*)ino.i_block + 2, (uint16_t)(eh_entries - 1u));
      } else {
        eh_entries = erd16(leaf_ptr + 2);
        ewr16(leaf_ptr + 2, (uint16_t)(eh_entries - 1u));
        ext_write_block(ctx, leaf_block, leaf_ptr);
      }
    }
  } else {
    /* Данные через resolve + освобождение метаданных. */
    uint64_t fsize = ext_inode_size(&ino);
    uint64_t nblocks = (fsize + ctx->block_size - 1) / ctx->block_size;

    for (uint64_t lb = 0; lb < nblocks; lb++) {
      uint64_t phys = ext_resolve_block(ctx, &ino, lb);
      if (phys) ext_bfree(ctx, (uint32_t)phys);
    }

    uint32_t ind  = erd32((const uint8_t*)&ino.i_block[12]);
    uint32_t dind = erd32((const uint8_t*)&ino.i_block[13]);

    if (ind)  { ext_free_ind_tree_level(ctx, ind, 1);  ext_bfree(ctx, ind); }
    if (dind) { ext_free_ind_tree_level(ctx, dind, 2); ext_bfree(ctx, dind); }
    /* Triple-indirect при записи не создавался -- не освобождаем. */
  }

  ix_memset((uint8_t*)ino.i_block, 0, sizeof(ino.i_block));
  ewr32((uint8_t*)&ino.i_size, 0);
  ewr32((uint8_t*)&ino.i_size_high, 0);
  ewr32((uint8_t*)&ino.i_blocks, 0);

  return ext_write_inode(ctx, ino_num, &ino) ? 0 : -1;
}


/*
 * Удалить файл.  Сначала освобождаются блоки данных, затем запись
 * каталога, и только потом сам inode: если система упадёт посередине,
 * файл останется видимым, а не превратится в висячую ссылку на
 * освобождённый inode.
 */
int ext_unlink(ext_context_t* ctx, const char* path) {
  char parent[EXT_NAME_MAX];
  const char* leaf;
  int plen = 0, last_slash = -1;

  if (!ctx || !ctx->valid || !path || !path[0]) return -1;

  while (path[plen]) {
    if (path[plen] == '/' || path[plen] == 92) last_slash = plen;
    plen++;
  }
  leaf = (last_slash >= 0) ? path + last_slash + 1 : path;
  if (!leaf[0]) return -1;

  if (last_slash >= 0) {
    int n = last_slash;
    if (n >= (int)sizeof(parent)) n = sizeof(parent) - 1;
    ix_memcpy(parent, path, n);
    parent[n] = 0;
  } else {
    parent[0] = 0;
  }

  uint32_t dir_ino = EXT_ROOT_INO;
  if (parent[0] && !(parent[0] == '/' && !parent[1]))
    if (!ext_lookup_path(ctx, parent, &dir_ino)) return -1;

  uint32_t target = ext_lookup_in_dir(ctx, dir_ino, leaf);
  if (target == 0) return -1;

  ext_inode_t ino;
  if (!ext_read_inode(ctx, target, &ino)) return -1;

  uint16_t mode = erd16((const uint8_t*)&ino.i_mode);
  if ((mode & 0xF000u) == 0x4000u) return -1;

  uint64_t fsize = ext_inode_size(&ino);
  uint64_t nblocks = (fsize + ctx->block_size - 1) / ctx->block_size;
  int is_extent = (erd32((const uint8_t*)&ino.i_flags) & EXT4_EXTENTS_FL) ? 1 : 0;

  if (is_extent) {
    for (int guard = 0; guard < 8; guard++) {
      uint64_t leaf_block = 0;
      const uint8_t* base;
      uint8_t* leaf_ptr = 0;

      if (ext_find_last_extent(ctx, (const uint8_t*)ino.i_block, &leaf_block) < 0)
        break;

      if (leaf_block == 0) {
        base = (const uint8_t*)ino.i_block + 12;
      } else {
        if (!ext_read_block(ctx, leaf_block, g_ext_ind2buf)) break;
        leaf_ptr = g_ext_ind2buf;
        base = leaf_ptr + 12;
      }

      {
        uint16_t ee_len = erd16(base) & 0x7FFFu;
        uint64_t start = (uint64_t)erd32(base + 4) |
                         ((uint64_t)erd16(base + 8) << 32);

        for (uint16_t b = 0; b < ee_len; b++)
          ext_bfree(ctx, (uint32_t)(start + b));
      }

      if (leaf_block == 0) {
        ewr16((uint8_t*)ino.i_block + 2,
              (uint16_t)(erd16((uint8_t*)ino.i_block + 2) - 1u));
      } else {
        ewr16(leaf_ptr + 2, (uint16_t)(erd16(leaf_ptr + 2) - 1u));
        ext_write_block(ctx, leaf_block, leaf_ptr);
      }
    }
  } else {
    for (uint64_t lb = 0; lb < nblocks; lb++) {
      uint64_t phys = ext_resolve_block(ctx, &ino, lb);
      if (phys) ext_bfree(ctx, (uint32_t)phys);
    }

    {
      uint32_t ind  = erd32((const uint8_t*)&ino.i_block[12]);
      uint32_t dind = erd32((const uint8_t*)&ino.i_block[13]);

      if (ind)  { ext_free_ind_tree_level(ctx, ind, 1);  ext_bfree(ctx, ind); }
      if (dind) { ext_free_ind_tree_level(ctx, dind, 2); ext_bfree(ctx, dind); }
    }
  }

  if (ext_remove_dir_entry(ctx, dir_ino, leaf) <= 0) return -1;
  if (!ext_ifree(ctx, target)) return -1;

  ix_memset((uint8_t*)&ino, 0, sizeof(ino));
  ext_write_inode(ctx, target, &ino);
  ext_rc_invalidate();

  return 0;
}

/*
 * Переименовать файл.  Запись каталога в ext -- это ссылка на inode,
 * поэтому переносится только номер inode: создать запись с новым
 * именем, выкинуть её пустой inode, убрать старую запись.
 * links_count не меняется -- файл всё время существовал ровно в одном
 * каталоге.
 */
int ext_rename(ext_context_t* ctx, const char* from, const char* to) {
  char parent[EXT_NAME_MAX];
  const char* leaf;
  int plen;
  int last_slash;
  uint32_t ino_num;
  uint32_t dir_ino;
  ext_inode_t ino;

  if (!ctx || !ctx->valid || !from || !to || !from[0] || !to[0]) return -1;

  if (!ext_lookup_path(ctx, from, &ino_num)) return -1;
  if (!ext_read_inode(ctx, ino_num, &ino)) return -1;

  {
    uint16_t mode = erd16((const uint8_t*)&ino.i_mode);
    if ((mode & 0xF000u) == 0x4000u) return -1;
  }

  {
    uint32_t existing = 0;
    if (ext_lookup_path(ctx, to, &existing) && existing) return -1;
  }

  if (ext_create(ctx, to) != 0) return -1;

  {
    uint32_t replacement = 0;
    if (!ext_lookup_path(ctx, to, &replacement)) {
      ext_unlink(ctx, to);
      return -1;
    }

    if (!ext_ifree(ctx, replacement)) {
      ext_unlink(ctx, to);
      return -1;
    }
  }

  plen = 0;
  last_slash = -1;
  while (to[plen]) {
    if (to[plen] == '/' || to[plen] == 92) last_slash = plen;
    plen++;
  }
  leaf = (last_slash >= 0) ? to + last_slash + 1 : to;

  if (last_slash >= 0) {
    int n = last_slash;
    if (n >= (int)sizeof(parent)) n = sizeof(parent) - 1;
    ix_memcpy(parent, to, n);
    parent[n] = 0;
  } else {
    parent[0] = 0;
  }

  dir_ino = EXT_ROOT_INO;
  if (parent[0] && !(parent[0] == '/' && !parent[1]))
    if (!ext_lookup_path(ctx, parent, &dir_ino)) return -1;

  if (ext_remove_dir_entry(ctx, dir_ino, leaf) != 1) return -1;
  if (ext_add_dir_entry(ctx, dir_ino, leaf, ino_num, EXT_FT_REG_FILE) != 0)
    return -1;

  plen = 0;
  last_slash = -1;
  while (from[plen]) {
    if (from[plen] == '/' || from[plen] == 92) last_slash = plen;
    plen++;
  }
  leaf = (last_slash >= 0) ? from + last_slash + 1 : from;

  if (last_slash >= 0) {
    int n = last_slash;
    if (n >= (int)sizeof(parent)) n = sizeof(parent) - 1;
    ix_memcpy(parent, from, n);
    parent[n] = 0;
  } else {
    parent[0] = 0;
  }

  dir_ino = EXT_ROOT_INO;
  if (parent[0] && !(parent[0] == '/' && !parent[1]))
    if (!ext_lookup_path(ctx, parent, &dir_ino)) return -1;

  if (ext_remove_dir_entry(ctx, dir_ino, leaf) != 1) return -1;

  ext_rc_invalidate();
  return 0;
}
