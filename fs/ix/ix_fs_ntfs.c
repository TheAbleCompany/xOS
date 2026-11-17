#include "ix_fs_ntfs.h"
#include "kernel.h"
#include "ixk_string.h"
#include "ix_utf8.h"
#include "ixk_video.h"

#define NTFS_SECBUF_SIZE 4096

typedef struct {
  int      active;
  int      is_dir;
  uint64_t mft_ref;
  uint64_t file_size;
  uint64_t cur_offset;
  ntfs_run_t runs[NTFS_RUNLIST_MAX];
  int      run_count;
} ntfs_file_handle_t;

typedef struct {
  int      active;
  uint64_t dir_mft_ref;
  uint8_t* idx_buf;
  uint32_t idx_buf_size;
  uint32_t idx_pos;
  int      phase;
  ntfs_run_t alloc_runs[NTFS_RUNLIST_MAX];
  int      alloc_run_count;
  uint32_t alloc_block_size;
  int      alloc_run_idx;
  uint64_t alloc_vcn;
} ntfs_dir_handle_t;

static ntfs_file_handle_t g_ntfs_files[NTFS_MAX_HANDLES];
static ntfs_dir_handle_t  g_ntfs_dirs[NTFS_MAX_HANDLES];
static uint8_t g_ntfs_secbuf[NTFS_SECBUF_SIZE];
static uint8_t g_ntfs_mftbuf[NTFS_MFT_BUF_SIZE];
static uint8_t g_ntfs_attrbuf[NTFS_MFT_BUF_SIZE];
static uint8_t g_ntfs_idxbuf[NTFS_MFT_BUF_SIZE];
static uint8_t g_ntfs_databuf[NTFS_SECBUF_SIZE];

static uint16_t nrd16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t nrd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t nrd64(const uint8_t* p) {
  return (uint64_t)nrd32(p) | ((uint64_t)nrd32(p + 4) << 32);
}

/* "Сектор ФС" = bytes_per_sector (512..4096); хранилище -- 512 байт. */
static int ntfs_read_sectors(ntfs_context_t* ctx, uint64_t sector_offset, uint32_t count, void* buf) {
  uint32_t k = ctx->bytes_per_sector / 512u;
  if (!k) k = 1;
  return ixk_storage_read(ctx->part_lba + sector_offset * k, count * k, buf);
}

static int ntfs_read_cluster(ntfs_context_t* ctx, uint64_t lcn, void* buf) {
  uint64_t lba = lcn * (uint64_t)ctx->sectors_per_cluster;
  return ntfs_read_sectors(ctx, lba, ctx->sectors_per_cluster, buf);
}

static void ntfs_apply_fixup(uint8_t* record, uint32_t record_size, uint16_t sector_size) {
  ntfs_mft_entry_t* mft = (ntfs_mft_entry_t*)record;
  uint16_t usa_offset = nrd16((const uint8_t*)&mft->update_seq_offset);
  uint16_t usa_count = nrd16((const uint8_t*)&mft->update_seq_count);

  if (usa_offset >= record_size || usa_count < 2) return;

  const uint8_t* usa = record + usa_offset;
  uint16_t sig = nrd16(usa);

  for (uint16_t i = 1; i < usa_count && (uint64_t)i * (uint64_t)sector_size <= (uint64_t)record_size; i++) {
    uint32_t pos = (uint32_t)i * (uint32_t)sector_size - 2;
    if (pos + 1 >= record_size) break;
    uint16_t stored = nrd16(record + pos);
    if (stored != sig) break;
    record[pos] = usa[i * 2];
    record[pos + 1] = usa[i * 2 + 1];
  }
}

