#include "ix_fs_fat.h"
#include "kernel.h"
#include "ixk_string.h"
#include "ix_utf8.h"
#include "ixk_video.h"

#define FAT_EOC12  0x0FF8u
#define FAT_EOC16  0xFFF8u
#define FAT_EOC32  0x0FFFFFF8u
#define FAT_FREE   0x00000000u

typedef struct {
  int      active;
  int      is_dir;
  uint32_t start_cluster;
  uint32_t file_size;
  uint32_t cur_cluster;
  uint32_t cur_offset;
  uint32_t dir_sector;
  uint32_t dir_entry_idx;
} fat_handle_t;

static fat_handle_t g_fat_handles[FAT_MAX_HANDLES];
static uint8_t g_fat_secbuf[FAT_SECTOR_SIZE];
static uint8_t g_fat_clusterbuf[FAT_CLUSTER_BUF];
static uint8_t g_fat_dirbuf[FAT_DIR_BUF_SECTORS * FAT_SECTOR_SIZE];

static uint16_t frd16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t frd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int fat_alloc_handle(void) {
  for (int i = 0; i < FAT_MAX_HANDLES; i++) {
    if (!g_fat_handles[i].active) return i;
  }
  return -1;
}

/*
 * "Сектор ФС" -- это BPB.bytes_per_sector (512..4096), а ixk_storage_*
 * адресуется 512-байтными секторами.  На 4Kn-томах (ESP на 4K-NVMe,
 * 4Kn USB) без пересчёта драйвер читал и ПИСАЛ не те сектора.
 */
static uint32_t fat_ss_mul(const fat_context_t* ctx) {
  uint32_t k = ctx->bytes_per_sector / 512u;
  return k ? k : 1u;
}

static int fat_read_sector(fat_context_t* ctx, uint64_t sector_offset, uint32_t count, void* buf) {
  uint32_t k = fat_ss_mul(ctx);
  return ixk_storage_read(ctx->part_lba + sector_offset * k, count * k, buf);
}

int fat_mount(fat_context_t* ctx, uint64_t part_lba, uint64_t part_sectors, ix_fs_type_t type) {
  ix_memset(ctx, 0, sizeof(fat_context_t));
  ix_memset(g_fat_handles, 0, sizeof(g_fat_handles));

  if (!ixk_storage_read(part_lba, 1, g_fat_secbuf))
    return 0;

  const fat_bpb_t* bpb = (const fat_bpb_t*)g_fat_secbuf;
  ctx->bytes_per_sector = frd16((const uint8_t*)&bpb->bytes_per_sector);
  ctx->sectors_per_cluster = bpb->sectors_per_cluster;
  ctx->reserved_sectors = frd16((const uint8_t*)&bpb->reserved_sectors);
  ctx->num_fats = bpb->num_fats;
  ctx->root_entry_count = frd16((const uint8_t*)&bpb->root_entry_count);

  uint16_t fat_sz16 = frd16((const uint8_t*)&bpb->fat_size_16);
  uint32_t fat_sz32 = frd32((const uint8_t*)&bpb->ext.fat32.fat_size_32);
  ctx->fat_size = fat_sz16 ? (uint32_t)fat_sz16 : fat_sz32;

  uint16_t tot16 = frd16((const uint8_t*)&bpb->total_sectors_16);
  uint32_t tot32 = frd32((const uint8_t*)&bpb->total_sectors_32);
  uint32_t total_sec = tot16 ? (uint32_t)tot16 : tot32;

  if (ctx->bytes_per_sector == 0 || ctx->sectors_per_cluster == 0 || ctx->fat_size == 0)
    return 0;
  if (ctx->bytes_per_sector != 512 && ctx->bytes_per_sector != 1024 &&
      ctx->bytes_per_sector != 2048 && ctx->bytes_per_sector != 4096)
    return 0;

  ctx->cluster_size = (uint32_t)ctx->bytes_per_sector * (uint32_t)ctx->sectors_per_cluster;
  ctx->root_dir_sectors = (((uint32_t)ctx->root_entry_count * 32u) + (ctx->bytes_per_sector - 1u)) / ctx->bytes_per_sector;
  ctx->first_data_sector = ctx->reserved_sectors + ((uint32_t)ctx->num_fats * ctx->fat_size) + ctx->root_dir_sectors;

  if (total_sec > ctx->first_data_sector)
    ctx->total_data_clusters = (total_sec - ctx->first_data_sector) / (uint32_t)ctx->sectors_per_cluster;
  else
    ctx->total_data_clusters = 0;

  ctx->fat_start_lba = ctx->reserved_sectors;
  ctx->root_dir_lba = ctx->reserved_sectors + ((uint32_t)ctx->num_fats * ctx->fat_size);
  ctx->data_start_lba = ctx->first_data_sector;

  if (type == IX_FS_TYPE_FAT32)
    ctx->root_cluster = frd32((const uint8_t*)&bpb->ext.fat32.root_cluster);
  else
    ctx->root_cluster = 0;

  ctx->fat_type = type;
  ctx->part_lba = part_lba;
  ctx->part_sectors = part_sectors;
  ctx->valid = 1;
  return 1;
}

static uint32_t fat_next_cluster(fat_context_t* ctx, uint32_t cluster) {
  if (ctx->fat_type == IX_FS_TYPE_FAT12) {
    uint32_t fat_offset = cluster + (cluster / 2);
    uint32_t fat_sector = (uint32_t)(ctx->fat_start_lba + (fat_offset / ctx->bytes_per_sector));
    uint32_t ent_offset = fat_offset % ctx->bytes_per_sector;

    if (!fat_read_sector(ctx, fat_sector, 1, g_fat_secbuf))
      return 0x0FFFu;

    uint16_t val;
    if (ent_offset == ctx->bytes_per_sector - 1u) {
      uint8_t lo = g_fat_secbuf[ent_offset];
      uint8_t hi;
      if (!fat_read_sector(ctx, fat_sector + 1, 1, g_fat_secbuf))
        return 0x0FFFu;
      hi = g_fat_secbuf[0];
      val = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    } else {
      val = frd16(g_fat_secbuf + ent_offset);
    }

    if (cluster & 1u)
      val >>= 4;
    else
      val &= 0x0FFFu;

    return (val >= 0x0FF8u) ? 0x0FFFFFFFu : (uint32_t)val;
  }

  if (ctx->fat_type == IX_FS_TYPE_FAT16) {
    uint32_t fat_offset = cluster * 2u;
    uint32_t fat_sector = (uint32_t)(ctx->fat_start_lba + (fat_offset / ctx->bytes_per_sector));
    uint32_t ent_offset = fat_offset % ctx->bytes_per_sector;

    if (!fat_read_sector(ctx, fat_sector, 1, g_fat_secbuf))
      return 0x0000FFFFu;

    uint16_t val = frd16(g_fat_secbuf + ent_offset);
    return (val >= FAT_EOC16) ? 0x0FFFFFFFu : (uint32_t)val;
  }

  {
    uint32_t fat_offset = cluster * 4u;
    uint32_t fat_sector = (uint32_t)(ctx->fat_start_lba + (fat_offset / ctx->bytes_per_sector));
    uint32_t ent_offset = fat_offset % ctx->bytes_per_sector;

    if (!fat_read_sector(ctx, fat_sector, 1, g_fat_secbuf))
      return 0x0FFFFFFFu;

    uint32_t val = frd32(g_fat_secbuf + ent_offset) & 0x0FFFFFFFu;
    return (val >= FAT_EOC32) ? 0x0FFFFFFFu : val;
  }
}

static int fat_is_eoc(fat_context_t* ctx, uint32_t cluster) {
  (void)ctx;
  return cluster >= 0x0FFFFFF8u || cluster < 2u;
}

static uint64_t fat_cluster_to_lba(fat_context_t* ctx, uint32_t cluster) {
  return ctx->data_start_lba + (uint64_t)(cluster - 2u) * (uint64_t)ctx->sectors_per_cluster;
}

static int fat_read_cluster(fat_context_t* ctx, uint32_t cluster, void* buf) {
  uint64_t lba = fat_cluster_to_lba(ctx, cluster);
  return fat_read_sector(ctx, lba, ctx->sectors_per_cluster, buf);
}

