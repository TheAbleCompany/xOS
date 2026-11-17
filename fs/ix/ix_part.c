// ix_part.c -- определение типа ФС + сканер таблиц разделов.
//
// r13-storage: то, что раньше было classify_fat()+MBR-only-сканером в
// ix_disk_fat.c, здесь разделено и расширено:
//   - ix_probe_fs(): FAT12/16/32 (BPB), NTFS (OEM-подпись + валидные поля
//     boot-сектора), EXT2/3/4 (magic 0xEF53 на lba+2);
//   - ix_part_scan(): GPT (header "EFI PART" на LBA1, до 128 записей),
//     MBR (4 primary + цепочка расширенных 05/0F/85), суперфлоппи
//     (ФС с LBA0 при пустой таблице разделов).
//
// Все чтения идут через ixk_storage_read() -- т.е. через активно
// выбранное устройство слоя storage.

#include "ix_part.h"
#include "ixk_storage.h"

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* little-endian читатели                                             */
/* ------------------------------------------------------------------ */

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32l(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64l(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static int qmemcmp(const uint8_t *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != (uint8_t)b[i])
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* ix_probe_fs                                                        */
/* ------------------------------------------------------------------ */

/*
 * NTFS: boot-сектор
 *   [3..10]  = "NTFS    "
 *   [11..12] = bytes_per_sector (>= 128, степень двойки)
 *   [13]     = sectors_per_cluster (степень двойки)
 *   [0x40]   = clusters_per_mft_record (int8, <0 => размер = 2^-c)
 *   [40..47] = total_sectors (8 байт)
 *   [16..17] = reserved (0, разница с FAT)
 */
static ix_fs_type_t probe_ntfs(uint64_t lba)
{
    uint8_t sec[512];
    if (!ixk_storage_read(lba, 1, sec))
        return IX_FS_TYPE_UNKNOWN;

    if (!qmemcmp(sec + 3, "NTFS    ", 8))
        return IX_FS_TYPE_UNKNOWN;

    uint32_t bps  = rd16(sec + 11);
    uint8_t  spc  = sec[13];
    int8_t   cpmr = (int8_t)sec[0x40];   /* clusters per MFT record */

    if (bps < 128 || bps > 4096 || (bps & (bps - 1)) != 0)
        return IX_FS_TYPE_UNKNOWN;
    if (spc == 0 || (spc & (spc - 1)) != 0)
        return IX_FS_TYPE_UNKNOWN;
    if (cpmr == 0)
        return IX_FS_TYPE_UNKNOWN;
    if (cpmr < 0) {
        /* размер записи MFT = 2^(-cpmr); > 4КиБ наш буфер не держит */
        if (cpmr < -12)
            return IX_FS_TYPE_UNKNOWN;
    } else {
        /* размер = cpmr * cluster_size; > 4КиБ не держит */
        uint64_t cl = (uint64_t)bps * (uint64_t)spc;
        if (cpmr > 7 || (uint64_t)((uint32_t)cpmr) * cl > 4096)
            return IX_FS_TYPE_UNKNOWN;
    }

    uint64_t total = rd64l(sec + 40);
    if (total == 0)
        return IX_FS_TYPE_UNKNOWN;

    return IX_FS_TYPE_NTFS;
}

/*
 * FAT: BPB-валидация + классификация FAT12/16/32 по числу кластеров.
 * (Перенесено из classify_fat() в раннем ix_disk_fat.c.)
 */
static ix_fs_type_t probe_fat(uint64_t part_lba)
{
    uint8_t sec[512];
    if (!ixk_storage_read(part_lba, 1, sec))
        return IX_FS_TYPE_UNKNOWN;
    if (sec[510] != 0x55 || sec[511] != 0xAA)
        return IX_FS_TYPE_UNKNOWN;

    uint16_t bytes_per_sector    = (uint16_t)rd16(sec + 11);
    uint8_t  sectors_per_cluster = sec[13];
    uint16_t reserved_sectors    = (uint16_t)rd16(sec + 14);
    uint8_t  num_fats            = sec[16];
    uint16_t root_entry_count    = (uint16_t)rd16(sec + 17);
    uint16_t tot16               = (uint16_t)rd16(sec + 19);
    uint16_t fat_sz16            = (uint16_t)rd16(sec + 22);
    uint32_t tot32               = rd32l(sec + 32);
    uint32_t fat_sz32            = rd32l(sec + 36);

    if (bytes_per_sector == 0 || sectors_per_cluster == 0)
        return IX_FS_TYPE_UNKNOWN;

    uint32_t fat_size  = fat_sz16 ? (uint32_t)fat_sz16 : fat_sz32;
    uint32_t total_sec = tot16 ? (uint32_t)tot16 : tot32;
    if (fat_size == 0 || total_sec == 0)
        return IX_FS_TYPE_UNKNOWN;

    uint32_t root_dir_sectors =
        (((uint32_t)root_entry_count * 32u) + (bytes_per_sector - 1u)) / bytes_per_sector;
    uint32_t first_data_sector =
        reserved_sectors + ((uint32_t)num_fats * fat_size) + root_dir_sectors;

    if (total_sec <= first_data_sector)
        return IX_FS_TYPE_UNKNOWN;

    uint32_t data_clusters =
        (total_sec - first_data_sector) / (uint32_t)sectors_per_cluster;

    if (data_clusters < 4085)  return IX_FS_TYPE_FAT12;
    if (data_clusters < 65525) return IX_FS_TYPE_FAT16;
    return IX_FS_TYPE_FAT32;
}

/*
 * EXT2/3/4: суперблок лежит в секторе lba+2 (offset 1024),
 * magic 0xEF53 в поле s_magic (смещение 56).  EXT4 в 64KiB-блоках может
 * иметь магию иначе, но FAT-совместимые раскладки нас не интересуют --
 * главное отловить "обычные" ext-разделы.
 */
static ix_fs_type_t probe_ext(uint64_t lba)
{
    uint8_t sb[512];
    if (!ixk_storage_read(lba + 2, 1, sb))
        return IX_FS_TYPE_UNKNOWN;

    uint16_t magic = rd16(sb + 56);
    if (magic != 0xEF53)
        return IX_FS_TYPE_UNKNOWN;

    uint32_t blk_size = rd32l(sb + 24);      /* s_log_block_size */
    if (blk_size > 6)
        return IX_FS_TYPE_UNKNOWN;

    uint32_t inodes = rd32l(sb + 0);
    uint32_t blocks = rd32l(sb + 4);
    if (inodes == 0 || blocks == 0)
        return IX_FS_TYPE_UNKNOWN;

    return IX_FS_TYPE_EXT;
}

ix_fs_type_t ix_probe_fs(uint64_t lba)
{
    /* NTFS проверяем первым: его BPB-зона частично совпадает с FAT. */
    ix_fs_type_t t = probe_ntfs(lba);
    if (t != IX_FS_TYPE_UNKNOWN)
        return t;

    t = probe_fat(lba);
    if (t != IX_FS_TYPE_UNKNOWN)
        return t;

    return probe_ext(lba);
}

/* ------------------------------------------------------------------ */
/* ix_part_scan                                                       */
/* ------------------------------------------------------------------ */

/* Типы расширённых разделов (EBR-цепочка). */
static int is_ext_chain(uint8_t t)
{
    return t == 0x05 || t == 0x0F || t == 0x85;
}

static int add_part(ix_part_t *out, int max, int n,
                    uint64_t lba, uint64_t sectors,
                    uint64_t total_sectors, uint8_t bootable)
{
    if (n >= max)
        return n;
    if (lba == 0 && sectors == 0)
        return n;                                  /* пустая запись      */
    if (total_sectors && lba + sectors > total_sectors)
        return n;                                  /* за краем диска     */

    out[n].lba      = lba;
    out[n].sectors  = sectors;
    out[n].type     = IX_FS_TYPE_UNKNOWN;
    out[n].bootable = bootable;
    return n + 1;
}

int ix_part_scan(ix_part_t *out, int max)
{
    int n = 0;

    if (!out || max <= 0 || !ixk_storage_dev_present())
        return 0;

    const ixk_storage_dev_t *di = ixk_storage_dev_info(ixk_storage_cur_dev());
    uint64_t total_sectors = di ? di->sectors : 0;

    uint8_t s0[512];
    if (!ixk_storage_read(0, 1, s0))
        return 0;

    /* ---- GPT (защитный MBR логотип c type 0xEE виден ниже и
     *           перехватывается прямо здесь) ---- */
    uint8_t ghdr[512];
    if (ixk_storage_read(1, 1, ghdr) &&
        qmemcmp(ghdr, "EFI PART", 8)) {

        uint64_t ent_lba = rd64l(ghdr + 72);
        uint32_t nent    = rd32l(ghdr + 80);
        uint32_t esz     = rd32l(ghdr + 84);

        if (ent_lba && nent && esz && esz <= 512 && nent <= 256) {
            for (uint32_t i = 0; i < nent && n < max; i++) {
                uint64_t file_pos = (uint64_t)i * esz;
                uint64_t sec_off  = ent_lba + (file_pos >> 9);
                uint32_t in_off   = (uint32_t)(file_pos & 0x1FF);

                uint8_t ebuf[1024];
                if (!ixk_storage_read(sec_off, 2, ebuf))
                    break;
                const uint8_t *ent = ebuf + in_off;

                /* type GUID != 0 -> живая запись */
                int nonzero = 0;
                for (int k = 0; k < 16; k++)
                    if (ent[k])
                        { nonzero = 1; break; }
                if (!nonzero)
                    continue;

                uint64_t first = rd64l(ent + 32);
                uint64_t last  = rd64l(ent + 40);
                if (last < first)
                    continue;

                n = add_part(out, max, n, first, last - first + 1,
                             total_sectors, 0);
            }
        }
        if (n)
            return n;
        /* GPT-заголовок был, но записей не вытащили -- дальше уже
         * нет смысла верить защитному MBR: отдадим пустой результат. */
        return n;
    }

    /* ---- MBR ---- */
    if (s0[510] == 0x55 && s0[511] == 0xAA) {

        /* primary-разделы */
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = s0 + 446 + (uint32_t)i * 16;
            uint8_t  type   = e[4];
            uint64_t lba    = rd32l(e + 8);
            uint64_t secs   = rd32l(e + 12);
            uint8_t  boot   = (e[0] == 0x80);

            if (type == 0xEE)
                continue;              /* защитный MBR -- GPT выше/ниже */
            if (is_ext_chain(type)) {
                /* расширенный раздел: обойти EBR-цепочку дальше */
                uint64_t ext_base = lba;
                uint64_t ebr_cur  = lba;
                int      guard    = 32;

                while (guard--) {
                    uint8_t ebr[512];
                    if (!ixk_storage_read(ebr_cur, 1, ebr))
                        break;
                    if (ebr[510] != 0x55 || ebr[511] != 0xAA)
                        break;

                    const uint8_t *le = ebr + 446;
                    uint8_t lt = le[4];
                    uint32_t rel0 = rd32l(le + 8);
                    uint32_t len0 = rd32l(le + 12);

                    /* данные-запись */
                    if (lt != 0 && !is_ext_chain(lt) && len0)
                        n = add_part(out, max, n,
                                     ebr_cur + rel0, len0,
                                     total_sectors, 0);
                    if (n >= max)
                        break;

                    const uint8_t *ne = ebr + 446 + 16;
                    uint8_t nt = ne[4];
                    uint32_t relief = rd32l(ne + 8);

                    if (!is_ext_chain(nt) || relief == 0)
                        break;
                    ebr_cur = ext_base + relief;
                }
                continue;
            }
            if (type && secs)
                n = add_part(out, max, n, lba, secs, total_sectors, boot);
        }
    }

    if (n)
        return n;

    /* ---- суперфлоппи: ФС прямо с LBA0 при пустой таблице ---- */
    ix_fs_type_t t = ix_probe_fs(0);
    if (t != IX_FS_TYPE_UNKNOWN)
        n = add_part(out, max, n, 0, total_sectors, total_sectors, 0);

    return n;
}