int ntfs_mount(ntfs_context_t* ctx, uint64_t part_lba, uint64_t part_sectors) {
  ix_memset(ctx, 0, sizeof(ntfs_context_t));
  ix_memset(g_ntfs_files, 0, sizeof(g_ntfs_files));
  ix_memset(g_ntfs_dirs, 0, sizeof(g_ntfs_dirs));

  if (!ixk_storage_read(part_lba, 1, g_ntfs_secbuf))
    return 0;

  const ntfs_boot_sector_t* bs = (const ntfs_boot_sector_t*)g_ntfs_secbuf;

  if (ix_memcmp(bs->oem, "NTFS    ", 8) != 0)
    return 0;

  ctx->bytes_per_sector = nrd16((const uint8_t*)&bs->bytes_per_sector);
  ctx->sectors_per_cluster = bs->sectors_per_cluster;

  if (ctx->bytes_per_sector == 0 || ctx->sectors_per_cluster == 0)
    return 0;

  ctx->cluster_size = (uint32_t)ctx->bytes_per_sector * (uint32_t)ctx->sectors_per_cluster;

  int8_t cpf = bs->clusters_per_mft_record;
  if (cpf > 0) {
    ctx->mft_record_size = (uint32_t)cpf * ctx->cluster_size;
  } else {
    ctx->mft_record_size = 1u << (uint32_t)(-cpf);
  }

  if (ctx->mft_record_size == 0 || ctx->mft_record_size > NTFS_MFT_BUF_SIZE)
    ctx->mft_record_size = 1024;

  ctx->mft_lcn = nrd64((const uint8_t*)&bs->mft_lcn);
  ctx->mft_lba = ctx->mft_lcn * (uint64_t)ctx->sectors_per_cluster;
  ctx->part_lba = part_lba;
  ctx->part_sectors = part_sectors;

  uint32_t secs_per_record = ctx->mft_record_size / ctx->bytes_per_sector;
  if (secs_per_record == 0) secs_per_record = 1;
  if (!ntfs_read_sectors(ctx, ctx->mft_lba, secs_per_record, g_ntfs_mftbuf))
    return 0;

  ntfs_apply_fixup(g_ntfs_mftbuf, ctx->mft_record_size, ctx->bytes_per_sector);

  ntfs_mft_entry_t* mft0 = (ntfs_mft_entry_t*)g_ntfs_mftbuf;
  if (nrd32((const uint8_t*)&mft0->magic) != NTFS_MFT_MAGIC)
    return 0;

  ctx->valid = 1;
  return 1;
}

static int ntfs_read_mft_record(ntfs_context_t* ctx, uint64_t mft_ref, uint8_t* buf) {
  uint64_t record_num = mft_ref & 0x0000FFFFFFFFFFFFull;
  uint32_t secs_per_record = ctx->mft_record_size / ctx->bytes_per_sector;
  if (secs_per_record == 0) secs_per_record = 1;

  if (record_num > 0xFFFFFFFFu / (uint64_t)ctx->mft_record_size) return 0;
  uint64_t byte_offset = record_num * (uint64_t)ctx->mft_record_size;
  uint64_t sector = ctx->mft_lba + byte_offset / ctx->bytes_per_sector;
  if (sector < ctx->mft_lba) return 0;

  if (!ntfs_read_sectors(ctx, sector, secs_per_record, buf))
    return 0;

  ntfs_apply_fixup(buf, ctx->mft_record_size, ctx->bytes_per_sector);

  ntfs_mft_entry_t* entry = (ntfs_mft_entry_t*)buf;
  if (nrd32((const uint8_t*)&entry->magic) != NTFS_MFT_MAGIC)
    return 0;

  return 1;
}

static const uint8_t* ntfs_find_attr(const uint8_t* record, uint32_t record_size, uint32_t attr_type) {
  const ntfs_mft_entry_t* mft = (const ntfs_mft_entry_t*)record;
  uint16_t off = nrd16((const uint8_t*)&mft->first_attr_offset);

  while ((uint32_t)off + 16u <= record_size) {
    const uint8_t* attr = record + off;
    uint32_t type = nrd32(attr);
    if (type == NTFS_ATTR_END || type == 0) break;
    uint32_t len = nrd32(attr + 4);
    if (len < 16 || off + len > record_size) break;
    if (type == attr_type) return attr;
    off += len;
  }
  return 0;
}

static int ntfs_decode_runs(const uint8_t* run_data, uint32_t run_data_len, ntfs_run_t* runs, int max_runs) {
  int count = 0;
  uint32_t pos = 0;
  int64_t prev_offset = 0;

  while (pos < run_data_len && count < max_runs) {
    uint8_t header = run_data[pos++];
    if (header == 0) break;

    uint8_t len_size = header & 0x0Fu;
    uint8_t off_size = (header >> 4) & 0x0Fu;

    if (len_size == 0 || pos + len_size + off_size > run_data_len)
      break;

    uint64_t length = 0;
    for (uint8_t i = 0; i < len_size; i++)
      length |= (uint64_t)run_data[pos + i] << (i * 8);
    pos += len_size;

    int64_t offset = 0;
    if (off_size > 0) {
      for (uint8_t i = 0; i < off_size; i++)
        offset |= (int64_t)((uint64_t)run_data[pos + i]) << (i * 8);
      if (run_data[pos + off_size - 1] & 0x80) {
        for (uint8_t i = off_size; i < 8; i++)
          offset |= (int64_t)0xFFull << (i * 8);
      }
      pos += off_size;
    }

    runs[count].offset = prev_offset + offset;
    runs[count].length = length;
    prev_offset = runs[count].offset;
    count++;
  }

  return count;
}

