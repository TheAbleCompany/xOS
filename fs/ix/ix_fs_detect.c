// ix_fs_detect.c -- см. ix_fs_detect.h.
//
// Порядок проверок важен: сначала форматы с однозначной сигнатурой в
// начале раздела (NTFS/exFAT/BitLocker/ReFS/LUKS/XFS/APFS), затем FAT
// (по BPB с полной валидацией), затем ext (суперблок @1024 с валидацией),
// затем остальные сигнатуры по смещениям.

#include "ix_fs_detect.h"
#include "kernel.h"
#include "ixk_string.h"

#define PROBE_HEAD_SECTORS 8          // первые 4 КиБ раздела

static uint8_t g_head[PROBE_HEAD_SECTORS * 512];
static uint8_t g_aux[512];

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p)
{ return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

static int pow2(uint32_t v) { return v && !(v & (v - 1)); }

static int mem_eq(const uint8_t *a, const char *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}

const char *ix_fs_type_name(ix_fs_type_t t)
{
    switch (t) {
        case IX_FS_TYPE_FAT12:     return "FAT12";
        case IX_FS_TYPE_FAT16:     return "FAT16";
        case IX_FS_TYPE_FAT32:     return "FAT32";
        case IX_FS_TYPE_NTFS:      return "NTFS";
        case IX_FS_TYPE_EXT:       return "ext";
        case IX_FS_TYPE_EXFAT:     return "exFAT";
        case IX_FS_TYPE_BITLOCKER: return "BitLocker";
        case IX_FS_TYPE_LUKS:      return "LUKS";
        case IX_FS_TYPE_SWAP:      return "swap";
        case IX_FS_TYPE_XFS:       return "XFS";
        case IX_FS_TYPE_BTRFS:     return "Btrfs";
        case IX_FS_TYPE_F2FS:      return "F2FS";
        case IX_FS_TYPE_LVM:       return "LVM";
        case IX_FS_TYPE_MDRAID:    return "RAID";
        case IX_FS_TYPE_ISO9660:   return "ISO9660";
        case IX_FS_TYPE_HFSPLUS:   return "HFS+";
        case IX_FS_TYPE_APFS:      return "APFS";
        case IX_FS_TYPE_REFS:      return "ReFS";
        case IX_FS_TYPE_EMPTY:     return "empty";
        default:                   return "unknown";
    }
}

int ix_fs_type_mountable(ix_fs_type_t t)
{
    return t == IX_FS_TYPE_FAT12 || t == IX_FS_TYPE_FAT16 ||
           t == IX_FS_TYPE_FAT32 || t == IX_FS_TYPE_NTFS  ||
           t == IX_FS_TYPE_EXT;
}

static void copy_label(char *dst, const uint8_t *src, int n)
{
    int k = 0;
    for (int i = 0; i < n && k < 19; i++) {
        uint8_t c = src[i];
        if (c == 0) break;
        dst[k++] = (c < 0x20 || c > 0x7E) ? '?' : (char)c;
    }
    while (k > 0 && dst[k - 1] == ' ') k--;
    dst[k] = 0;
    if (k == 7 && mem_eq((const uint8_t *)dst, "NO NAME", 7))   // метка по умолчанию
        dst[0] = 0;
}

// Чтение секторов раздела с проверкой границ.
static int prb_read(uint64_t part_lba, uint64_t part_sectors,
                    uint64_t rel, uint32_t count, void *buf)
{
    if (part_sectors && rel + count > part_sectors)
        return 0;
    return ixk_storage_read(part_lba + rel, count, buf);
}

// ------------------------------------------------------------------ NTFS
static int probe_ntfs(const uint8_t *s)
{
    if (!mem_eq(s + 3, "NTFS    ", 8)) return 0;
    if (s[510] != 0x55 || s[511] != 0xAA) return 0;
    uint32_t bps = le16(s + 11);
    if (bps < 512 || bps > 4096 || !pow2(bps)) return 0;
    uint32_t spc = s[13];
    if (spc >= 0xF4) spc = 1u << (256 - spc);          // 2^(256-v)
    if (!pow2(spc) || spc > 128) return 0;
    if (le64(s + 40) == 0) return 0;                    // total sectors
    if (le64(s + 48) == 0) return 0;                    // $MFT LCN
    return 1;
}

// ----------------------------------------------------------------- exFAT
static int probe_exfat(const uint8_t *s)
{
    if (!mem_eq(s + 3, "EXFAT   ", 8)) return 0;
    if (s[510] != 0x55 || s[511] != 0xAA) return 0;
    for (int i = 11; i < 64; i++) if (s[i]) return 0;   // MustBeZero
    if (s[108] < 9 || s[108] > 12) return 0;
    return 1;
}

// ------------------------------------------------------------------- FAT
// Возвращает IX_FS_TYPE_FAT12/16/32 или UNKNOWN.
static ix_fs_type_t probe_fat(const uint8_t *s, char *label)
{
    if (s[510] != 0x55 || s[511] != 0xAA) return IX_FS_TYPE_UNKNOWN;
    if (s[0] != 0xEB && s[0] != 0xE9)     return IX_FS_TYPE_UNKNOWN;

    uint32_t bps  = le16(s + 11);
    uint32_t spc  = s[13];
    uint32_t rsvd = le16(s + 14);
    uint32_t nfat = s[16];
    uint32_t root = le16(s + 17);
    uint32_t t16  = le16(s + 19);
    uint32_t media = s[21];
    uint32_t fsz16 = le16(s + 22);
    uint32_t t32  = le32(s + 32);
    uint32_t fsz32 = le32(s + 36);

    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return IX_FS_TYPE_UNKNOWN;
    if (!pow2(spc) || spc > 128)       return IX_FS_TYPE_UNKNOWN;
    if (rsvd == 0)                     return IX_FS_TYPE_UNKNOWN;
    if (nfat < 1 || nfat > 2)          return IX_FS_TYPE_UNKNOWN;
    if (media != 0xF0 && media < 0xF8) return IX_FS_TYPE_UNKNOWN;

    uint32_t total = t16 ? t16 : t32;
    uint32_t fsz   = fsz16 ? fsz16 : fsz32;
    if (!total || !fsz)                return IX_FS_TYPE_UNKNOWN;

    int is32 = (fsz16 == 0);
    if (is32) {
        // FAT32: корневого каталога фикс. размера нет, версия 0.0, root >= 2.
        if (root != 0)                 return IX_FS_TYPE_UNKNOWN;
        if (le16(s + 42) != 0)         return IX_FS_TYPE_UNKNOWN;
        if (le32(s + 44) < 2)          return IX_FS_TYPE_UNKNOWN;
    } else {
        if (root == 0)                 return IX_FS_TYPE_UNKNOWN;
    }

    uint32_t root_secs = (root * 32u + bps - 1u) / bps;
    uint32_t first_data = rsvd + nfat * fsz + root_secs;
    if (first_data >= total)           return IX_FS_TYPE_UNKNOWN;

    uint32_t clusters = (total - first_data) / spc;

    if (is32) {
        copy_label(label, s + 71, 11);
        return IX_FS_TYPE_FAT32;
    }
    copy_label(label, s + 43, 11);
    return clusters < 4085 ? IX_FS_TYPE_FAT12 : IX_FS_TYPE_FAT16;
}

// ------------------------------------------------------------------- ext
#define EXT_MAGIC 0xEF53

static int probe_ext(const uint8_t *sb, ix_fs_info_t *o)
{
    if (le16(sb + 56) != EXT_MAGIC) return 0;

    uint32_t log_bs   = le32(sb + 24);
    uint32_t inodes   = le32(sb + 0);
    uint32_t blocks   = le32(sb + 4);
    uint32_t first_db = le32(sb + 20);
    uint32_t bpg      = le32(sb + 32);
    uint32_t ipg      = le32(sb + 40);
    uint32_t rev      = le32(sb + 76);
    uint16_t state    = le16(sb + 58);

    if (log_bs > 6)                      return 0;     // блок до 64 КиБ
    if (!inodes || !blocks)              return 0;
    if (first_db > 1)                    return 0;
    uint32_t bsize = 1024u << log_bs;
    if (!bpg || bpg > bsize * 8u)        return 0;
    if (!ipg)                            return 0;
    if (rev > 1)                         return 0;
    if (state & ~0x7u)                   return 0;
    if (rev >= 1) {
        uint32_t isz = le16(sb + 88);
        if (!pow2(isz) || isz < 128 || isz > bsize) return 0;
    }

    uint32_t compat = 0, incompat = 0, rocompat = 0;
    if (rev >= 1) {
        compat   = le32(sb + 92);
        incompat = le32(sb + 96);
        rocompat = le32(sb + 100);
    }

    // Версия: фичи ext4 > журнал ext3 > ext2.
    int ext4 = (incompat & (0x40u | 0x80u | 0x200u)) ||
               (rocompat & (0x8u | 0x20u | 0x40u | 0x400u));
    o->ext_ver = ext4 ? 4 : ((compat & 0x4u) ? 3 : 2);
    copy_label(o->label, sb + 120, 16);

    // incompat, которых драйвер не понимает: монтировать нельзя вовсе.
    //   0x1 compression, 0x8 journal_dev, 0x10 meta_bg, 0x1000 dirdata,
    //   0x8000 inline_data, 0x10000 encrypt.
    uint32_t bad = incompat & (0x1u | 0x8u | 0x10u | 0x1000u | 0x8000u | 0x10000u);
    if (bad) {
        o->mountable = 0;
        if (bad & 0x8000u)      o->note = "ext inline_data not supported";
        else if (bad & 0x10000u) o->note = "ext encryption not supported";
        else if (bad & 0x10u)   o->note = "ext meta_bg not supported";
        else if (bad & 0x8u)    o->note = "ext external journal device";
        else                    o->note = "ext feature not supported";
        return 1;
    }

    o->mountable = 1;

    // Только чтение: том не закрыт чисто (нужен replay журнала), либо
    // фичи, которые запись драйвера не поддерживает (bigalloc, quota, ...).
    if (incompat & 0x4u) {
        o->read_only = 1; o->note = "ext needs journal recovery (unclean)";
    } else if (incompat & (0x100u | 0x4000u | 0x20000u)) {
        o->read_only = 1; o->note = "ext feature (mmp/largedir/casefold): read-only";
    } else if (rocompat & (0x100u | 0x200u | 0x1000u | 0x2000u | 0x8000u)) {
        o->read_only = 1; o->note = "ext feature (quota/bigalloc/ro/project/verity): read-only";
    } else if (!(state & 0x1u)) {
        o->read_only = 1; o->note = "ext not cleanly unmounted";
    }
#ifndef IX_EXT_ALLOW_CSUM_WRITE
    /*
     * metadata_csum (0x400): драйвер записи не пересчитывает crc32c
     * суперблока/дескрипторов групп/битмапов/inode.  Linux после такой
     * записи видит "bad checksum" (e2fsck: "Checksum errors in
     * superblock").  На реальном диске это порча -- монтируем только
     * для чтения.  Старое поведение: -DIX_EXT_ALLOW_CSUM_WRITE.
     */
    if (!o->read_only && (rocompat & 0x400u)) {
        o->read_only = 1;
        o->note = "ext metadata_csum: checksums not maintained -- read-only";
    }
#endif
    return 1;
}

// ------------------------------------------------------------------ main
int ix_fs_probe(uint64_t part_lba, uint64_t part_sectors, ix_fs_info_t *out)
{
    ix_memset(out, 0, sizeof(*out));
    out->type = IX_FS_TYPE_UNKNOWN;
    out->name = "unknown";

    uint32_t n = PROBE_HEAD_SECTORS;
    if (part_sectors && part_sectors < n) n = (uint32_t)part_sectors;
    if (n == 0) return 0;

    ix_memset(g_head, 0, sizeof(g_head));
    if (!ixk_storage_read(part_lba, n, g_head))
        return 0;
    const uint8_t *s = g_head;

    // 1. Однозначные сигнатуры в начале.
    if (probe_ntfs(s))   { out->type = IX_FS_TYPE_NTFS;  out->mountable = 1; goto done; }
    if (probe_exfat(s))  { out->type = IX_FS_TYPE_EXFAT; out->note = "exFAT: no driver"; goto done; }
    if (mem_eq(s + 3, "-FVE-FS-", 8) ||
        (s[0xA0] == 0x3B && s[0xA1] == 0xD6 && s[0xA2] == 0x67 && s[0xA3] == 0x49 &&
         s[0xA4] == 0x29 && s[0xA5] == 0x2E && s[0xA6] == 0xD8 && s[0xA7] == 0x4A)) {
        out->type = IX_FS_TYPE_BITLOCKER; out->note = "encrypted volume"; goto done;
    }
    if (mem_eq(s + 3, "ReFS\0\0\0\0", 8)) { out->type = IX_FS_TYPE_REFS; out->note = "ReFS: no driver"; goto done; }
    if (mem_eq(s, "LUKS\xBA\xBE", 6) || mem_eq(s, "SKUL\xBA\xBE", 6)) {
        out->type = IX_FS_TYPE_LUKS; out->note = "encrypted volume"; goto done;
    }
    if (mem_eq(s, "XFSB", 4))        { out->type = IX_FS_TYPE_XFS;  out->note = "XFS: no driver"; goto done; }
    if (mem_eq(s + 32, "NXSB", 4))   { out->type = IX_FS_TYPE_APFS; out->note = "APFS: no driver"; goto done; }

    // 2. FAT.
    {
        ix_fs_type_t ft = probe_fat(s, out->label);
        if (ft != IX_FS_TYPE_UNKNOWN) { out->type = ft; out->mountable = 1; goto done; }
    }

    // 3. ext.
    if (n >= 4 && probe_ext(s + 1024, out)) { out->type = IX_FS_TYPE_EXT; goto done; }

    // 4. Остальные сигнатуры.
    if (n >= 4) {
        if (le32(s + 1024 + 0) == 0xF2F52010u) {
            out->type = IX_FS_TYPE_F2FS; out->note = "F2FS: no driver"; goto done;
        }
        if ((s[1024] == 'H' && s[1025] == '+') || (s[1024] == 'H' && s[1025] == 'X')) {
            out->type = IX_FS_TYPE_HFSPLUS; out->note = "HFS+: no driver"; goto done;
        }
    }
    for (uint32_t sec = 0; sec < 4 && sec < n; sec++) {
        const uint8_t *p = s + sec * 512;
        if (mem_eq(p, "LABELONE", 8) && mem_eq(p + 24, "LVM2 001", 8)) {
            out->type = IX_FS_TYPE_LVM; out->note = "LVM physical volume"; goto done;
        }
    }
    if (n >= 8 && (mem_eq(s + 4086, "SWAPSPACE2", 10) || mem_eq(s + 4086, "SWAP-SPACE", 10))) {
        out->type = IX_FS_TYPE_SWAP; out->note = "swap"; goto done;
    }
    if (le32(s) == 0xA92B4EFCu) {                        // md v1.1 (начало)
        out->type = IX_FS_TYPE_MDRAID; out->note = "RAID member"; goto done;
    }

    // 5. Дальние сигнатуры (отдельные чтения, только если раздел достаточно велик).
    if (prb_read(part_lba, part_sectors, 8, 1, g_aux) && le32(g_aux) == 0xA92B4EFCu) {   // md v1.2 @4096
        out->type = IX_FS_TYPE_MDRAID; out->note = "RAID member"; goto done;
    }
    if (prb_read(part_lba, part_sectors, 64, 1, g_aux) && mem_eq(g_aux + 1, "CD001", 5)) { // ISO9660 @32769
        out->type = IX_FS_TYPE_ISO9660; out->note = "ISO9660: no driver"; goto done;
    }
    if (prb_read(part_lba, part_sectors, 128, 1, g_aux) && mem_eq(g_aux + 64, "_BHRfS_M", 8)) { // btrfs @65600
        out->type = IX_FS_TYPE_BTRFS; out->note = "Btrfs: no driver"; goto done;
    }

    // 6. Пусто?
    {
        int zero = 1;
        for (uint32_t i = 0; i < n * 512u; i++)
            if (g_head[i]) { zero = 0; break; }
        if (zero) out->type = IX_FS_TYPE_EMPTY;
    }

done:
    if (out->type == IX_FS_TYPE_EXT) {
        static const char *nm[] = { "ext", "ext", "ext2", "ext3", "ext4" };
        out->name = nm[out->ext_ver];
    } else {
        out->name = ix_fs_type_name(out->type);
    }
    return 1;
}

ix_fs_type_t ix_fs_detect(uint64_t part_lba)
{
    ix_fs_info_t info;
    if (!ix_fs_probe(part_lba, 0, &info))
        return IX_FS_TYPE_UNKNOWN;
    return info.mountable ? info.type : IX_FS_TYPE_UNKNOWN;
}