static void fat_sfn_to_name(const uint8_t* sfn, char* out) {
  int i, j = 0;
  for (i = 0; i < 8 && sfn[i] != ' '; i++) {
    char c = (char)sfn[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    out[j++] = c;
  }
  if (sfn[8] != ' ') {
    out[j++] = '.';
    for (i = 8; i < 11 && sfn[i] != ' '; i++) {
      char c = (char)sfn[i];
      if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
      out[j++] = c;
    }
  }
  out[j] = 0;
}

static void fat_collect_lfn(const fat_lfn_entry_t* entries, int count, char* out, int cap) {
  /* Собрать UTF-16 по порядку, затем перекодировать в UTF-8. */
  uint16_t units[FAT_NAME_MAX];
  int n = 0;
  for (int i = count - 1; i >= 0 && n < FAT_NAME_MAX - 1; i--) {
    const fat_lfn_entry_t* e = &entries[i];
    for (int j = 0; j < 5 && n < FAT_NAME_MAX - 1; j++) {
      uint16_t ch = e->name1[j];
      if (ch == 0 || ch == 0xFFFFu) goto done;
      units[n++] = ch;
    }
    for (int j = 0; j < 6 && n < FAT_NAME_MAX - 1; j++) {
      uint16_t ch = e->name2[j];
      if (ch == 0 || ch == 0xFFFFu) goto done;
      units[n++] = ch;
    }
    for (int j = 0; j < 2 && n < FAT_NAME_MAX - 1; j++) {
      uint16_t ch = e->name3[j];
      if (ch == 0 || ch == 0xFFFFu) goto done;
      units[n++] = ch;
    }
  }
done:
  ix_utf16_to_utf8(units, n, out, cap);
}

static int fat_strcmp_ci(const char* a, const char* b) {
  return ix_utf8_casecmp(a, b);
}

typedef struct {
  uint32_t cluster;
  uint32_t size;
  uint8_t  attr;
  uint16_t wrt_time;
  uint16_t wrt_date;
  uint16_t crt_time;
  uint16_t crt_date;
  char     name[FAT_NAME_MAX];
  int      found;
} fat_lookup_result_t;

/*
 * Сканирование каталога на предмет записи.
 *
 * out_lba/out_idx (могут быть 0) получают расположение КОРОТКОЙ
 * записи в каталоге -- нужно для обновления size/first cluster
 * при записи в файл (fat_update_dirent).
 */
static int fat_scan_dir_root16(fat_context_t* ctx, const char* target_name,
                               fat_lookup_result_t* result,
                               uint64_t* out_lba, uint32_t* out_idx) {
  fat_lfn_entry_t lfn_buf[20];
  int lfn_count = 0;

  uint32_t entries = ctx->root_entry_count;
  uint32_t sectors = ctx->root_dir_sectors;

  for (uint32_t s = 0; s < sectors; s++) {
    if (!fat_read_sector(ctx, ctx->root_dir_lba + s, 1, g_fat_secbuf))
      return 0;

    uint32_t ent_per_sec = ctx->bytes_per_sector / 32u;
    for (uint32_t e = 0; e < ent_per_sec && (s * ent_per_sec + e) < entries; e++) {
      const fat_dir_entry_t* de = (const fat_dir_entry_t*)(g_fat_secbuf + e * 32u);

      if (de->name[0] == 0x00) return 0;
      if (de->name[0] == 0xE5) { lfn_count = 0; continue; }

      if (de->attr == FAT_ATTR_LONG_NAME) {
        const fat_lfn_entry_t* le = (const fat_lfn_entry_t*)de;
        int ord = le->order & 0x3Fu;
        if (le->order & 0x40u) lfn_count = 0;
        if (ord > 0 && ord <= 20 && lfn_count < 20)
          lfn_buf[lfn_count++] = *le;
        continue;
      }

      if (de->attr & FAT_ATTR_VOLUME_ID) { lfn_count = 0; continue; }

      char name[FAT_NAME_MAX];
      if (lfn_count > 0) {
        fat_collect_lfn(lfn_buf, lfn_count, name, FAT_NAME_MAX);
      } else {
        fat_sfn_to_name(de->name, name);
      }
      lfn_count = 0;

      if (!target_name) {
        result->cluster = ((uint32_t)frd16((const uint8_t*)&de->fst_clus_hi) << 16) | (uint32_t)frd16((const uint8_t*)&de->fst_clus_lo);
        result->size = frd32((const uint8_t*)&de->file_size);
        result->attr = de->attr;
        result->wrt_time = frd16((const uint8_t*)&de->wrt_time);
        result->wrt_date = frd16((const uint8_t*)&de->wrt_date);
        result->crt_time = frd16((const uint8_t*)&de->crt_time);
        result->crt_date = frd16((const uint8_t*)&de->crt_date);
        ixk_string_copy_n(result->name, name, FAT_NAME_MAX);
        result->found = 1;
        if (out_lba) *out_lba = ctx->root_dir_lba + s;
        if (out_idx) *out_idx = e;
        return 1;
      }

      if (fat_strcmp_ci(name, target_name) == 0) {
        result->cluster = ((uint32_t)frd16((const uint8_t*)&de->fst_clus_hi) << 16) | (uint32_t)frd16((const uint8_t*)&de->fst_clus_lo);
        result->size = frd32((const uint8_t*)&de->file_size);
        result->attr = de->attr;
        result->wrt_time = frd16((const uint8_t*)&de->wrt_time);
        result->wrt_date = frd16((const uint8_t*)&de->wrt_date);
        result->crt_time = frd16((const uint8_t*)&de->crt_time);
        result->crt_date = frd16((const uint8_t*)&de->crt_date);
        ixk_string_copy_n(result->name, name, FAT_NAME_MAX);
        result->found = 1;
        if (out_lba) *out_lba = ctx->root_dir_lba + s;
        if (out_idx) *out_idx = e;
        return 1;
      }
    }
  }
  return 0;
}

static int fat_scan_dir_cluster(fat_context_t* ctx, uint32_t dir_cluster,
                                const char* target_name, fat_lookup_result_t* result,
                                uint64_t* out_lba, uint32_t* out_idx) {
  fat_lfn_entry_t lfn_buf[20];
  int lfn_count = 0;
  uint32_t cluster = dir_cluster;
  uint32_t max_iter = 1000000u;

  while (!fat_is_eoc(ctx, cluster) && max_iter > 0) {
    max_iter--;
    uint32_t spc = ctx->sectors_per_cluster;
    uint64_t lba = fat_cluster_to_lba(ctx, cluster);

    for (uint32_t s = 0; s < spc; s++) {
      if (!fat_read_sector(ctx, lba + s, 1, g_fat_secbuf))
        return 0;

      uint32_t ent_per_sec = ctx->bytes_per_sector / 32u;
      for (uint32_t e = 0; e < ent_per_sec; e++) {
        const fat_dir_entry_t* de = (const fat_dir_entry_t*)(g_fat_secbuf + e * 32u);

        if (de->name[0] == 0x00) return 0;
        if (de->name[0] == 0xE5) { lfn_count = 0; continue; }

        if (de->attr == FAT_ATTR_LONG_NAME) {
          const fat_lfn_entry_t* le = (const fat_lfn_entry_t*)de;
          int ord = le->order & 0x3Fu;
          if (le->order & 0x40u) lfn_count = 0;
          if (ord > 0 && ord <= 20 && lfn_count < 20)
            lfn_buf[lfn_count++] = *le;
          continue;
        }

        if (de->attr & FAT_ATTR_VOLUME_ID) { lfn_count = 0; continue; }

        char name[FAT_NAME_MAX];
        if (lfn_count > 0) {
          fat_collect_lfn(lfn_buf, lfn_count, name, FAT_NAME_MAX);
        } else {
          fat_sfn_to_name(de->name, name);
        }
        lfn_count = 0;

        if (!target_name) {
          result->cluster = ((uint32_t)frd16((const uint8_t*)&de->fst_clus_hi) << 16) | (uint32_t)frd16((const uint8_t*)&de->fst_clus_lo);
          result->size = frd32((const uint8_t*)&de->file_size);
          result->attr = de->attr;
          result->wrt_time = frd16((const uint8_t*)&de->wrt_time);
          result->wrt_date = frd16((const uint8_t*)&de->wrt_date);
          result->crt_time = frd16((const uint8_t*)&de->crt_time);
          result->crt_date = frd16((const uint8_t*)&de->crt_date);
          ixk_string_copy_n(result->name, name, FAT_NAME_MAX);
          result->found = 1;
          if (out_lba) *out_lba = fat_cluster_to_lba(ctx, cluster) + s;
          if (out_idx) *out_idx = e;
          return 1;
        }

        if (fat_strcmp_ci(name, target_name) == 0) {
          result->cluster = ((uint32_t)frd16((const uint8_t*)&de->fst_clus_hi) << 16) | (uint32_t)frd16((const uint8_t*)&de->fst_clus_lo);
          result->size = frd32((const uint8_t*)&de->file_size);
          result->attr = de->attr;
          result->wrt_time = frd16((const uint8_t*)&de->wrt_time);
          result->wrt_date = frd16((const uint8_t*)&de->wrt_date);
          result->crt_time = frd16((const uint8_t*)&de->crt_time);
          result->crt_date = frd16((const uint8_t*)&de->crt_date);
          ixk_string_copy_n(result->name, name, FAT_NAME_MAX);
          result->found = 1;
          if (out_lba) *out_lba = fat_cluster_to_lba(ctx, cluster) + s;
          if (out_idx) *out_idx = e;
          return 1;
        }
      }
    }
    cluster = fat_next_cluster(ctx, cluster);
  }
  return 0;
}

static int fat_lookup_path(fat_context_t* ctx, const char* path, fat_lookup_result_t* result) {
  ix_memset(result, 0, sizeof(*result));

  if (!path || !path[0] || (path[0] == '/' && !path[1])) {
    if (ctx->fat_type == IX_FS_TYPE_FAT32 || ctx->fat_type == IX_FS_TYPE_FAT12) {
      result->cluster = ctx->root_cluster;
    } else {
      result->cluster = 0;
    }
    result->attr = FAT_ATTR_DIRECTORY;
    result->found = 1;
    result->name[0] = 0;
    return 1;
  }

  char component[FAT_NAME_MAX];
  int pi = 0;
  uint32_t cur_cluster;
  int is_root;

  if (ctx->fat_type == IX_FS_TYPE_FAT32) {
    cur_cluster = ctx->root_cluster;
    is_root = 0;
  } else {
    cur_cluster = 0;
    is_root = 1;
  }

  while (path[pi]) {
    if (path[pi] == '/' || path[pi] == '\\') { pi++; continue; }

    int ci = 0;
    while (path[pi] && path[pi] != '/' && path[pi] != '\\' && ci < FAT_NAME_MAX - 1)
      component[ci++] = path[pi++];
    component[ci] = 0;

    if (!component[0]) continue;

    fat_lookup_result_t lr;
    ix_memset(&lr, 0, sizeof(lr));

    if (is_root && ctx->fat_type != IX_FS_TYPE_FAT32) {
      if (!fat_scan_dir_root16(ctx, component, &lr, 0, 0)) return 0;
    } else {
      if (!fat_scan_dir_cluster(ctx, cur_cluster, component, &lr, 0, 0)) return 0;
    }

    if (!lr.found) return 0;

    if (path[pi] && (path[pi] == '/' || path[pi] == '\\') && path[pi + 1]) {
      if (!(lr.attr & FAT_ATTR_DIRECTORY)) return 0;
      cur_cluster = lr.cluster;
      is_root = 0;
    } else {
      *result = lr;
      return 1;
    }
  }

  result->cluster = cur_cluster;
  result->attr = FAT_ATTR_DIRECTORY;
  result->found = 1;
  return 1;
}

static uint64_t fat_dostime_to_ns(uint16_t date, uint16_t time_val) {
  uint32_t sec = (time_val & 0x1Fu) * 2u;
  uint32_t min = (time_val >> 5) & 0x3Fu;
  uint32_t hour = (time_val >> 11) & 0x1Fu;
  uint32_t day = date & 0x1Fu;
  uint32_t month = (date >> 5) & 0x0Fu;
  uint32_t year = ((date >> 9) & 0x7Fu) + 1980u;

  uint64_t days = 0;
  for (uint32_t y = 1970; y < year; y++) {
    int leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
    days += leap ? 366u : 365u;
  }
  static const uint16_t mdays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
  if (month >= 1 && month <= 12)
    days += mdays[month - 1];
  int ly = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
  if (ly && month > 2) days++;
  if (day > 0) days += day - 1u;

  uint64_t total_sec = days * 86400u + (uint64_t)hour * 3600u + (uint64_t)min * 60u + sec;
  return total_sec * 1000000000ull;
}

int fat_open(fat_context_t* ctx, const char* path, uint32_t flags) {
  (void)flags;
  if (!ctx || !ctx->valid) return -1;

  fat_lookup_result_t lr;
  if (!fat_lookup_path(ctx, path, &lr)) return -1;
  if (!lr.found) return -1;
  if (lr.attr & FAT_ATTR_DIRECTORY) return -1;

  int h = fat_alloc_handle();
  if (h < 0) return -1;

  g_fat_handles[h].active = 1;
  g_fat_handles[h].is_dir = 0;
  g_fat_handles[h].start_cluster = lr.cluster;
  g_fat_handles[h].file_size = lr.size;
  g_fat_handles[h].cur_cluster = lr.cluster;
  g_fat_handles[h].cur_offset = 0;
  return h;
}

int fat_read(fat_context_t* ctx, int handle, void* buffer, uint32_t size) {
  if (handle < 0 || handle >= FAT_MAX_HANDLES) return -1;
  fat_handle_t* fh = &g_fat_handles[handle];
  if (!fh->active || fh->is_dir) return -1;
  if (!ctx || !ctx->valid) return -1;

  uint8_t* dst = (uint8_t*)buffer;
  uint32_t remaining = size;
  if (fh->cur_offset + remaining > fh->file_size)
    remaining = fh->file_size - fh->cur_offset;

  uint32_t total_read = 0;

  while (remaining > 0 && !fat_is_eoc(ctx, fh->cur_cluster)) {
    uint32_t cluster_off = fh->cur_offset % ctx->cluster_size;
    uint32_t sector_in_cluster = cluster_off / ctx->bytes_per_sector;
    uint32_t byte_in_sector = cluster_off % ctx->bytes_per_sector;

    uint64_t lba = fat_cluster_to_lba(ctx, fh->cur_cluster) + sector_in_cluster;
    if (!fat_read_sector(ctx, lba, 1, g_fat_secbuf))
      break;

    uint32_t avail = ctx->bytes_per_sector - byte_in_sector;
    if (avail > remaining) avail = remaining;

    ix_memcpy(dst + total_read, g_fat_secbuf + byte_in_sector, avail);
    total_read += avail;
    remaining -= avail;
    fh->cur_offset += avail;

    if (fh->cur_offset % ctx->cluster_size == 0 && remaining > 0) {
      fh->cur_cluster = fat_next_cluster(ctx, fh->cur_cluster);
    }
  }

  return (int)total_read;
}

int fat_stat(fat_context_t* ctx, const char* path, ix_file_info_t* out) {
  if (!ctx || !ctx->valid || !out) return -1;
  ix_memset(out, 0, sizeof(*out));

  fat_lookup_result_t lr;
  if (!fat_lookup_path(ctx, path, &lr)) return -1;
  if (!lr.found) return -1;

  out->size = (lr.attr & FAT_ATTR_DIRECTORY) ? 0 : (uint64_t)lr.size;
  out->attrs = 0;
  if (lr.attr & FAT_ATTR_DIRECTORY) out->attrs |= IX_FS_ATTR_DIR;
  if (lr.attr & FAT_ATTR_HIDDEN) out->attrs |= IX_FS_ATTR_HIDDEN;
  if (lr.attr & FAT_ATTR_SYSTEM) out->attrs |= IX_FS_ATTR_SYSTEM;
  if (lr.attr & FAT_ATTR_READ_ONLY) out->attrs |= IX_FS_ATTR_READONLY;

  out->modified_ns = fat_dostime_to_ns(lr.wrt_date, lr.wrt_time);
  out->created_ns = fat_dostime_to_ns(lr.crt_date, lr.crt_time);
  return 0;
}

int fat_close(fat_context_t* ctx, int handle) {
  (void)ctx;
  if (handle < 0 || handle >= FAT_MAX_HANDLES) return -1;
  g_fat_handles[handle].active = 0;
  return 0;
}

typedef struct {
  int      active;
  int      is_root16;
  uint32_t dir_cluster;
  uint32_t cur_cluster;
  uint32_t cur_sector;
  uint32_t cur_entry;
  uint32_t total_root_entries;
  uint32_t root_sector_idx;
  fat_lfn_entry_t lfn_buf[20];
  int      lfn_count;
} fat_dir_handle_t;

static fat_dir_handle_t g_fat_dirs[FAT_MAX_HANDLES];

int fat_opendir(fat_context_t* ctx, const char* path) {
  if (!ctx || !ctx->valid) return -1;

  fat_lookup_result_t lr;
  if (!fat_lookup_path(ctx, path, &lr)) return -1;
  if (!lr.found) return -1;
  if (!(lr.attr & FAT_ATTR_DIRECTORY) && path && path[0] && !(path[0] == '/' && !path[1])) return -1;

  int h = -1;
  for (int i = 0; i < FAT_MAX_HANDLES; i++) {
    if (!g_fat_dirs[i].active) { h = i; break; }
  }
  if (h < 0) return -1;

  ix_memset(&g_fat_dirs[h], 0, sizeof(fat_dir_handle_t));
  g_fat_dirs[h].active = 1;

  if (lr.cluster == 0 && ctx->fat_type != IX_FS_TYPE_FAT32) {
    g_fat_dirs[h].is_root16 = 1;
    g_fat_dirs[h].total_root_entries = ctx->root_entry_count;
    g_fat_dirs[h].root_sector_idx = 0;
    g_fat_dirs[h].cur_entry = 0;
  } else {
    g_fat_dirs[h].is_root16 = 0;
    g_fat_dirs[h].dir_cluster = lr.cluster;
    g_fat_dirs[h].cur_cluster = lr.cluster;
    g_fat_dirs[h].cur_sector = 0;
    g_fat_dirs[h].cur_entry = 0;
  }

  return h;
}

int fat_readdir(fat_context_t* ctx, int dir_handle, ix_dirent_t* out) {
  if (dir_handle < 0 || dir_handle >= FAT_MAX_HANDLES) return -1;
  fat_dir_handle_t* dh = &g_fat_dirs[dir_handle];
  if (!dh->active || !ctx || !ctx->valid) return -1;
  ix_memset(out, 0, sizeof(*out));

  /* Общий g_fat_dirbuf могли затереть другие операции между вызовами
   * (stat/lookup, чтение): сектор каталога всегда подгружаем заново. */
  uint64_t loaded_lba = (uint64_t)-1;

  for (;;) {
    const fat_dir_entry_t* de = 0;

    if (dh->is_root16) {
      uint32_t abs_entry = dh->cur_entry;
      if (abs_entry >= dh->total_root_entries) return 0;

      uint32_t ent_per_sec = ctx->bytes_per_sector / 32u;
      uint32_t sec_idx = abs_entry / ent_per_sec;
      uint32_t in_sec = abs_entry % ent_per_sec;

      if (ctx->root_dir_lba + sec_idx != loaded_lba) {
        if (!fat_read_sector(ctx, ctx->root_dir_lba + sec_idx, 1, g_fat_dirbuf))
          return -1;
        loaded_lba = ctx->root_dir_lba + sec_idx;
      }

      de = (const fat_dir_entry_t*)(g_fat_dirbuf + in_sec * 32u);
      dh->cur_entry++;
    } else {
      if (fat_is_eoc(ctx, dh->cur_cluster)) return 0;

      uint32_t ent_per_sec = ctx->bytes_per_sector / 32u;
      uint32_t in_sec = dh->cur_entry % ent_per_sec;

      {
        uint64_t lba = fat_cluster_to_lba(ctx, dh->cur_cluster) + dh->cur_sector;
        if (lba != loaded_lba) {
          if (!fat_read_sector(ctx, lba, 1, g_fat_dirbuf))
            return -1;
          loaded_lba = lba;
        }
      }

      de = (const fat_dir_entry_t*)(g_fat_dirbuf + in_sec * 32u);
      dh->cur_entry++;

      if (dh->cur_entry % ent_per_sec == 0) {
        dh->cur_sector++;
        if (dh->cur_sector >= ctx->sectors_per_cluster) {
          dh->cur_cluster = fat_next_cluster(ctx, dh->cur_cluster);
          dh->cur_sector = 0;
        }
      }
    }

    if (de->name[0] == 0x00) return 0;
    if (de->name[0] == 0xE5) { dh->lfn_count = 0; continue; }

    if (de->attr == FAT_ATTR_LONG_NAME) {
      const fat_lfn_entry_t* le = (const fat_lfn_entry_t*)de;
      if (le->order & 0x40u) dh->lfn_count = 0;
      int ord = le->order & 0x3Fu;
      if (ord > 0 && ord <= 20 && dh->lfn_count < 20)
        dh->lfn_buf[dh->lfn_count++] = *le;
      continue;
    }

    if (de->attr & FAT_ATTR_VOLUME_ID) { dh->lfn_count = 0; continue; }

    if (de->name[0] == '.' && de->name[1] == ' ') { dh->lfn_count = 0; continue; }
    if (de->name[0] == '.' && de->name[1] == '.' && de->name[2] == ' ') { dh->lfn_count = 0; continue; }

    char name[FAT_NAME_MAX];
    if (dh->lfn_count > 0) {
      fat_collect_lfn(dh->lfn_buf, dh->lfn_count, name, FAT_NAME_MAX);
    } else {
      fat_sfn_to_name(de->name, name);
    }
    dh->lfn_count = 0;

    ixk_string_copy_n(out->name, name, 256);
    out->is_dir = (de->attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
    out->size = frd32((const uint8_t*)&de->file_size);
    out->info.size = out->size;
    if (out->is_dir) out->info.attrs = IX_FS_ATTR_DIR;
    out->info.modified_ns = fat_dostime_to_ns(frd16((const uint8_t*)&de->wrt_date), frd16((const uint8_t*)&de->wrt_time));
    out->info.created_ns = fat_dostime_to_ns(frd16((const uint8_t*)&de->crt_date), frd16((const uint8_t*)&de->crt_time));
    return 1;
  }
}

int fat_closedir(fat_context_t* ctx, int dir_handle) {
  (void)ctx;
  if (dir_handle < 0 || dir_handle >= FAT_MAX_HANDLES) return -1;
  g_fat_dirs[dir_handle].active = 0;
  return 0;
}

/* =====================================================================
 *  WRITE-ПОДДЕРЖКА (ArtyomX64 fs-autodetect iteration)
 * =====================================================================
 *
 *  Модель -- stateless-функции над ПУТЕМ (без дескрипторов):
 *
 *    fat_read_at       -- чтение диапазона файла по абсолютному смещению;
 *    fat_write_at      -- запись с расширением цепочки кластеров;
 *    fat_create        -- создание файла (8.3 + LFN);
 *    fat_truncate_path -- обнуление файла с освобождением кластеров.
 *
 *  Запись идёт секторами по 512 байт (нет зависимости от размера
 *  кластера в буферах).  Обе копии FAT обновляются всегда.
 *  FSInfo (FAT32) не трогаем: next_free/fsck восстановит.
 * =================================================================== */

static void fat_rc_invalidate(void);

static int fat_write_sector(fat_context_t* ctx, uint64_t sector_offset, uint32_t count, const void* buf) {
  fat_rc_invalidate();               /* кэш чтения больше не актуален */
  uint32_t k = fat_ss_mul(ctx);
  return ixk_storage_write(ctx->part_lba + sector_offset * k, count * k, buf);
}

/* Байтовое смещение элемента FAT внутри ОДНОЙ копии FAT. */
static uint64_t fat_entry_byte_off(fat_context_t* ctx, uint32_t cluster) {
  if (ctx->fat_type == IX_FS_TYPE_FAT12)
    return (uint64_t)cluster + (cluster / 2u);
  if (ctx->fat_type == IX_FS_TYPE_FAT16)
    return (uint64_t)cluster * 2u;
  return (uint64_t)cluster * 4u;
}

/*
 * Записать элемент FAT для кластера во ВСЕ копии FAT.
 * FAT12-элемент может пересекать границу секторов -- RMW каждого
 * затронутого сектора отдельно.
 */
static int fat_set_fat_entry(fat_context_t* ctx, uint32_t cluster, uint32_t value) {
  if (cluster < 2u) return 0;

  for (uint8_t copy = 0; copy < ctx->num_fats; copy++) {
    uint64_t base = ctx->fat_start_lba + (uint64_t)copy * ctx->fat_size;

    if (ctx->fat_type == IX_FS_TYPE_FAT12) {
      uint64_t off     = fat_entry_byte_off(ctx, cluster);
      uint64_t sec     = base + off / ctx->bytes_per_sector;
      uint32_t ent     = (uint32_t)(off % ctx->bytes_per_sector);
      uint16_t newval;

      /* Значение 12 бит, выровненное по чётности кластера. */
      if (!fat_read_sector(ctx, sec, 1, g_fat_secbuf)) return 0;
      uint16_t cur = (uint16_t)(g_fat_secbuf[ent] |
                     ((ent + 1u < ctx->bytes_per_sector)
                        ? ((uint16_t)g_fat_secbuf[ent + 1u] << 8) : 0));
      if (cluster & 1u) newval = (uint16_t)((cur & 0x000Fu) | ((value & 0x0FFFu) << 4));
      else              newval = (uint16_t)((cur & 0xF000u) | (value & 0x0FFFu));
      g_fat_secbuf[ent] = (uint8_t)(newval & 0xFFu);
      if (ent + 1u < ctx->bytes_per_sector)
        g_fat_secbuf[ent + 1u] = (uint8_t)(newval >> 8);
      if (!fat_write_sector(ctx, sec, 1, g_fat_secbuf)) return 0;

      /* Старший байт уехал в следующий сектор -- отдельный RMW. */
      if (ent + 1u >= ctx->bytes_per_sector) {
        if (!fat_read_sector(ctx, sec + 1, 1, g_fat_secbuf)) return 0;
        g_fat_secbuf[0] = (uint8_t)(newval >> 8);
        if (!fat_write_sector(ctx, sec + 1, 1, g_fat_secbuf)) return 0;
      }
      continue;
    }

    uint64_t off = fat_entry_byte_off(ctx, cluster);
    uint64_t sec = base + off / ctx->bytes_per_sector;
    uint32_t ent = (uint32_t)(off % ctx->bytes_per_sector);

    if (!fat_read_sector(ctx, sec, 1, g_fat_secbuf)) return 0;

    if (ctx->fat_type == IX_FS_TYPE_FAT16) {
      g_fat_secbuf[ent]     = (uint8_t)(value & 0xFFu);
      g_fat_secbuf[ent + 1] = (uint8_t)((value >> 8) & 0xFFu);
    } else {
      /* FAT32: старшие 4 бита сохраняем (там биты 0xF8FFFFFF). */
      uint32_t old = frd32(g_fat_secbuf + ent);
      uint32_t new = (old & 0xF0000000u) | (value & 0x0FFFFFFFu);
      g_fat_secbuf[ent]     = (uint8_t)(new & 0xFFu);
      g_fat_secbuf[ent + 1] = (uint8_t)((new >> 8) & 0xFFu);
      g_fat_secbuf[ent + 2] = (uint8_t)((new >> 16) & 0xFFu);
      g_fat_secbuf[ent + 3] = (uint8_t)((new >> 24) & 0xFFu);
    }

    if (!fat_write_sector(ctx, sec, 1, g_fat_secbuf)) return 0;
  }

  return 1;
}

/* Кэш одного сектора FAT (только первая копия) для поиска свободных. */
static uint8_t  g_fat_alloc_cache[FAT_SECTOR_SIZE];
static uint32_t g_fat_alloc_cache_sec = 0xFFFFFFFFu;

static int fat_alloc_cache_get(fat_context_t* ctx, uint32_t rel_sec, uint8_t** out) {
  if (g_fat_alloc_cache_sec != rel_sec) {
    if (!fat_read_sector(ctx, ctx->fat_start_lba + rel_sec, 1, g_fat_alloc_cache))
      return 0;
    g_fat_alloc_cache_sec = rel_sec;
  }
  *out = g_fat_alloc_cache;
  return 1;
}

static void fat_alloc_cache_invalidate(void) {
  g_fat_alloc_cache_sec = 0xFFFFFFFFu;
}

/*
 * Найти свободный кластер, пометить его EOC.  Возвращает номер
 * кластера или 0 (нет места / ошибка).
 */
static uint32_t fat_alloc_cluster(fat_context_t* ctx) {
  if (ctx->total_data_clusters == 0) return 0;

  uint32_t max_cluster = ctx->total_data_clusters + 1u;  /* номера 2..max */

  for (uint32_t c = 2; c <= max_cluster; c++) {
    uint64_t off = fat_entry_byte_off(ctx, c);
    uint32_t rel_sec = (uint32_t)(off / ctx->bytes_per_sector);
    uint32_t ent     = (uint32_t)(off % ctx->bytes_per_sector);
    uint32_t val;

    /* FAT12 на границе сектора -- через обычный (некэшированный) путь. */
    if (ctx->fat_type == IX_FS_TYPE_FAT12 && ent >= ctx->bytes_per_sector - 1u) {
      val = fat_next_cluster(ctx, c);
      if (val == 0x0FFFFFFFu) val = 0xFFFFFFFFu;   /* EOC -> не свободен */
      if (val == FAT_FREE) {
        fat_alloc_cache_invalidate();
        if (!fat_set_fat_entry(ctx, c, 0x0FFFFFFFu)) return 0;
        return c;
      }
      continue;
    }

    uint8_t* buf;
    if (!fat_alloc_cache_get(ctx, rel_sec, &buf)) return 0;

    if (ctx->fat_type == IX_FS_TYPE_FAT12) {
      uint16_t v = frd16(buf + ent);
      if (c & 1u) v >>= 4; else v &= 0x0FFFu;
      val = (v == 0) ? FAT_FREE : 1u;
    } else if (ctx->fat_type == IX_FS_TYPE_FAT16) {
      val = (frd16(buf + ent) == 0) ? FAT_FREE : 1u;
    } else {
      val = ((frd32(buf + ent) & 0x0FFFFFFFu) == 0) ? FAT_FREE : 1u;
    }

    if (val == FAT_FREE) {
      fat_alloc_cache_invalidate();
      if (!fat_set_fat_entry(ctx, c,
                             ctx->fat_type == IX_FS_TYPE_FAT12 ? 0x0FFFu : 0x0FFFFFFFu))
        return 0;
      return c;
    }
  }

  return 0;
}

/* Обнулить все секторы кластера. */
static int fat_zero_cluster(fat_context_t* ctx, uint32_t cluster) {
  static uint8_t zeros[FAT_SECTOR_SIZE];

  uint64_t lba = fat_cluster_to_lba(ctx, cluster);
  for (uint32_t s = 0; s < ctx->sectors_per_cluster; s++) {
    if (!fat_write_sector(ctx, lba + s, 1, zeros)) return 0;
  }
  return 1;
}

/*
 * Stateless чтение: path -> цепочка кластеров -> секторы.
 * Возвращает число реально прочитанных байт (обрезка по EOF).
 */
/*
 * Кэш последовательного чтения.  fat_read_at() вызывается VFS кусками
 * (512..4096 байт); раньше КАЖДЫЙ вызов заново искал файл по пути и заново
 * проходил цепочку кластеров от начала файла -- чтение файла в N кусков
 * стоило O(N^2) обращений к FAT.  Теперь помним результат поиска и позицию
 * в цепочке (индекс кластера + номер кластера); любая запись сбрасывает кэш.
 */
#define FAT_RC_N        4
#define FAT_RC_PATH_MAX 256

typedef struct {
  int                 valid;
  const fat_context_t* ctx;
  char                path[FAT_RC_PATH_MAX];
  fat_lookup_result_t lr;
  uint32_t            idx;      /* номер кластера в цепочке (0 = первый) */
  uint32_t            cur;      /* номер кластера на позиции idx */
  uint32_t            stamp;
} fat_rc_t;

static fat_rc_t g_fat_rc[FAT_RC_N];
static uint32_t g_fat_rc_tick = 0;

static void fat_rc_invalidate(void) {
  for (int i = 0; i < FAT_RC_N; i++) g_fat_rc[i].valid = 0;
}

static fat_rc_t* fat_rc_get(fat_context_t* ctx, const char* path) {
  int plen = 0;
  while (path[plen]) plen++;
  if (plen >= FAT_RC_PATH_MAX) return 0;

  fat_rc_t* victim = &g_fat_rc[0];
  for (int i = 0; i < FAT_RC_N; i++) {
    fat_rc_t* e = &g_fat_rc[i];
    if (e->valid && e->ctx == ctx && ix_memcmp(e->path, path, (uint32_t)plen + 1u) == 0) {
      e->stamp = ++g_fat_rc_tick;
      return e;
    }
    if (!e->valid) victim = e;
    else if (victim->valid && e->stamp < victim->stamp) victim = e;
  }

  fat_lookup_result_t lr;
  if (!fat_lookup_path(ctx, path, &lr) || !lr.found) return 0;

  victim->valid = 1;
  victim->ctx = ctx;
  ix_memcpy(victim->path, path, (uint32_t)plen + 1u);
  victim->lr = lr;
  victim->idx = 0;
  victim->cur = lr.cluster;
  victim->stamp = ++g_fat_rc_tick;
  return victim;
}

int fat_read_at(fat_context_t* ctx, const char* path,
                uint32_t offset, void* buffer, uint32_t size) {
  if (!ctx || !ctx->valid || !path || !buffer) return -1;

  fat_rc_t  local;
  fat_rc_t* e = fat_rc_get(ctx, path);
  if (!e) {
    /* Слишком длинный путь или не найден: без кэша. */
    if (!fat_lookup_path(ctx, path, &local.lr) || !local.lr.found) return -1;
    local.idx = 0;
    local.cur = local.lr.cluster;
    e = &local;
  }
  const fat_lookup_result_t* lr = &e->lr;

  if (lr->attr & FAT_ATTR_DIRECTORY) return -1;
  if (offset >= lr->size || size == 0) return 0;
  if (offset + size > lr->size) size = lr->size - offset;

  uint8_t* dst = (uint8_t*)buffer;
  uint32_t done = 0;

  uint32_t skip = offset / ctx->cluster_size;
  uint32_t in_off = offset % ctx->cluster_size;

  /* Стартовая точка: из кэша, если мы не ушли назад. */
  uint32_t idx, cluster;
  if (skip >= e->idx) { idx = e->idx; cluster = e->cur; }
  else                { idx = 0;      cluster = lr->cluster; }

  while (idx < skip) {
    if (cluster < 2u || fat_is_eoc(ctx, cluster)) return (int)done;
    cluster = fat_next_cluster(ctx, cluster);
    idx++;
  }

  while (done < size) {
    if (cluster < 2u || fat_is_eoc(ctx, cluster)) break;

    uint32_t sec_in_clus = in_off / ctx->bytes_per_sector;
    uint32_t byte_in_sec = in_off % ctx->bytes_per_sector;

    uint64_t lba = fat_cluster_to_lba(ctx, cluster) + sec_in_clus;
    if (!fat_read_sector(ctx, lba, 1, g_fat_secbuf)) break;

    uint32_t avail = ctx->bytes_per_sector - byte_in_sec;
    if (avail > size - done) avail = size - done;

    ix_memcpy(dst + done, g_fat_secbuf + byte_in_sec, avail);
    done += avail;
    in_off += avail;

    if (in_off >= ctx->cluster_size) {
      in_off = 0;
      cluster = fat_next_cluster(ctx, cluster);
      idx++;
    }
  }

  /* Запомнить позицию (кэшируем только позицию, не за концом цепочки). */
  if (e != &local && cluster >= 2u && !fat_is_eoc(ctx, cluster)) {
    e->idx = idx;
    e->cur = cluster;
  }

  return (int)done;
}

/*
 * Обновить size и first cluster записи файла в родительском каталоге.
 * Расположение записи ищется повторным сканом родителя.
 */
static int fat_update_dirent(fat_context_t* ctx, const char* path,
                             uint32_t new_size, uint32_t first_cluster) {
  /* Разрезать путь на родителя и лист. */
  char parent[FAT_NAME_MAX];
  const char* leaf;
  int plen = 0;
  int last_slash = -1;

  while (path[plen]) {
    if (path[plen] == '/') last_slash = plen;
    plen++;
  }
  leaf = (last_slash >= 0) ? path + last_slash + 1 : path;

  if (last_slash >= 0) {
    int n = last_slash;
    if (n >= (int)sizeof(parent)) n = sizeof(parent) - 1;
    ix_memcpy(parent, path, n);
    parent[n] = 0;
  } else {
    parent[0] = 0;   /* корень */
  }

  int is_root16 = 0;
  uint32_t dir_cluster;

  if (!parent[0] || (parent[0] == '/' && !parent[1])) {
    if (ctx->fat_type == IX_FS_TYPE_FAT32) {
      dir_cluster = ctx->root_cluster;
    } else {
      is_root16 = 1;
      dir_cluster = 0;
    }
  } else {
    fat_lookup_result_t pr;
    if (!fat_lookup_path(ctx, parent, &pr) || !pr.found) return -1;
    if (!(pr.attr & FAT_ATTR_DIRECTORY)) return -1;
    dir_cluster = pr.cluster;
  }

  uint64_t ent_lba = 0;
  uint32_t ent_idx = 0;
  fat_lookup_result_t lr;

  if (is_root16) {
    if (!fat_scan_dir_root16(ctx, leaf, &lr, &ent_lba, &ent_idx)) return -1;
  } else {
    if (!fat_scan_dir_cluster(ctx, dir_cluster, leaf, &lr, &ent_lba, &ent_idx)) return -1;
  }
  if (!lr.found) return -1;

  if (!fat_read_sector(ctx, ent_lba, 1, g_fat_secbuf)) return -1;

  fat_dir_entry_t* de = (fat_dir_entry_t*)(g_fat_secbuf + ent_idx * 32u);
  de->file_size = new_size;
  de->fst_clus_lo = (uint16_t)(first_cluster & 0xFFFFu);
  de->fst_clus_hi = (uint16_t)((first_cluster >> 16) & 0xFFFFu);

  return fat_write_sector(ctx, ent_lba, 1, g_fat_secbuf) ? 0 : -1;
}

/*
 * Stateless запись: расширяет цепочку кластеров при необходимости,
 * пишет данные секторами, затем обновляет запись каталога.
 * Возвращает число записанных байт либо -1.
 */
int fat_write_at(fat_context_t* ctx, const char* path,
                 uint32_t offset, const void* buffer, uint32_t size) {
  if (!ctx || !ctx->valid || !path || !buffer || size == 0) return -1;

  fat_lookup_result_t lr;
  if (!fat_lookup_path(ctx, path, &lr) || !lr.found) return -1;
  if (lr.attr & FAT_ATTR_DIRECTORY) return -1;
  if (lr.attr & FAT_ATTR_READ_ONLY) return -1;

  uint32_t old_size   = lr.size;
  uint32_t file_first = (lr.cluster >= 2u) ? lr.cluster : 0;
  uint32_t end        = offset + size;

  /* Идти до кластера, содержащего offset, создавая недостающие. */
  uint32_t target_idx = offset / ctx->cluster_size;
  uint32_t prev = 0;
  uint32_t cur  = lr.cluster;

  for (uint32_t i = 0; i < target_idx; i++) {
    if (cur < 2u || fat_is_eoc(ctx, cur)) {
      uint32_t nc = fat_alloc_cluster(ctx);
      if (!nc) return -1;
      if (!fat_zero_cluster(ctx, nc)) return -1;
      if (prev >= 2u) {
        if (!fat_set_fat_entry(ctx, prev, nc)) return -1;
      } else {
        file_first = nc;
      }
      cur = nc;
    }
    prev = cur;
    cur  = fat_next_cluster(ctx, cur);
  }

  if (cur < 2u || fat_is_eoc(ctx, cur)) {
    uint32_t nc = fat_alloc_cluster(ctx);
    if (!nc) return -1;
    if (!fat_zero_cluster(ctx, nc)) return -1;
    if (prev >= 2u) {
      if (!fat_set_fat_entry(ctx, prev, nc)) return -1;
    } else {
      file_first = nc;
    }
    cur = nc;
  }

  /* Запись данных. */
  const uint8_t* src = (const uint8_t*)buffer;
  uint32_t done = 0;
  uint32_t pos  = offset;
  int      first_iter = 1;

  while (done < size) {
    uint32_t coff = pos % ctx->cluster_size;
    int      progressed = 0;

    /*
     * Дыра между старым EOF и offset -- заполняем нулями
     * (FAT этого не требует, но предсказуемость важнее).
     */
    if (first_iter && offset > old_size) {
      uint32_t gap_start = old_size % ctx->cluster_size;
      if (gap_start >= coff) gap_start = 0;

      for (uint32_t s = 0; s < ctx->sectors_per_cluster && gap_start < coff; s++) {
        uint32_t s0 = s * ctx->bytes_per_sector;
        uint32_t s1 = s0 + ctx->bytes_per_sector;
        if (gap_start >= s1 || coff <= s0) continue;

        uint32_t a = (gap_start > s0) ? gap_start - s0 : 0;
        uint32_t b = (coff < s1) ? coff - s0 : ctx->bytes_per_sector;
        if (b <= a) continue;

        uint64_t lba = fat_cluster_to_lba(ctx, cur) + s;
        if (!fat_read_sector(ctx, lba, 1, g_fat_secbuf)) return -1;
        ix_memset(g_fat_secbuf + a, 0, b - a);
        if (!fat_write_sector(ctx, lba, 1, g_fat_secbuf)) return -1;
      }
    }
    first_iter = 0;

    for (uint32_t s = 0; s < ctx->sectors_per_cluster; s++) {
      uint32_t s0 = s * ctx->bytes_per_sector;
      uint32_t s1 = s0 + ctx->bytes_per_sector;
      if (coff >= s1) continue;
      if (done >= size) break;

      uint32_t avail = size - done;
      uint32_t a = (coff > s0) ? coff - s0 : 0;
      uint32_t b = ((uint64_t)coff + avail < s1) ? (coff + avail - s0)
                                                 : ctx->bytes_per_sector;
      if (b <= a) continue;

      uint64_t lba = fat_cluster_to_lba(ctx, cur) + s;

      if (a == 0 && b == ctx->bytes_per_sector) {
        ix_memcpy(g_fat_secbuf, src + done, ctx->bytes_per_sector);
        if (!fat_write_sector(ctx, lba, 1, g_fat_secbuf)) return progressed ? (int)done : -1;
      } else {
        if (!fat_read_sector(ctx, lba, 1, g_fat_secbuf)) return progressed ? (int)done : -1;
        ix_memcpy(g_fat_secbuf + a, src + done, b - a);
        if (!fat_write_sector(ctx, lba, 1, g_fat_secbuf)) return progressed ? (int)done : -1;
      }

      uint32_t wrote = b - a;
      done += wrote;
      pos  += wrote;
      coff += wrote;
      progressed = 1;
    }

    if (done < size) {
      uint32_t nxt = fat_next_cluster(ctx, cur);
      if (nxt < 2u || fat_is_eoc(ctx, nxt)) {
        nxt = fat_alloc_cluster(ctx);
        if (!nxt) return (int)done;
        if (!fat_zero_cluster(ctx, nxt)) return (int)done;
        if (!fat_set_fat_entry(ctx, cur, nxt)) return (int)done;
      }
      cur = nxt;
    }
  }

  /* Обновить запись каталога. */
  uint32_t new_size = (end > old_size) ? end : old_size;
  if (new_size != old_size || file_first != lr.cluster) {
    if (fat_update_dirent(ctx, path, new_size, file_first) != 0)
      return (int)done;
  }

  return (int)done;
}

/* ---------------------------------------------------------------------
 *  Создание файлов: 8.3 + LFN
 * ------------------------------------------------------------------- */

static int fat_valid_sfn_char(char c) {
  unsigned char u = (unsigned char)c;
  if (u < 0x20 || u == 0x7F) return 0;
  const char* bad = "\"*+,./:;<=>?[\\]|";
  for (int i = 0; bad[i]; i++)
    if (c == bad[i]) return 0;
  return 1;
}

static char fat_up(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* Контрольная сумма короткого имени для LFN-записей. */
static uint8_t fat_lfn_checksum(const uint8_t* sfn) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; i++)
    sum = (uint8_t)((((sum & 1u) << 7) + (sum >> 1) + sfn[i]) & 0xFFu);
  return sum;
}

/*
 * Разобрать лист на base/ext, собрать SFN-байты.
 * needs_lfn = 1, если имя не представимо 8.3 точно (длина,
 * недопустимые символы, строчные буквы).
 * Возвращает 0 при успехе.
 */
static int fat_build_sfn(const char* leaf, uint8_t* out11, int* needs_lfn) {
  char base[256];
  char ext[4];
  int  blen = 0, elen = 0;
  int  dot = -1;
  int  len = 0;

  *needs_lfn = 0;

  while (leaf[len]) {
    if (leaf[len] == '.') dot = len;
    len++;
  }
  if (len == 0 || len > 255) return -1;
  if (dot == 0 || dot == len - 1) {
    /* ".name" или "name." -- имени-алиасу всё равно нужен LFN-подход */
    dot = -1;
    if (leaf[0] == '.') return -1;
  }

  int base_end = (dot >= 0) ? dot : (leaf[len - 1] == '.' ? len - 1 : len);
  for (int i = 0; i < base_end && blen < 255; i++) base[blen++] = leaf[i];
  if (dot >= 0) {
    for (int i = dot + 1; i < len && elen < 3; i++) ext[elen++] = leaf[i];
    if ((len - dot - 1) > 3) *needs_lfn = 1;
  } else if (leaf[len - 1] == '.') {
    *needs_lfn = 1;   /* хвостовая точка не представима 8.3 */
  }

  if (blen == 0) return -1;
  if (blen > 8) *needs_lfn = 1;

  for (int i = 0; i < blen; i++) {
    if (!fat_valid_sfn_char(base[i])) { *needs_lfn = 1; base[i] = '_'; }
  }
  /* Строчные буквы в оригинале тоже требуют LFN. */
  for (int i = 0; i < base_end; i++)
    if (leaf[i] >= 'a' && leaf[i] <= 'z') *needs_lfn = 1;
  for (int i = dot + 1; i < len; i++)
    if (leaf[i] >= 'a' && leaf[i] <= 'z') *needs_lfn = 1;

  for (int i = 0; i < elen; i++)
    if (!fat_valid_sfn_char(ext[i])) { *needs_lfn = 1; ext[i] = '_'; }

  for (int i = 0; i < 11; i++) out11[i] = ' ';

  int n = (blen > 8) ? 8 : blen;
  for (int i = 0; i < n; i++) out11[i] = (uint8_t)fat_up(base[i]);
  for (int i = 0; i < elen; i++) out11[8 + i] = (uint8_t)fat_up(ext[i]);

  /* Хвостовые точки/пробелы в SFN запрещены. */
  for (int i = 7; i >= 0 && out11[i] == ' '; i--) out11[i] = ' ';
  return 0;
}

/* Есть ли в каталоге короткая запись ровно с этими 11 байтами. */
static int fat_sfn_exists(fat_context_t* ctx, int is_root16,
                          uint32_t dir_cluster, const uint8_t* sfn11) {
  if (is_root16) {
    for (uint32_t s = 0; s < ctx->root_dir_sectors; s++) {
      if (!fat_read_sector(ctx, ctx->root_dir_lba + s, 1, g_fat_secbuf)) return 0;
      for (uint32_t e = 0; e < ctx->bytes_per_sector / 32u; e++) {
        const fat_dir_entry_t* de = (const fat_dir_entry_t*)(g_fat_secbuf + e * 32u);
        if (de->name[0] == 0x00) return 0;
        if (de->name[0] == 0xE5 || de->attr == FAT_ATTR_LONG_NAME) continue;
        if (de->attr & FAT_ATTR_VOLUME_ID) continue;
        int same = 1;
        for (int i = 0; i < 11; i++)
          if (de->name[i] != sfn11[i]) { same = 0; break; }
        if (same) return 1;
      }
    }
    return 0;
  }

  uint32_t cluster = dir_cluster;
  uint32_t guard = 1000000u;
  while (!fat_is_eoc(ctx, cluster) && cluster >= 2u && guard--) {
    uint64_t lba = fat_cluster_to_lba(ctx, cluster);
    for (uint32_t s = 0; s < ctx->sectors_per_cluster; s++) {
      if (!fat_read_sector(ctx, lba + s, 1, g_fat_secbuf)) return 0;
      for (uint32_t e = 0; e < ctx->bytes_per_sector / 32u; e++) {
        const fat_dir_entry_t* de = (const fat_dir_entry_t*)(g_fat_secbuf + e * 32u);
        if (de->name[0] == 0x00) return 0;
        if (de->name[0] == 0xE5 || de->attr == FAT_ATTR_LONG_NAME) continue;
        if (de->attr & FAT_ATTR_VOLUME_ID) continue;
        int same = 1;
        for (int i = 0; i < 11; i++)
          if (de->name[i] != sfn11[i]) { same = 0; break; }
        if (same) return 1;
      }
    }
    cluster = fat_next_cluster(ctx, cluster);
  }
  return 0;
}

#define FAT_STAMP_DATE 0x5A21u   /* 2025-01-01 */
#define FAT_STAMP_TIME 0x6000u   /* 12:00:00  */

static int fat_dir_map_entry(fat_context_t* ctx, int is_root16,
                             uint32_t dir_cluster, uint32_t index,
                             int allow_extend, uint64_t* out_lba);
static int fat_dir_read_entry(fat_context_t* ctx, int is_root16,
                              uint32_t dir_cluster, uint32_t index,
                              fat_dir_entry_t* out);
static int fat_dir_write_entry(fat_context_t* ctx, int is_root16,
                               uint32_t dir_cluster, uint32_t index,
                               const fat_dir_entry_t* in);

/*
 * Записать короткую запись каталога (имя уже в SFN-байтах).
 * Общая часть fat_create и fat_mkdir: 8.3-алиас, LFN-записи,
 * поиск свободных слотов, при необходимости -- расширение
 * кластерного каталога.
 * attr уже должен быть выставлен (ARCHIVE или DIRECTORY).
 * first_cluster -- значение для fst_clus_lo/fst_clus_hi.
 */
static int fat_add_entry_common(fat_context_t* ctx, int is_root16,
                                uint32_t dir_cluster, const char* leaf,
                                uint8_t attr, uint32_t first_cluster) {
  uint8_t  sfn11[11];
  int      needs_lfn = 0;
  if (fat_build_sfn(leaf, sfn11, &needs_lfn) != 0) return -1;

  if (!needs_lfn && fat_sfn_exists(ctx, is_root16, dir_cluster, sfn11))
    needs_lfn = 1;

  if (needs_lfn) {
    uint8_t alias[11];
    uint32_t tail;
    for (tail = 1; tail < 100000u; tail++) {
      ix_memcpy(alias, sfn11, 11);

      char digs[8];
      int  nd = 0;
      uint32_t t = tail;
      if (t == 0) digs[nd++] = '0';
      while (t) { digs[nd++] = (char)('0' + (t % 10)); t /= 10; }

      int tlen = nd + 1;
      if (tlen > 4) return -1;

      int baselen = 0;
      while (baselen < 8 && alias[baselen] != ' ') baselen++;
      int keep = baselen;
      if (keep > 8 - tlen) keep = 8 - tlen;
      if (keep <= 0) return -1;

      alias[keep] = '~';
      for (int i = 0; i < nd; i++)
        alias[keep + 1 + i] = (uint8_t)digs[nd - 1 - i];
      for (int i = keep + tlen; i < 8; i++) alias[i] = ' ';

      if (!fat_sfn_exists(ctx, is_root16, dir_cluster, alias)) {
        ix_memcpy(sfn11, alias, 11);
        break;
      }
    }
    if (tail >= 100000u) return -1;
  }

  /* LFN-записи (UTF-16 из ASCII). */
  char     lname[FAT_NAME_MAX];
  uint16_t lchars[FAT_NAME_MAX + 1];
  int      llen = 0;
  while (leaf[llen] && llen < FAT_NAME_MAX - 1) { lname[llen] = leaf[llen]; llen++; }
  lname[llen] = 0;

  int lfn_n = 0;
  if (needs_lfn) {
    int ulen = 0;
    const char* up = lname;
    while (*up && ulen < FAT_NAME_MAX - 2) {
      uint32_t cp = ix_utf8_next(&up);
      if (cp < 0x20u) cp = '?';
      if (cp >= 0x10000u) {
        cp -= 0x10000u;
        lchars[ulen++] = (uint16_t)(0xD800u | (cp >> 10));
        lchars[ulen++] = (uint16_t)(0xDC00u | (cp & 0x3FFu));
      } else {
        lchars[ulen++] = (uint16_t)cp;
      }
    }
    llen = ulen;
    lchars[llen] = 0x0000u;
    int units = llen + 1;
    lfn_n = (units + 12) / 13;
    for (int i = units; i < lfn_n * 13; i++) lchars[i] = 0xFFFFu;
  }

  uint32_t need = (uint32_t)lfn_n + 1u;

  uint32_t cap_hint = 0;
  int      have_end = 0;
  long     run_start = -1;
  uint32_t run_len = 0;
  long     found_at = -1;

  for (uint32_t idx = 0; ; idx++) {
    fat_dir_entry_t tmp;
    if (fat_dir_read_entry(ctx, is_root16, dir_cluster, idx, &tmp) != 0)
      break;

    if (tmp.name[0] == 0x00) {
      cap_hint = idx;
      have_end = 1;
      break;
    }
    if (tmp.name[0] == 0xE5) {
      if (run_len == 0) run_start = (long)idx;
      run_len++;
      if (run_len >= need) { found_at = run_start; break; }
    } else {
      run_len = 0;
      run_start = -1;
    }
  }

  uint32_t base_idx;

  if (found_at >= 0) {
    base_idx = (uint32_t)found_at;
  } else {
    if (!have_end) {
      if (is_root16) return -1;
      uint32_t per_cluster = ctx->cluster_size / 32u;
      uint32_t c = dir_cluster, total = 0, guard = 100000u;
      while (c >= 2u && !fat_is_eoc(ctx, c) && guard--) {
        total += per_cluster;
        c = fat_next_cluster(ctx, c);
      }
      cap_hint = total;
      have_end = 1;
    }
    base_idx = cap_hint;

    fat_dir_entry_t probe;
    if (fat_dir_read_entry(ctx, is_root16, dir_cluster,
                           base_idx + need - 1u, &probe) != 0) {
      if (is_root16) return -1;
      uint64_t dummy;
      if (fat_dir_map_entry(ctx, is_root16, dir_cluster,
                            base_idx + need - 1u, 1, &dummy) != 0)
        return -1;
    }
  }

  uint8_t  csum = fat_lfn_checksum(sfn11);
  uint32_t slot = base_idx;

  for (int i = 0; i < lfn_n; i++, slot++) {
    fat_lfn_entry_t le;
    ix_memset(&le, 0, sizeof(le));
    int order = lfn_n - i;
    le.order = (uint8_t)((i == 0) ? (order | 0x40u) : (uint32_t)order);
    le.attr  = FAT_ATTR_LONG_NAME;
    le.type  = 0;
    le.checksum = csum;
    le.fst_clus_lo = 0;

    for (int j = 0; j < 5; j++)  le.name1[j] = lchars[i * 13 + j];
    for (int j = 0; j < 6; j++)  le.name2[j] = lchars[i * 13 + 5 + j];
    for (int j = 0; j < 2; j++)  le.name3[j] = lchars[i * 13 + 11 + j];

    if (fat_dir_write_entry(ctx, is_root16, dir_cluster, slot,
                            (const fat_dir_entry_t*)&le) != 0)
      return -1;
  }

  {
    fat_dir_entry_t de;
    ix_memset(&de, 0, sizeof(de));
    ix_memcpy(de.name, sfn11, 11);
    de.attr       = attr;
    de.crt_time_tenth = 0;
    de.crt_time   = FAT_STAMP_TIME;
    de.crt_date   = FAT_STAMP_DATE;
    de.lst_acc_date = FAT_STAMP_DATE;
    de.wrt_time   = FAT_STAMP_TIME;
    de.wrt_date   = FAT_STAMP_DATE;
    de.fst_clus_hi = (uint16_t)(first_cluster >> 16);
    de.fst_clus_lo = (uint16_t)(first_cluster & 0xFFFFu);
    de.file_size  = 0;

    if (fat_dir_write_entry(ctx, is_root16, dir_cluster, slot, &de) != 0)
      return -1;
  }

  return 0;
}

/*
 * Создать подкаталог: кластер под содержимое, записи "." и "..",
 * затем запись каталога в родителе.
 *
 * root16 (FAT12/16): . и .. ссылаются на "0" -- так же, как для
 * корня в стандарте.
 */
int fat_mkdir(fat_context_t* ctx, const char* path) {
  if (!ctx || !ctx->valid || !path || !path[0]) return -1;

  char parent[FAT_NAME_MAX];
  const char* leaf;
  int plen = 0, last_slash = -1;
  while (path[plen]) {
    if (path[plen] == '/' || path[plen] == (char)92) last_slash = plen;
    plen++;
  }
  leaf = (last_slash >= 0) ? path + last_slash + 1 : path;
  if (!leaf[0]) return -1;
  if (leaf[0] == '.' && (!leaf[1] || (leaf[1] == '.' && !leaf[2]))) return -1;

  if (last_slash >= 0) {
    int n = last_slash;
    if (n >= (int)sizeof(parent)) n = sizeof(parent) - 1;
    ix_memcpy(parent, path, n);
    parent[n] = 0;
  } else {
    parent[0] = 0;
  }

  int is_root16 = 0;
  uint32_t parent_cluster = 0;
  uint32_t dotdot_cluster = 0;

  if (!parent[0] || (parent[0] == '/' && !parent[1])) {
    if (ctx->fat_type == IX_FS_TYPE_FAT32) parent_cluster = ctx->root_cluster;
    else { is_root16 = 1; parent_cluster = 0; }
  } else {
    fat_lookup_result_t pr;
    if (!fat_lookup_path(ctx, parent, &pr) || !pr.found) return -1;
    if (!(pr.attr & FAT_ATTR_DIRECTORY)) return -1;
    if (pr.attr & FAT_ATTR_READ_ONLY) return -1;
    parent_cluster = pr.cluster;
    dotdot_cluster = pr.cluster;
  }

  /* Уже существует (файл или каталог)? */
  {
    fat_lookup_result_t lr;
    int found;
    if (is_root16) found = fat_scan_dir_root16(ctx, leaf, &lr, 0, 0);
    else           found = fat_scan_dir_cluster(ctx, parent_cluster, leaf, &lr, 0, 0);
    if (found && lr.found) return -1;
  }

  uint32_t new_cluster = fat_alloc_cluster(ctx);
  if (!new_cluster) return -1;
  if (!fat_zero_cluster(ctx, new_cluster)) {
    fat_set_fat_entry(ctx, new_cluster, 0);
    return -1;
  }

  /* "." и ".." в новом каталоге. */
  {
    static uint8_t minor_buf[FAT_SECTOR_SIZE];
    fat_dir_entry_t dot;
    int ok = 1;

    if (!fat_read_cluster(ctx, new_cluster, minor_buf)) {
      ok = 0;
    } else {
      ix_memset(&dot, 0, sizeof(dot));
      ix_memset(dot.name, ' ', 11);
      dot.name[0]   = '.';
      dot.attr      = FAT_ATTR_DIRECTORY;
      dot.crt_time  = FAT_STAMP_TIME;
      dot.crt_date  = FAT_STAMP_DATE;
      dot.lst_acc_date = FAT_STAMP_DATE;
      dot.wrt_time  = FAT_STAMP_TIME;
      dot.wrt_date  = FAT_STAMP_DATE;
      dot.fst_clus_hi = (uint16_t)(new_cluster >> 16);
      dot.fst_clus_lo = (uint16_t)(new_cluster & 0xFFFFu);
      ix_memcpy(minor_buf, &dot, sizeof(dot));

      ix_memset(&dot, 0, sizeof(dot));
      ix_memset(dot.name, ' ', 11);
      dot.name[0] = '.';
      dot.name[1] = '.';
      dot.attr    = FAT_ATTR_DIRECTORY;
      dot.crt_time = FAT_STAMP_TIME;
      dot.crt_date = FAT_STAMP_DATE;
      dot.lst_acc_date = FAT_STAMP_DATE;
      dot.wrt_time = FAT_STAMP_TIME;
      dot.wrt_date = FAT_STAMP_DATE;
      dot.fst_clus_hi = is_root16 ? 0u : (uint16_t)(dotdot_cluster >> 16);
      dot.fst_clus_lo = is_root16 ? 0u : (uint16_t)(dotdot_cluster & 0xFFFFu);
      ix_memcpy(minor_buf + 32, &dot, sizeof(dot));

      ok = fat_write_sector(ctx, fat_cluster_to_lba(ctx, new_cluster),
                            1, minor_buf);
    }

    if (!ok) {
      fat_set_fat_entry(ctx, new_cluster, 0);
      return -1;
    }
  }

  if (fat_add_entry_common(ctx, is_root16, parent_cluster, leaf,
                           FAT_ATTR_DIRECTORY, new_cluster) != 0) {
    fat_set_fat_entry(ctx, new_cluster, 0);
    return -1;
  }

  return 0;
}

/*
 * Отобразить глобальный индекс записи каталога в (LBA, индекс в секторе).
 * allow_extend: для кластерных каталогов создавать новые кластеры
 * (обнулять их).  Для root16 расширение невозможно.
 */
static int fat_dir_map_entry(fat_context_t* ctx, int is_root16,
                             uint32_t dir_cluster, uint32_t index,
                             int allow_extend, uint64_t* out_lba) {
  uint32_t eps = ctx->bytes_per_sector / 32u;

  if (is_root16) {
    if (index >= ctx->root_entry_count) return -1;
    *out_lba = ctx->root_dir_lba + (index / eps);
    return 0;
  }

  uint32_t per_cluster = ctx->cluster_size / 32u;
  uint32_t cur  = dir_cluster;
  uint32_t prev = 0;
  uint32_t left = index;

  while (left >= per_cluster) {
    left -= per_cluster;
    uint32_t nxt = (cur >= 2u && !fat_is_eoc(ctx, cur))
                     ? fat_next_cluster(ctx, cur) : 0;
    if (nxt < 2u || fat_is_eoc(ctx, nxt)) {
      if (!allow_extend) return -1;
      nxt = fat_alloc_cluster(ctx);
      if (!nxt) return -1;
      if (!fat_zero_cluster(ctx, nxt)) return -1;
      if (prev >= 2u) {
        if (!fat_set_fat_entry(ctx, prev, nxt)) return -1;
      } else if (cur < 2u || fat_is_eoc(ctx, cur)) {
        /* Каталог нулевой длины -- не бывает, но подстрахуемся. */
        return -1;
      } else {
        if (!fat_set_fat_entry(ctx, cur, nxt)) return -1;
      }
    }
    prev = cur;
    cur  = nxt;
  }

  if (cur < 2u || fat_is_eoc(ctx, cur)) return -1;

  *out_lba = fat_cluster_to_lba(ctx, cur) + (left / eps);
  return 0;
}

/* Прочитать 32-байтовую запись каталога по глобальному индексу. */
static int fat_dir_read_entry(fat_context_t* ctx, int is_root16,
                              uint32_t dir_cluster, uint32_t index,
                              fat_dir_entry_t* out) {
  uint64_t lba;
  if (fat_dir_map_entry(ctx, is_root16, dir_cluster, index, 0, &lba) != 0)
    return -1;
  if (!fat_read_sector(ctx, lba, 1, g_fat_secbuf)) return -1;
  ix_memcpy(out, g_fat_secbuf + (index % (ctx->bytes_per_sector / 32u)) * 32u, 32);
  return 0;
}

static int fat_dir_write_entry(fat_context_t* ctx, int is_root16,
                               uint32_t dir_cluster, uint32_t index,
                               const fat_dir_entry_t* in) {
  uint64_t lba;
  if (fat_dir_map_entry(ctx, is_root16, dir_cluster, index, 1, &lba) != 0)
    return -1;
  if (!fat_read_sector(ctx, lba, 1, g_fat_secbuf)) return -1;
  ix_memcpy(g_fat_secbuf + (index % (ctx->bytes_per_sector / 32u)) * 32u, in, 32);
  return fat_write_sector(ctx, lba, 1, g_fat_secbuf) ? 0 : -1;
}

#define FAT_STAMP_DATE 0x5A21u   /* 2025-01-01 */
#define FAT_STAMP_TIME 0x6000u   /* 12:00:00  */

/*
 * Создать файл (лист) в каталоге.  Родитель должен существовать.
 * Поддержаны LFN-записи и алиасы вида BASE~1.EXT.
 */
int fat_create(fat_context_t* ctx, const char* path) {
  if (!ctx || !ctx->valid || !path || !path[0]) return -1;

  /* Родитель + лист. */
  char parent[FAT_NAME_MAX];
  const char* leaf;
  int plen = 0, last_slash = -1;
  while (path[plen]) {
    if (path[plen] == '/') last_slash = plen;
    plen++;
  }
  leaf = (last_slash >= 0) ? path + last_slash + 1 : path;
  if (!leaf[0]) return -1;
  if (leaf[0] == '.' && (!leaf[1] || (leaf[1] == '.' && !leaf[2]))) return -1;

  if (last_slash >= 0) {
    int n = last_slash;
    if (n >= (int)sizeof(parent)) n = sizeof(parent) - 1;
    ix_memcpy(parent, path, n);
    parent[n] = 0;
  } else {
    parent[0] = 0;
  }

  int is_root16 = 0;
  uint32_t dir_cluster;

  if (!parent[0] || (parent[0] == '/' && !parent[1])) {
    if (ctx->fat_type == IX_FS_TYPE_FAT32) dir_cluster = ctx->root_cluster;
    else { is_root16 = 1; dir_cluster = 0; }
  } else {
    fat_lookup_result_t pr;
    if (!fat_lookup_path(ctx, parent, &pr) || !pr.found) return -1;
    if (!(pr.attr & FAT_ATTR_DIRECTORY)) return -1;
    if (pr.attr & FAT_ATTR_READ_ONLY) return -1;
    dir_cluster = pr.cluster;
  }

  /* Уже существует? */
  {
    fat_lookup_result_t lr;
    int found;
    if (is_root16) found = fat_scan_dir_root16(ctx, leaf, &lr, 0, 0);
    else           found = fat_scan_dir_cluster(ctx, dir_cluster, leaf, &lr, 0, 0);
    if (found && lr.found) return -1;
  }

  uint8_t  sfn11[11];
  int      needs_lfn = 0;
  if (fat_build_sfn(leaf, sfn11, &needs_lfn) != 0) return -1;

  if (!needs_lfn && fat_sfn_exists(ctx, is_root16, dir_cluster, sfn11))
    needs_lfn = 1;

  if (needs_lfn) {
    /* Алиас: первые 6 символов + "~" + десятичный хвост. */
    uint8_t alias[11];
    uint32_t tail;
    for (tail = 1; tail < 100000u; tail++) {
      ix_memcpy(alias, sfn11, 11);

      char digs[8];
      int  nd = 0;
      uint32_t t = tail;
      if (t == 0) digs[nd++] = '0';
      while (t) { digs[nd++] = (char)('0' + (t % 10)); t /= 10; }

      int tlen = nd + 1;             /* "~" + цифры */
      if (tlen > 4) { return -1; }

      int baselen = 0;
      while (baselen < 8 && alias[baselen] != ' ') baselen++;
      int keep = baselen;
      if (keep > 8 - tlen) keep = 8 - tlen;
      if (keep <= 0) return -1;

      alias[keep] = '~';
      for (int i = 0; i < nd; i++)
        alias[keep + 1 + i] = (uint8_t)digs[nd - 1 - i];
      for (int i = keep + tlen; i < 8; i++) alias[i] = ' ';

      if (!fat_sfn_exists(ctx, is_root16, dir_cluster, alias)) {
        ix_memcpy(sfn11, alias, 11);
        break;
      }
    }
    if (tail >= 100000u) return -1;
  }

  /* LFN-записи (UTF-16 из ASCII). */
  char     lname[FAT_NAME_MAX];
  uint16_t lchars[FAT_NAME_MAX + 1];
  int      llen = 0;
  while (leaf[llen] && llen < FAT_NAME_MAX - 1) { lname[llen] = leaf[llen]; llen++; }
  lname[llen] = 0;

  int lfn_n = 0;
  if (needs_lfn) {
    /* UTF-8 -> UTF-16 (суррогатные пары для U+10000 и выше). */
    int ulen = 0;
    const char* up = lname;
    while (*up && ulen < FAT_NAME_MAX - 2) {
      uint32_t cp = ix_utf8_next(&up);
      if (cp < 0x20u) cp = '?';
      if (cp >= 0x10000u) {
        cp -= 0x10000u;
        lchars[ulen++] = (uint16_t)(0xD800u | (cp >> 10));
        lchars[ulen++] = (uint16_t)(0xDC00u | (cp & 0x3FFu));
      } else {
        lchars[ulen++] = (uint16_t)cp;
      }
    }
    llen = ulen;
    lchars[llen] = 0x0000u;
    int units = llen + 1;
    lfn_n = (units + 12) / 13;
    for (int i = units; i < lfn_n * 13; i++) lchars[i] = 0xFFFFu;
  }

  uint32_t need = (uint32_t)lfn_n + 1u;

  /*
   * Пасс 1: непрерывная серия свободных слотов (0xE5) среди
   * существующих записей.
   */
  uint32_t cap_hint = 0;      /* обнаруженный конец каталога (первый 0x00) */
  int      have_end = 0;
  long     run_start = -1;
  uint32_t run_len = 0;
  long     found_at = -1;

  for (uint32_t idx = 0; ; idx++) {
    fat_dir_entry_t tmp;
    if (fat_dir_read_entry(ctx, is_root16, dir_cluster, idx, &tmp) != 0)
      break;   /* конец ёмкости */

    if (tmp.name[0] == 0x00) {
      cap_hint = idx;
      have_end = 1;
      break;
    }
    if (tmp.name[0] == 0xE5) {
      if (run_len == 0) run_start = (long)idx;
      run_len++;
      if (run_len >= need) { found_at = run_start; break; }
    } else {
      run_len = 0;
      run_start = -1;
    }
  }

  uint32_t base_idx;

  if (found_at >= 0) {
    base_idx = (uint32_t)found_at;
  } else {
    /* Пасс 2: дописать в конец (с расширением для кластерных каталогов). */
    if (!have_end) {
      /* Каталог забит вплотную -- конец = текущая ёмкость. */
      if (is_root16) return -1;
      /* Найдём длину цепочки: пройдём с конца. */
      uint32_t per_cluster = ctx->cluster_size / 32u;
      uint32_t c = dir_cluster, total = 0, guard = 100000u;
      while (c >= 2u && !fat_is_eoc(ctx, c) && guard--) {
        total += per_cluster;
        c = fat_next_cluster(ctx, c);
      }
      cap_hint = total;
      have_end = 1;
    }
    base_idx = cap_hint;

    /* Проверить/создать ёмкость: последний слот должен адресоваться. */
    fat_dir_entry_t probe;
    if (fat_dir_read_entry(ctx, is_root16, dir_cluster,
                           base_idx + need - 1u, &probe) != 0) {
      if (is_root16) return -1;
      /* Расширить: map_entry с allow_extend сам создаст кластер. */
      uint64_t dummy;
      if (fat_dir_map_entry(ctx, is_root16, dir_cluster,
                            base_idx + need - 1u, 1, &dummy) != 0)
        return -1;
    }
  }

  uint8_t  csum = fat_lfn_checksum(sfn11);
  uint32_t slot = base_idx;

  /* LFN-записи: старший order -- первым физическим слотом. */
  for (int i = 0; i < lfn_n; i++, slot++) {
    fat_lfn_entry_t le;
    ix_memset(&le, 0, sizeof(le));
    int order = lfn_n - i;
    le.order = (uint8_t)((i == 0) ? (order | 0x40u) : (uint32_t)order);
    le.attr  = FAT_ATTR_LONG_NAME;
    le.type  = 0;
    le.checksum = csum;
    le.fst_clus_lo = 0;

    for (int j = 0; j < 5; j++)  le.name1[j] = lchars[i * 13 + j];
    for (int j = 0; j < 6; j++)  le.name2[j] = lchars[i * 13 + 5 + j];
    for (int j = 0; j < 2; j++)  le.name3[j] = lchars[i * 13 + 11 + j];

    if (fat_dir_write_entry(ctx, is_root16, dir_cluster, slot,
                            (const fat_dir_entry_t*)&le) != 0)
      return -1;
  }

  /* Короткая запись. */
  {
    fat_dir_entry_t de;
    ix_memset(&de, 0, sizeof(de));
    ix_memcpy(de.name, sfn11, 11);
    de.attr       = FAT_ATTR_ARCHIVE;
    de.crt_time_tenth = 0;
    de.crt_time   = FAT_STAMP_TIME;
    de.crt_date   = FAT_STAMP_DATE;
    de.lst_acc_date = FAT_STAMP_DATE;
    de.wrt_time   = FAT_STAMP_TIME;
    de.wrt_date   = FAT_STAMP_DATE;
    de.fst_clus_hi = 0;
    de.fst_clus_lo = 0;
    de.file_size  = 0;

    if (fat_dir_write_entry(ctx, is_root16, dir_cluster, slot, &de) != 0)
      return -1;
  }

  return 0;
}

/*
 * Обнулить файл: освободить цепочку кластеров, size = 0,
 * first cluster = 0.
 */

/*
 * Найти запись каталога, соответствующую leaf из path, и вернуть её
 * позицию (LBA сектора + индекс внутри сектора) вместе с SFN.
 * Используется удалением и переименованием: обеим операциям нужно
 * менять саму запись, а не только содержимое файла.
 */
static int fat_locate_entry(fat_context_t* ctx, const char* path,
                            uint64_t* out_lba, uint32_t* out_idx,
                            fat_lookup_result_t* out_lr) {
  char parent[FAT_NAME_MAX];
  const char* leaf;
  int plen = 0, last_slash = -1;

  while (path[plen]) {
    if (path[plen] == '/' || path[plen] == (char)92) last_slash = plen;
    plen++;
  }
  leaf = (last_slash >= 0) ? path + last_slash + 1 : path;
  if (!leaf[0]) return 0;
  if (leaf[0] == '.' && (!leaf[1] || (leaf[1] == '.' && !leaf[2]))) return 0;

  if (last_slash >= 0) {
    int n = last_slash;
    if (n >= (int)sizeof(parent)) n = sizeof(parent) - 1;
    ix_memcpy(parent, path, n);
    parent[n] = 0;
  } else {
    parent[0] = 0;
  }

  int is_root16 = 0;
  uint32_t dir_cluster;

  if (!parent[0] || (parent[0] == '/' && !parent[1])) {
    if (ctx->fat_type == IX_FS_TYPE_FAT32) dir_cluster = ctx->root_cluster;
    else { is_root16 = 1; dir_cluster = 0; }
  } else {
    fat_lookup_result_t pr;
    if (!fat_lookup_path(ctx, parent, &pr) || !pr.found) return 0;
    if (!(pr.attr & FAT_ATTR_DIRECTORY)) return 0;
    if (pr.attr & FAT_ATTR_READ_ONLY) return 0;
    dir_cluster = pr.cluster;
  }

  fat_lookup_result_t lr;
  uint64_t lba = 0;
  uint32_t idx = 0;

  if (is_root16) {
    if (!fat_scan_dir_root16(ctx, leaf, &lr, &lba, &idx)) return 0;
  } else {
    if (!fat_scan_dir_cluster(ctx, dir_cluster, leaf, &lr, &lba, &idx)) return 0;
  }

  if (!lr.found) return 0;
  if (lr.attr & FAT_ATTR_READ_ONLY) return 0;

  if (out_lba) *out_lba = lba;
  if (out_idx) *out_idx = idx;
  if (out_lr)  *out_lr  = lr;

  return 1;
}

/*
 * Погасить саму запись и предшествующие ей LFN-слоты: порядковый номер
 * LFN растёт к началу, поэтому идём назад, пока встречаем LONG_NAME с тем
 * же контрольным байтом.  Всё, что помечается, помечается 0xE5 -- ровно так
 * это делает DOS, и именно поэтому освобождённые слоты переиспользуются
 * созданием файлов.
 */
static int fat_kill_entry(fat_context_t* ctx, uint64_t ent_lba, uint32_t ent_idx) {
  uint32_t eps = ctx->bytes_per_sector / 32u;
  uint64_t lba = ent_lba;
  uint32_t idx = ent_idx;
  uint8_t  csum = 0;

  if (!fat_read_sector(ctx, lba, 1, g_fat_secbuf)) return -1;

  {
    const fat_dir_entry_t* de = (const fat_dir_entry_t*)(g_fat_secbuf + idx * 32u);
    if (de->name[0] == 0x00 || de->name[0] == 0xE5) return -1;
    csum = fat_lfn_checksum(de->name);
    ((fat_dir_entry_t *)(g_fat_secbuf + idx * 32u))->name[0] = (uint8_t)0xE5;
    if (!fat_write_sector(ctx, lba, 1, g_fat_secbuf)) return -1;
  }

  for (;;) {
    if (idx == 0) {
      if (lba == 0) break;
      lba--;
      idx = eps;
      if (!fat_read_sector(ctx, lba, 1, g_fat_secbuf)) return -1;
    }

    idx--;

    {
      const fat_dir_entry_t* de = (const fat_dir_entry_t*)(g_fat_secbuf + idx * 32u);
      if (de->attr != FAT_ATTR_LONG_NAME || de->name[0] == 0x00) break;
      if (((const fat_lfn_entry_t *)de)->checksum != csum) break;

      ((fat_dir_entry_t *)(g_fat_secbuf + idx * 32u))->name[0] = (uint8_t)0xE5;
      if (!fat_write_sector(ctx, lba, 1, g_fat_secbuf)) return -1;
    }

    if (lba == 0 && idx == 0) break;
  }

  return 0;
}

static int fat_free_chain(fat_context_t* ctx, uint32_t first) {
  uint32_t c = first;
  uint32_t guard = 1000000u;

  while (c >= 2u && !fat_is_eoc(ctx, c) && guard--) {
    uint32_t nxt = fat_next_cluster(ctx, c);
    if (!fat_set_fat_entry(ctx, c, 0)) return -1;
    c = nxt;
  }

  fat_alloc_cache_invalidate();
  return 0;
}

int fat_truncate_path(fat_context_t* ctx, const char* path) {
  if (!ctx || !ctx->valid || !path) return -1;

  fat_lookup_result_t lr;
  if (!fat_lookup_path(ctx, path, &lr) || !lr.found) return -1;
  if (lr.attr & FAT_ATTR_DIRECTORY) return -1;
  if (lr.attr & FAT_ATTR_READ_ONLY) return -1;

  uint32_t c = lr.cluster;
  uint32_t guard = 1000000u;

  while (c >= 2u && !fat_is_eoc(ctx, c) && guard--) {
    uint32_t nxt = fat_next_cluster(ctx, c);
    if (!fat_set_fat_entry(ctx, c, 0)) return -1;
    c = nxt;
  }

  fat_alloc_cache_invalidate();

  if (lr.cluster >= 2u || lr.size != 0)
    return fat_update_dirent(ctx, path, 0, 0);

  return 0;
}

/*
 * Удалить файл: освободить цепочку, погасить запись и её LFN-слоты.
 * Каталоги не удаляются -- у них внутри "." и "..", и освобождать их
 * цепочку вместе с чужими ссылками нужно отдельной, гораздо более
 * осторожной процедурой.
 */
int fat_unlink(fat_context_t* ctx, const char* path) {
  if (!ctx || !ctx->valid || !path || !path[0]) return -1;

  uint64_t lba = 0;
  uint32_t idx = 0;
  fat_lookup_result_t lr;

  if (!fat_locate_entry(ctx, path, &lba, &idx, &lr)) return -1;
  if (lr.attr & FAT_ATTR_DIRECTORY) return -1;
  if (lr.attr & FAT_ATTR_READ_ONLY) return -1;

  if (lr.cluster >= 2u && fat_free_chain(ctx, lr.cluster) != 0) return -1;
  if (fat_kill_entry(ctx, lba, idx) != 0) return -1;

  fat_rc_invalidate();
  return 0;
}

/*
 * Переименовать файл.  Реализовано как create новой записи + перенос
 * полей + удаление старой, а не правкой 11 байт на месте: смена имени
 * меняет и SFN, и всю LFN-цепочку, а также (в общем случае) контрольный
 * байт.  Порядок операций такой, чтобы при сбое посередине данные
 * оставались доступны под старым именем.
 */
int fat_rename(fat_context_t* ctx, const char* from, const char* to) {
  if (!ctx || !ctx->valid || !from || !to || !from[0] || !to[0]) return -1;

  uint64_t lba = 0;
  uint32_t idx = 0;
  fat_lookup_result_t lr;

  if (!fat_locate_entry(ctx, from, &lba, &idx, &lr)) return -1;
  if (lr.attr & FAT_ATTR_DIRECTORY) return -1;

  {
    fat_lookup_result_t existing;
    if (fat_lookup_path(ctx, to, &existing) && existing.found) return -1;
  }

  if (fat_create(ctx, to) != 0) return -1;

  {
    uint64_t nlb = 0;
    uint32_t nidx = 0;
    fat_lookup_result_t nlr;

    if (!fat_locate_entry(ctx, to, &nlb, &nidx, &nlr)) {
      fat_unlink(ctx, to);
      return -1;
    }

    if (nlr.attr & FAT_ATTR_READ_ONLY) {
      fat_unlink(ctx, to);
      return -1;
    }

    if (!fat_read_sector(ctx, nlb, 1, g_fat_secbuf)) {
      fat_unlink(ctx, to);
      return -1;
    }

    {
      fat_dir_entry_t* nd = (fat_dir_entry_t*)(g_fat_secbuf + nidx * 32u);

      nd->fst_clus_lo = (uint16_t)(lr.cluster & 0xFFFFu);
      nd->fst_clus_hi = (uint16_t)((lr.cluster >> 16) & 0xFFFFu);
      nd->file_size   = lr.size;
      nd->wrt_time    = lr.wrt_time;
      nd->wrt_date    = lr.wrt_date;
      nd->crt_time    = lr.crt_time;
      nd->crt_date    = lr.crt_date;

      if (!fat_write_sector(ctx, nlb, 1, g_fat_secbuf)) {
        fat_unlink(ctx, to);
        return -1;
      }
    }
  }

  if (fat_kill_entry(ctx, lba, idx) != 0) return -1;

  fat_rc_invalidate();
  return 0;
}