static int ntfs_get_data_runs(const uint8_t* record, uint32_t record_size, ntfs_run_t* runs, int max_runs, uint64_t* out_size) {
  const uint8_t* attr = ntfs_find_attr(record, record_size, NTFS_ATTR_DATA);
  if (!attr) return 0;

  uint8_t non_res = attr[8];
  if (!non_res) {
    uint32_t val_len = nrd32(attr + 16);
    uint16_t val_off = nrd16(attr + 20);
    (void)val_len;
    (void)val_off;
    if (out_size) *out_size = val_len;
    return 0;
  }

  uint16_t run_offset = nrd16(attr + 32);
  uint64_t real_size = nrd64(attr + 48);
  if (out_size) *out_size = real_size;

  uint32_t attr_len = nrd32(attr + 4);
  if (run_offset >= attr_len) return 0;

  return ntfs_decode_runs(attr + run_offset, attr_len - run_offset, runs, max_runs);
}

static int ntfs_read_resident_data(const uint8_t* record, uint32_t record_size, void* buf, uint32_t buf_size, uint32_t* out_len) {
  const uint8_t* attr = ntfs_find_attr(record, record_size, NTFS_ATTR_DATA);
  if (!attr) return 0;

  uint8_t non_res = attr[8];
  if (non_res) return 0;

  uint32_t val_len = nrd32(attr + 16);
  uint16_t val_off = nrd16(attr + 20);

  if (val_off + val_len > nrd32(attr + 4)) return 0;
  uint32_t to_copy = val_len;
  if (to_copy > buf_size) to_copy = buf_size;

  ix_memcpy(buf, attr + val_off, to_copy);
  if (out_len) *out_len = to_copy;
  return 1;
}

static int ntfs_read_data_runs(ntfs_context_t* ctx, const ntfs_run_t* runs, int run_count,
                               uint64_t offset, void* buf, uint32_t size) {
  uint8_t* dst = (uint8_t*)buf;
  uint32_t done = 0;
  uint64_t vcn_offset = 0;

  for (int i = 0; i < run_count && done < size; i++) {
    uint64_t run_bytes = runs[i].length * ctx->cluster_size;

    if (offset >= vcn_offset + run_bytes) {
      vcn_offset += run_bytes;
      continue;
    }

    uint64_t in_run = offset - vcn_offset;
    uint64_t avail = run_bytes - in_run;
    uint32_t to_read = (uint32_t)avail;
    if (to_read > size - done) to_read = size - done;

    uint64_t start_lcn = (uint64_t)runs[i].offset;
    uint64_t start_byte = start_lcn * ctx->cluster_size + in_run;
    uint64_t start_sector = start_byte / ctx->bytes_per_sector;
    uint32_t byte_off = (uint32_t)(start_byte % ctx->bytes_per_sector);

    while (to_read > 0) {
      if (!ntfs_read_sectors(ctx, start_sector, 1, g_ntfs_databuf))
        return (int)done;

      uint32_t chunk = ctx->bytes_per_sector - byte_off;
      if (chunk > to_read) chunk = to_read;

      ix_memcpy(dst + done, g_ntfs_databuf + byte_off, chunk);
      done += chunk;
      to_read -= chunk;
      offset += chunk;
      start_sector++;
      byte_off = 0;
    }

    vcn_offset += run_bytes;
  }

  return (int)done;
}

static int ntfs_get_filename(const uint8_t* record, uint32_t record_size, char* out, int cap) {
  const ntfs_mft_entry_t* mft = (const ntfs_mft_entry_t*)record;
  uint16_t off = nrd16((const uint8_t*)&mft->first_attr_offset);

  const uint8_t* best = 0;
  uint8_t best_type = 0xFF;

  while ((uint32_t)off + 16u <= record_size) {
    const uint8_t* attr = record + off;
    uint32_t type = nrd32(attr);
    if (type == NTFS_ATTR_END || type == 0) break;
    uint32_t len = nrd32(attr + 4);
    if (len < 16 || off + len > record_size) break;

    if (type == NTFS_ATTR_FILE_NAME && !attr[8]) {
      uint16_t voff = nrd16(attr + 20);
      uint32_t vlen = nrd32(attr + 16);
      if (voff + vlen <= len && vlen >= 66 + 2) {
        const uint8_t* fn = attr + voff;
        uint8_t name_type = fn[65];
        if (name_type == 1 || best_type == 0xFF || (name_type == 0 && best_type == 2)) {
          best = fn;
          best_type = name_type;
        }
      }
    }
    off += len;
  }

  if (!best) return 0;

  uint8_t name_len = best[64];
  const uint8_t* name_chars = best + 66;
  int j = 0;
  j = ix_utf16le_to_utf8(name_chars, (int)name_len, out, cap);
  out[j] = 0;
  return 1;
}

static uint64_t ntfs_get_file_size(const uint8_t* record, uint32_t record_size) {
  const uint8_t* attr = ntfs_find_attr(record, record_size, NTFS_ATTR_DATA);
  if (!attr) return 0;
  if (!attr[8]) return (uint64_t)nrd32(attr + 16);
  return nrd64(attr + 48);
}

static int ntfs_is_directory(const uint8_t* record) {
  const ntfs_mft_entry_t* mft = (const ntfs_mft_entry_t*)record;
  return (nrd16((const uint8_t*)&mft->flags) & NTFS_MFT_DIRECTORY) ? 1 : 0;
}

static uint64_t ntfs_win_to_unix_ns(uint64_t win_time) {
  if (win_time < 116444736000000000ull) return 0;
  return (win_time - 116444736000000000ull) * 100ull;
}

static int ntfs_get_timestamps(const uint8_t* record, uint32_t record_size,
                               uint64_t* created, uint64_t* modified, uint64_t* accessed) {
  const uint8_t* attr = ntfs_find_attr(record, record_size, NTFS_ATTR_STD_INFO);
  if (!attr || attr[8]) return 0;

  uint16_t voff = nrd16(attr + 20);
  uint32_t vlen = nrd32(attr + 16);
  if (vlen < 48) return 0;

  const uint8_t* si = attr + voff;
  if (created) *created = ntfs_win_to_unix_ns(nrd64(si + 0));
  if (modified) *modified = ntfs_win_to_unix_ns(nrd64(si + 8));
  if (accessed) *accessed = ntfs_win_to_unix_ns(nrd64(si + 24));
  return 1;
}

static int ntfs_strcmp_ci(const char* a, const char* b) {
  return ix_utf8_casecmp(a, b);
}

static uint64_t ntfs_lookup_in_index_root(ntfs_context_t* ctx, const uint8_t* record,
                                          uint32_t record_size, const char* target) {
  const uint8_t* attr = ntfs_find_attr(record, record_size, NTFS_ATTR_INDEX_ROOT);
  if (!attr || attr[8]) return (uint64_t)-1;

  uint16_t voff = nrd16(attr + 20);
  uint32_t vlen = nrd32(attr + 16);
  if (vlen < 32) return (uint64_t)-1;

  const uint8_t* ir = attr + voff;
  uint32_t entries_off = nrd32(ir + 16);
  uint32_t entries_size = nrd32(ir + 20);

  uint32_t pos = entries_off;
  while (pos + 16 <= entries_off + entries_size) {
    const uint8_t* ie = ir + pos;
    uint16_t ie_len = nrd16(ie + 8);
    uint16_t ie_content_len = nrd16(ie + 10);
    uint32_t ie_flags = nrd32(ie + 12);

    if (ie_len < 16) break;
    if (ie_flags & NTFS_INDEX_ENTRY_LAST) break;

    if (ie_content_len >= 66 + 2) {
      const uint8_t* fn = ie + 16;
      uint8_t name_len = fn[64];
      char name[NTFS_NAME_MAX];
      int j = 0;
      const uint8_t* name_chars = fn + 66;
      j = ix_utf16le_to_utf8(name_chars, (int)name_len, name, NTFS_NAME_MAX);
      name[j] = 0;

      if (ntfs_strcmp_ci(name, target) == 0) {
        return nrd64(ie) & 0x0000FFFFFFFFFFFFull;
      }
    }

    pos += ie_len;
  }

  return (uint64_t)-1;
}

static uint64_t ntfs_lookup_in_dir(ntfs_context_t* ctx, uint64_t dir_mft_ref, const char* name) {
  if (!ntfs_read_mft_record(ctx, dir_mft_ref, g_ntfs_attrbuf))
    return (uint64_t)-1;

  uint64_t result = ntfs_lookup_in_index_root(ctx, g_ntfs_attrbuf, ctx->mft_record_size, name);
  if (result != (uint64_t)-1) return result;

  const uint8_t* alloc_attr = ntfs_find_attr(g_ntfs_attrbuf, ctx->mft_record_size, NTFS_ATTR_INDEX_ALLOC);
  if (!alloc_attr || !alloc_attr[8]) return (uint64_t)-1;

  ntfs_run_t runs[NTFS_RUNLIST_MAX];
  uint16_t run_off = nrd16(alloc_attr + 32);
  uint32_t attr_len = nrd32(alloc_attr + 4);
  if (run_off >= attr_len) return (uint64_t)-1;

  int run_count = ntfs_decode_runs(alloc_attr + run_off, attr_len - run_off, runs, NTFS_RUNLIST_MAX);
  if (run_count <= 0) return (uint64_t)-1;

  uint64_t alloc_size = nrd64(alloc_attr + 40);
  uint32_t block_size = ctx->cluster_size;

  const uint8_t* ir_attr = ntfs_find_attr(g_ntfs_attrbuf, ctx->mft_record_size, NTFS_ATTR_INDEX_ROOT);
  if (ir_attr && !ir_attr[8]) {
    uint16_t ir_voff = nrd16(ir_attr + 20);
    const uint8_t* ir = ir_attr + ir_voff;
    uint32_t ias = nrd32(ir + 8);
    if (ias > 0 && ias <= 65536) block_size = ias;
  }

  for (uint64_t off = 0; off < alloc_size; off += block_size) {
    if (block_size > sizeof(g_ntfs_idxbuf)) break;

    int rd = ntfs_read_data_runs(ctx, runs, run_count, off, g_ntfs_idxbuf, block_size);
    if (rd < 48) continue;

    if (ix_memcmp(g_ntfs_idxbuf, "INDX", 4) != 0) continue;

    ntfs_apply_fixup(g_ntfs_idxbuf, block_size, ctx->bytes_per_sector);

    uint32_t entries_off = nrd32(g_ntfs_idxbuf + 24) + 24;
    uint32_t entries_size = nrd32(g_ntfs_idxbuf + 28);

    uint32_t pos = entries_off;
    while (pos + 16 <= entries_off + entries_size && pos + 16 <= block_size) {
      const uint8_t* ie = g_ntfs_idxbuf + pos;
      uint16_t ie_len = nrd16(ie + 8);
      uint16_t ie_content_len = nrd16(ie + 10);
      uint32_t ie_flags = nrd32(ie + 12);

      if (ie_len < 16) break;
      if (ie_flags & NTFS_INDEX_ENTRY_LAST) break;

      if (ie_content_len >= 66 + 2) {
        const uint8_t* fn = ie + 16;
        uint8_t fname_len = fn[64];
        char fname[NTFS_NAME_MAX];
        int j = 0;
        const uint8_t* fname_chars = fn + 66;
        j = ix_utf16le_to_utf8(fname_chars, (int)fname_len, fname, NTFS_NAME_MAX);
        fname[j] = 0;

        if (ntfs_strcmp_ci(fname, name) == 0) {
          return nrd64(ie) & 0x0000FFFFFFFFFFFFull;
        }
      }

      pos += ie_len;
    }
  }

  return (uint64_t)-1;
}

static int ntfs_lookup_path(ntfs_context_t* ctx, const char* path, uint64_t* out_mft_ref) {
  uint64_t cur = NTFS_FILE_ROOT;

  if (!path || !path[0] || (path[0] == '/' && !path[1])) {
    *out_mft_ref = NTFS_FILE_ROOT;
    return 1;
  }

  char component[NTFS_NAME_MAX];
  int pi = 0;

  while (path[pi]) {
    if (path[pi] == '/' || path[pi] == '\\') { pi++; continue; }

    int ci = 0;
    while (path[pi] && path[pi] != '/' && path[pi] != '\\' && ci < NTFS_NAME_MAX - 1)
      component[ci++] = path[pi++];
    component[ci] = 0;
    if (!component[0]) continue;

    uint64_t ref = ntfs_lookup_in_dir(ctx, cur, component);
    if (ref == (uint64_t)-1) return 0;
    cur = ref;
  }

  *out_mft_ref = cur;
  return 1;
}

int ntfs_open(ntfs_context_t* ctx, const char* path, uint32_t flags) {
  (void)flags;
  if (!ctx || !ctx->valid) return -1;

  uint64_t mft_ref;
  if (!ntfs_lookup_path(ctx, path, &mft_ref)) return -1;

  if (!ntfs_read_mft_record(ctx, mft_ref, g_ntfs_mftbuf))
    return -1;

  if (ntfs_is_directory(g_ntfs_mftbuf)) return -1;

  int h = -1;
  for (int i = 0; i < NTFS_MAX_HANDLES; i++) {
    if (!g_ntfs_files[i].active) { h = i; break; }
  }
  if (h < 0) return -1;

  ix_memset(&g_ntfs_files[h], 0, sizeof(ntfs_file_handle_t));
  g_ntfs_files[h].active = 1;
  g_ntfs_files[h].mft_ref = mft_ref;
  g_ntfs_files[h].file_size = ntfs_get_file_size(g_ntfs_mftbuf, ctx->mft_record_size);
  g_ntfs_files[h].cur_offset = 0;
  g_ntfs_files[h].is_dir = 0;

  g_ntfs_files[h].run_count = ntfs_get_data_runs(g_ntfs_mftbuf, ctx->mft_record_size,
                                                   g_ntfs_files[h].runs, NTFS_RUNLIST_MAX, 0);

  return h;
}

int ntfs_read(ntfs_context_t* ctx, int handle, void* buffer, uint32_t size) {
  if (handle < 0 || handle >= NTFS_MAX_HANDLES) return -1;
  ntfs_file_handle_t* fh = &g_ntfs_files[handle];
  if (!fh->active || fh->is_dir || !ctx || !ctx->valid) return -1;

  if (fh->cur_offset >= fh->file_size) return 0;

  uint32_t remaining = size;
  if (fh->cur_offset + remaining > fh->file_size)
    remaining = (uint32_t)(fh->file_size - fh->cur_offset);

  if (fh->run_count == 0) {
    /* Резидентные данные (малые файлы): читаем значение целиком и отдаём
     * срез с учётом смещения (раньше всегда копировалось с начала). */
    static uint8_t resbuf[NTFS_MFT_BUF_SIZE];
    if (!ntfs_read_mft_record(ctx, fh->mft_ref, g_ntfs_mftbuf))
      return -1;
    uint32_t total = 0;
    if (!ntfs_read_resident_data(g_ntfs_mftbuf, ctx->mft_record_size, resbuf, sizeof(resbuf), &total))
      return -1;
    if (fh->cur_offset >= total) return 0;
    uint32_t n = total - (uint32_t)fh->cur_offset;
    if (n > remaining) n = remaining;
    ix_memcpy(buffer, resbuf + (uint32_t)fh->cur_offset, n);
    fh->cur_offset += n;
    return (int)n;
  }

  int rd = ntfs_read_data_runs(ctx, fh->runs, fh->run_count, fh->cur_offset, buffer, remaining);
  if (rd > 0) fh->cur_offset += (uint64_t)rd;
  return rd;
}

/*
 * Чтение диапазона файла по абсолютному смещению (для VFS).  Открытый
 * дескриптор последнего файла кэшируется: раньше каждый вызов заново
 * открывал файл и "проматывал" offset чтением с начала по 512 байт --
 * чтение файла кусками было квадратичным.  Том read-only, поэтому
 * инвалидировать кэш нечем.
 */
#define NTFS_RC_PATH_MAX 256
static char                 g_ntfs_rc_path[NTFS_RC_PATH_MAX];
static int                  g_ntfs_rc_h = -1;
static const ntfs_context_t* g_ntfs_rc_ctx = 0;

int ntfs_read_at(ntfs_context_t* ctx, const char* path,
                 uint32_t offset, void* buffer, uint32_t size) {
  if (!ctx || !ctx->valid || !path || !buffer) return -1;
  if (size == 0) return 0;

  int plen = 0;
  while (path[plen]) plen++;

  int h = -1;
  int cached = 0;
  if (plen < NTFS_RC_PATH_MAX && g_ntfs_rc_h >= 0 && g_ntfs_rc_ctx == ctx &&
      g_ntfs_files[g_ntfs_rc_h].active &&
      ix_memcmp(g_ntfs_rc_path, path, (uint32_t)plen + 1u) == 0) {
    h = g_ntfs_rc_h;
    cached = 1;
  }

  if (h < 0) {
    if (g_ntfs_rc_h >= 0) {            /* освободить прежний кэшированный файл */
      ntfs_close(ctx, g_ntfs_rc_h);
      g_ntfs_rc_h = -1;
    }
    h = ntfs_open(ctx, path, 0);
    if (h < 0) return -1;
    if (plen < NTFS_RC_PATH_MAX) {
      ix_memcpy(g_ntfs_rc_path, path, (uint32_t)plen + 1u);
      g_ntfs_rc_h = h;
      g_ntfs_rc_ctx = ctx;
      cached = 1;
    }
  }

  ntfs_file_handle_t* fh = &g_ntfs_files[h];
  int rd = 0;
  if ((uint64_t)offset < fh->file_size) {
    fh->cur_offset = offset;
    rd = ntfs_read(ctx, h, buffer, size);
  }
  if (!cached) ntfs_close(ctx, h);
  return rd;
}

int ntfs_stat(ntfs_context_t* ctx, const char* path, ix_file_info_t* out) {
  if (!ctx || !ctx->valid || !out) return -1;
  ix_memset(out, 0, sizeof(*out));

  uint64_t mft_ref;
  if (!ntfs_lookup_path(ctx, path, &mft_ref)) return -1;
  if (!ntfs_read_mft_record(ctx, mft_ref, g_ntfs_mftbuf)) return -1;

  out->size = ntfs_get_file_size(g_ntfs_mftbuf, ctx->mft_record_size);
  if (ntfs_is_directory(g_ntfs_mftbuf)) out->attrs |= IX_FS_ATTR_DIR;
  ntfs_get_timestamps(g_ntfs_mftbuf, ctx->mft_record_size,
                      &out->created_ns, &out->modified_ns, &out->accessed_ns);
  return 0;
}

int ntfs_close(ntfs_context_t* ctx, int handle) {
  (void)ctx;
  if (handle < 0 || handle >= NTFS_MAX_HANDLES) return -1;
  g_ntfs_files[handle].active = 0;
  return 0;
}

int ntfs_opendir(ntfs_context_t* ctx, const char* path) {
  if (!ctx || !ctx->valid) return -1;

  uint64_t mft_ref;
  if (!ntfs_lookup_path(ctx, path, &mft_ref)) return -1;

  if (!ntfs_read_mft_record(ctx, mft_ref, g_ntfs_mftbuf))
    return -1;

  if (!ntfs_is_directory(g_ntfs_mftbuf)) return -1;

  int h = -1;
  for (int i = 0; i < NTFS_MAX_HANDLES; i++) {
    if (!g_ntfs_dirs[i].active) { h = i; break; }
  }
  if (h < 0) return -1;

  ix_memset(&g_ntfs_dirs[h], 0, sizeof(ntfs_dir_handle_t));
  g_ntfs_dirs[h].active = 1;
  g_ntfs_dirs[h].dir_mft_ref = mft_ref;
  g_ntfs_dirs[h].phase = 0;
  g_ntfs_dirs[h].idx_pos = 0;

  const uint8_t* alloc_attr = ntfs_find_attr(g_ntfs_mftbuf, ctx->mft_record_size, NTFS_ATTR_INDEX_ALLOC);
  if (alloc_attr && alloc_attr[8]) {
    uint16_t run_off = nrd16(alloc_attr + 32);
    uint32_t attr_len = nrd32(alloc_attr + 4);
    if (run_off < attr_len) {
      g_ntfs_dirs[h].alloc_run_count = ntfs_decode_runs(
        alloc_attr + run_off, attr_len - run_off,
        g_ntfs_dirs[h].alloc_runs, NTFS_RUNLIST_MAX);

      const uint8_t* ir_attr = ntfs_find_attr(g_ntfs_mftbuf, ctx->mft_record_size, NTFS_ATTR_INDEX_ROOT);
      if (ir_attr && !ir_attr[8]) {
        uint16_t ir_voff = nrd16(ir_attr + 20);
        g_ntfs_dirs[h].alloc_block_size = nrd32(ir_attr + ir_voff + 8);
        if (g_ntfs_dirs[h].alloc_block_size == 0)
          g_ntfs_dirs[h].alloc_block_size = ctx->cluster_size;
      } else {
        g_ntfs_dirs[h].alloc_block_size = ctx->cluster_size;
      }
    }
  }

  return h;
}

int ntfs_readdir(ntfs_context_t* ctx, int dir_handle, ix_dirent_t* out) {
  if (dir_handle < 0 || dir_handle >= NTFS_MAX_HANDLES) return -1;
  ntfs_dir_handle_t* dh = &g_ntfs_dirs[dir_handle];
  if (!dh->active || !ctx || !ctx->valid) return -1;
  ix_memset(out, 0, sizeof(*out));

  if (dh->phase == 0) {
    if (!ntfs_read_mft_record(ctx, dh->dir_mft_ref, g_ntfs_attrbuf))
      return -1;

    const uint8_t* ir_attr = ntfs_find_attr(g_ntfs_attrbuf, ctx->mft_record_size, NTFS_ATTR_INDEX_ROOT);
    if (!ir_attr || ir_attr[8]) {
      dh->phase = 1;
      dh->alloc_run_idx = 0;
      dh->alloc_vcn = 0;
      dh->idx_pos = 0;
      goto phase1;
    }

    uint16_t ir_voff = nrd16(ir_attr + 20);
    uint32_t ir_vlen = nrd32(ir_attr + 16);
    const uint8_t* ir = ir_attr + ir_voff;

    uint32_t entries_off = nrd32(ir + 16);
    uint32_t entries_size = nrd32(ir + 20);

    while (dh->idx_pos + 16 <= entries_off + entries_size) {
      uint32_t pos = entries_off + dh->idx_pos;
      if (pos + 16 > ir_vlen) break;
      const uint8_t* ie = ir + pos;
      uint16_t ie_len = nrd16(ie + 8);
      uint16_t ie_content_len = nrd16(ie + 10);
      uint32_t ie_flags = nrd32(ie + 12);

      if (ie_len < 16) break;
      dh->idx_pos += ie_len;

      if (ie_flags & NTFS_INDEX_ENTRY_LAST) break;

      if (ie_content_len >= 66 + 2) {
        const uint8_t* fn = ie + 16;
        uint8_t name_type = fn[65];
        if (name_type == 2) continue;

        uint8_t name_len = fn[64];
        char name[NTFS_NAME_MAX];
        int j = 0;
        const uint8_t* name_chars = fn + 66;
        j = ix_utf16le_to_utf8(name_chars, (int)name_len, name, NTFS_NAME_MAX);
        name[j] = 0;

        if (name[0] == '$') continue;
        if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) continue;

        uint64_t ref = nrd64(ie) & 0x0000FFFFFFFFFFFFull;
        uint32_t fn_flags = nrd32(fn + 56);

        ixk_string_copy_n(out->name, name, 256);
        out->is_dir = (fn_flags & 0x10000000u) ? 1 : 0;
        out->size = nrd64(fn + 48);
        out->info.size = out->size;
        if (out->is_dir) out->info.attrs = IX_FS_ATTR_DIR;
        out->info.created_ns = ntfs_win_to_unix_ns(nrd64(fn + 8));
        out->info.modified_ns = ntfs_win_to_unix_ns(nrd64(fn + 16));
        return 1;
      }
    }

    dh->phase = 1;
    dh->alloc_run_idx = 0;
    dh->alloc_vcn = 0;
    dh->idx_pos = 0;
  }

phase1:
  if (dh->phase == 1 && dh->alloc_run_count > 0) {
    uint32_t block_size = dh->alloc_block_size;
    if (block_size > sizeof(g_ntfs_idxbuf)) block_size = (uint32_t)sizeof(g_ntfs_idxbuf);
    if (block_size == 0) return 0;

    while (dh->alloc_run_idx < dh->alloc_run_count) {
      uint64_t run_len = dh->alloc_runs[dh->alloc_run_idx].length;
      uint64_t run_bytes = run_len * ctx->cluster_size;

      while (dh->alloc_vcn * ctx->cluster_size < run_bytes ||
             dh->idx_pos > 0) {
        uint64_t total_off = 0;
        for (int r = 0; r < dh->alloc_run_idx; r++)
          total_off += dh->alloc_runs[r].length * ctx->cluster_size;
        total_off += dh->alloc_vcn * ctx->cluster_size;

        if (dh->idx_pos == 0) {
          int rd = ntfs_read_data_runs(ctx, dh->alloc_runs, dh->alloc_run_count,
                                       total_off, g_ntfs_idxbuf, block_size);
          if (rd < 48) {
            dh->alloc_vcn += block_size / ctx->cluster_size;
            if (dh->alloc_vcn * ctx->cluster_size >= run_bytes) break;
            continue;
          }

          if (ix_memcmp(g_ntfs_idxbuf, "INDX", 4) != 0) {
            dh->alloc_vcn += block_size / ctx->cluster_size;
            if (dh->alloc_vcn * ctx->cluster_size >= run_bytes) break;
            continue;
          }

          ntfs_apply_fixup(g_ntfs_idxbuf, block_size, ctx->bytes_per_sector);
          dh->idx_pos = nrd32(g_ntfs_idxbuf + 24) + 24;
        }

        uint32_t entries_size = nrd32(g_ntfs_idxbuf + 28);
        uint32_t entries_end = nrd32(g_ntfs_idxbuf + 24) + 24 + entries_size;

        while (dh->idx_pos + 16 <= entries_end && dh->idx_pos + 16 <= block_size) {
          const uint8_t* ie = g_ntfs_idxbuf + dh->idx_pos;
          uint16_t ie_len = nrd16(ie + 8);
          uint16_t ie_content_len = nrd16(ie + 10);
          uint32_t ie_flags = nrd32(ie + 12);

          if (ie_len < 16) { dh->idx_pos = entries_end; break; }
          dh->idx_pos += ie_len;

          if (ie_flags & NTFS_INDEX_ENTRY_LAST) break;

          if (ie_content_len >= 66 + 2) {
            const uint8_t* fn = ie + 16;
            uint8_t name_type = fn[65];
            if (name_type == 2) continue;

            uint8_t name_len = fn[64];
            char name[NTFS_NAME_MAX];
            int j = 0;
            const uint8_t* name_chars = fn + 66;
            j = ix_utf16le_to_utf8(name_chars, (int)name_len, name, NTFS_NAME_MAX);
            name[j] = 0;

            if (name[0] == '$') continue;
            if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) continue;

            uint32_t fn_flags = nrd32(fn + 56);

            ixk_string_copy_n(out->name, name, 256);
            out->is_dir = (fn_flags & 0x10000000u) ? 1 : 0;
            out->size = nrd64(fn + 48);
            out->info.size = out->size;
            if (out->is_dir) out->info.attrs = IX_FS_ATTR_DIR;
            out->info.created_ns = ntfs_win_to_unix_ns(nrd64(fn + 8));
            out->info.modified_ns = ntfs_win_to_unix_ns(nrd64(fn + 16));
            return 1;
          }
        }

        dh->idx_pos = 0;
        dh->alloc_vcn += block_size / ctx->cluster_size;
        if (block_size < ctx->cluster_size)
          dh->alloc_vcn++;
        if (dh->alloc_vcn * ctx->cluster_size >= run_bytes) break;
      }

      dh->alloc_run_idx++;
      dh->alloc_vcn = 0;
      dh->idx_pos = 0;
    }
  }

  return 0;
}

int ntfs_closedir(ntfs_context_t* ctx, int dir_handle) {
  (void)ctx;
  if (dir_handle < 0 || dir_handle >= NTFS_MAX_HANDLES) return -1;
  g_ntfs_dirs[dir_handle].active = 0;
  return 0;
}
