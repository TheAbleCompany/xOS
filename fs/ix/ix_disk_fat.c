// ix_disk_fat.c -- "disk0" в VFS: реальный раздел физического диска
// (FAT12/16/32/NTFS/EXT2-4, read-only).
//
// r13-storage: раньше файл занимался ТОЛЬКО FAT и ТОЛЬКО на legacy
// ATA primary master, по байту типа из MBR.  Теперь:
//   - устройство выбирается из слоя ixk_storage (ATA x4 + AHCI);
//   - таблицы разделов разбирает ix_part_scan() (GPT/MBR/расширенные/
//     суперфлоппи), тип ФС определяет ix_probe_fs();
//   - поверх общего API ix-FS модулей смонтирована первая поддавшаяся
//     ФС -- через переключатель bd_* на fat_* / ntfs_* / ext_*.
//
// Запись пока не поддерживается: у ix_fs_*.c нет fat_write/fat_create/
// fat_seek/fat_truncate.  Когда появятся -- раскомментируйте ветки write.

#include "ix_disk_fat.h"
#include "ix_fs_fat.h"
#include "ix_fs_ntfs.h"
#include "ix_fs_ext.h"
#include "ix_part.h"
#include "ixstd_fs.h"
#include "kernel.h"
#include "ixk_string.h"
#include "ixk_storage.h"

#include "../../lib/common.h"
#include "../../mm/kheap.h"
#include "../fs.h"

#define malloc kmalloc
#define free   kfree

static fat_context_t  g_fat_ctx;
static ntfs_context_t g_ntfs_ctx;
static ext_context_t  g_ext_ctx;

static ix_fs_type_t  g_type    = IX_FS_TYPE_UNKNOWN;
static int           g_mounted = 0;

static fs_node_t     g_disk_root;
static struct dirent g_dirent;

#define DISKFAT_NODE_CACHE 128
static fs_node_t *g_node_cache[DISKFAT_NODE_CACHE];
static int        g_node_cache_n = 0;

/* ------------------------------------------------------------------ */
/* Диспatcher Personalix-FS API (fat_ / ntfs_ / ext_ по g_type)        */
/* ------------------------------------------------------------------ */

static int bd_mount(uint64_t lba, uint64_t sectors, ix_fs_type_t t)
{
    switch (t) {
    case IX_FS_TYPE_FAT12:
    case IX_FS_TYPE_FAT16:
    case IX_FS_TYPE_FAT32:
        return fat_mount(&g_fat_ctx, lba, sectors, t) ? 1 : 0;
    case IX_FS_TYPE_NTFS:
        return ntfs_mount(&g_ntfs_ctx, lba, sectors) ? 1 : 0;
    case IX_FS_TYPE_EXT:
        return ext_mount(&g_ext_ctx, lba, sectors) ? 1 : 0;
    default:
        return 0;
    }
}

static const char *bd_type_name(ix_fs_type_t t)
{
    switch (t) {
    case IX_FS_TYPE_FAT12: return "FAT12";
    case IX_FS_TYPE_FAT16: return "FAT16";
    case IX_FS_TYPE_FAT32: return "FAT32";
    case IX_FS_TYPE_NTFS:  return "NTFS";
    case IX_FS_TYPE_EXT:   return "EXT";
    default:               return "?";
    }
}

static int bd_open(const char *path)
{
    switch (g_type) {
    case IX_FS_TYPE_FAT12: case IX_FS_TYPE_FAT16: case IX_FS_TYPE_FAT32:
        return fat_open(&g_fat_ctx, path, 0);
    case IX_FS_TYPE_NTFS:
        return ntfs_open(&g_ntfs_ctx, path, 0);
    case IX_FS_TYPE_EXT:
        return ext_open(&g_ext_ctx, path, 0);
    default:
        return -1;
    }
}

static int bd_read(int h, void *buffer, uint32_t size)
{
    switch (g_type) {
    case IX_FS_TYPE_FAT12: case IX_FS_TYPE_FAT16: case IX_FS_TYPE_FAT32:
        return fat_read(&g_fat_ctx, h, buffer, size);
    case IX_FS_TYPE_NTFS:
        return ntfs_read(&g_ntfs_ctx, h, buffer, size);
    case IX_FS_TYPE_EXT:
        return ext_read(&g_ext_ctx, h, buffer, size);
    default:
        return -1;
    }
}

static int bd_close(int h)
{
    switch (g_type) {
    case IX_FS_TYPE_FAT12: case IX_FS_TYPE_FAT16: case IX_FS_TYPE_FAT32:
        return fat_close(&g_fat_ctx, h);
    case IX_FS_TYPE_NTFS:
        return ntfs_close(&g_ntfs_ctx, h);
    case IX_FS_TYPE_EXT:
        return ext_close(&g_ext_ctx, h);
    default:
        return -1;
    }
}

static int bd_stat(const char *path, ix_file_info_t *out)
{
    switch (g_type) {
    case IX_FS_TYPE_FAT12: case IX_FS_TYPE_FAT16: case IX_FS_TYPE_FAT32:
        return fat_stat(&g_fat_ctx, path, out) == 0 ? 1 : 0;
    case IX_FS_TYPE_NTFS:
        return ntfs_stat(&g_ntfs_ctx, path, out) == 0 ? 1 : 0;
    case IX_FS_TYPE_EXT:
        return ext_stat(&g_ext_ctx, path, out) == 0 ? 1 : 0;
    default:
        return 0;
    }
}

static int bd_opendir(const char *path)
{
    switch (g_type) {
    case IX_FS_TYPE_FAT12: case IX_FS_TYPE_FAT16: case IX_FS_TYPE_FAT32:
        return fat_opendir(&g_fat_ctx, path);
    case IX_FS_TYPE_NTFS:
        return ntfs_opendir(&g_ntfs_ctx, path);
    case IX_FS_TYPE_EXT:
        return ext_opendir(&g_ext_ctx, path);
    default:
        return -1;
    }
}

static int bd_readdir(int dh, ix_dirent_t *ent)
{
    switch (g_type) {
    case IX_FS_TYPE_FAT12: case IX_FS_TYPE_FAT16: case IX_FS_TYPE_FAT32:
        return fat_readdir(&g_fat_ctx, dh, ent);
    case IX_FS_TYPE_NTFS:
        return ntfs_readdir(&g_ntfs_ctx, dh, ent);
    case IX_FS_TYPE_EXT:
        return ext_readdir(&g_ext_ctx, dh, ent);
    default:
        return -1;
    }
}

static int bd_closedir(int dh)
{
    switch (g_type) {
    case IX_FS_TYPE_FAT12: case IX_FS_TYPE_FAT16: case IX_FS_TYPE_FAT32:
        return fat_closedir(&g_fat_ctx, dh);
    case IX_FS_TYPE_NTFS:
        return ntfs_closedir(&g_ntfs_ctx, dh);
    case IX_FS_TYPE_EXT:
        return ext_closedir(&g_ext_ctx, dh);
    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* VFS-обвязка (неизменна с r12)                                      */
/* ------------------------------------------------------------------ */

static void diskfat_debug_puts(const char *s)
{
    debug_puts(s);
}

static void diskfat_debug_putnum(uint64_t v)
{
    debug_putnum(v);
}

static void diskfat_make_path(char *dst, u32int dstsz, const char *name)
{
    dst[0] = '/';
    ixk_string_copy_n(dst + 1, name, dstsz - 1);
}

static u32int diskfat_read(fs_node_t *node, u32int offset, u32int size, u8int *buffer)
{
    if (!g_mounted || !node || !buffer || size == 0)
        return 0;

    char path[130];
    diskfat_make_path(path, sizeof(path), node->name);

    int h = bd_open(path);
    if (h < 0)
        return 0;

    /*
     * В ix_fs_*.c нет seek, а *_open всегда начинает с 0.
     * Поэтому offset > 0 реализуем чтением в корзину.
     */
    if (offset > 0) {
        uint8_t scratch[512];
        uint32_t remaining = offset;
        while (remaining > 0) {
            uint32_t chunk = remaining > sizeof(scratch) ? sizeof(scratch) : remaining;
            int got = bd_read(h, scratch, chunk);
            if (got <= 0) {
                bd_close(h);
                return 0;
            }
            remaining -= (uint32_t)got;
        }
    }

    int got = bd_read(h, buffer, size);
    bd_close(h);

    return got > 0 ? (u32int)got : 0;
}

static fs_node_t *diskfat_finddir(fs_node_t *node, char *name)
{
    if (!g_mounted || node != &g_disk_root || !name || !name[0])
        return 0;

    for (int i = 0; i < g_node_cache_n; i++)
        if (strcmp(g_node_cache[i]->name, name) == 0)
            return g_node_cache[i];

    char path[130];
    diskfat_make_path(path, sizeof(path), name);

    ix_file_info_t info;
    if (!bd_stat(path, &info))
        return 0;

    if (g_node_cache_n >= DISKFAT_NODE_CACHE)
        return 0;

    fs_node_t *n = (fs_node_t *)malloc(sizeof(fs_node_t));
    if (!n)
        return 0;

    strncpy(n->name, name, sizeof(n->name) - 1);
    n->name[sizeof(n->name) - 1] = 0;
    n->mask = n->uid = n->gid = n->inode = 0;
    n->program_type = 0;
    n->impl = FS_IMPL_DISKFAT;
    n->ptr  = 0;

    if (info.attrs & IX_FS_ATTR_DIR) {
        n->length  = 0;
        n->flags   = FS_DIRECTORY;
        n->read    = 0;
        n->write   = 0;
        n->readdir = 0;
        n->finddir = 0;
    } else {
        n->length  = (u32int)info.size;
        n->flags   = FS_FILE;
        n->read    = &diskfat_read;
        n->write   = 0;    /* read-only, пока нет *_write */
        n->readdir = 0;
        n->finddir = 0;
    }
    n->open  = 0;
    n->close = 0;

    g_node_cache[g_node_cache_n++] = n;
    return n;
}

static struct dirent *diskfat_readdir(fs_node_t *node, u32int index)
{
    if (!g_mounted || node != &g_disk_root)
        return 0;

    int dh = bd_opendir("/");
    if (dh < 0)
        return 0;

    ix_dirent_t ent;
    u32int i = 0;
    int rc = 0;
    for (;;) {
        rc = bd_readdir(dh, &ent);
        if (rc <= 0) break;
        if (i == index) break;
        i++;
    }
    bd_closedir(dh);

    if (rc <= 0)
        return 0;

    ixk_string_copy_n(g_dirent.name, ent.name, sizeof(g_dirent.name));
    g_dirent.ino  = index;
    g_dirent.type = ent.is_dir ? FS_DIRECTORY : FS_FILE;
    return &g_dirent;
}

/* ------------------------------------------------------------------ */
/* Монтирование                                                       */
/* ------------------------------------------------------------------ */

int diskfat_init(void)
{
    g_mounted = 0;
    g_node_cache_n = 0;

    /* Слой накопителей: legacy ATA (4 устройства) + AHCI/SATA. */
    ixk_storage_init();
    if (!ixk_storage_dev_present()) {
        diskfat_debug_puts("DISKFAT: no block devices\n");
        return 0;
    }

    int ndev = ixk_storage_dev_count();

    for (int d = 0; d < ndev && !g_mounted; d++) {
        const ixk_storage_dev_t *di = ixk_storage_dev_info(d);

        ixk_storage_set_dev(d);

        diskfat_debug_puts("DISKFAT: probing ");
        diskfat_debug_puts(di->label);
        diskfat_debug_puts("...\n");

        ix_part_t parts[IX_PART_MAX];
        int np = ix_part_scan(parts, IX_PART_MAX);

        diskfat_debug_puts("DISKFAT: partitions found: ");
        diskfat_debug_putnum((uint64_t)np);
        diskfat_debug_puts("\n");

        for (int i = 0; i < np; i++) {
            diskfat_debug_puts("DISKFAT:   #");
            diskfat_debug_putnum((uint64_t)i);
            diskfat_debug_puts(" lba=");
            diskfat_debug_putnum(parts[i].lba);
            diskfat_debug_puts(" secs=");
            diskfat_debug_putnum(parts[i].sectors);
            diskfat_debug_puts(" boot=");
            diskfat_debug_putnum((uint64_t)parts[i].bootable);
            diskfat_debug_puts("\n");
        }

        /*
         * Приоритет: загрузочный MBR-раздел, потом остальные.
         * GPT-разделы bootable-флага не имеют -- идут в порядке записей.
         */
        int order[IX_PART_MAX];
        int on = 0;
        for (int i = 0; i < np; i++)
            if (parts[i].bootable) order[on++] = i;
        for (int i = 0; i < np; i++)
            if (!parts[i].bootable) order[on++] = i;

        for (int k = 0; k < on && !g_mounted; k++) {
            ix_part_t *p = &parts[order[k]];

            ix_fs_type_t t = ix_probe_fs(p->lba);
            diskfat_debug_puts("DISKFAT: probe LBA ");
            diskfat_debug_putnum(p->lba);
            diskfat_debug_puts(" -> type ");
            diskfat_debug_putnum((uint64_t)t);
            diskfat_debug_puts("\n");
            if (t == IX_FS_TYPE_UNKNOWN)
                continue;

            if (!bd_mount(p->lba, p->sectors, t)) {
                diskfat_debug_puts("DISKFAT: ");
                diskfat_debug_puts(bd_type_name(t));
                diskfat_debug_puts(" mount failed on partition at LBA ");
                diskfat_debug_putnum(p->lba);
                diskfat_debug_puts("\n");
                continue;
            }

            g_type    = t;
            g_mounted = 1;

            diskfat_debug_puts("DISKFAT: mounted ");
            diskfat_debug_puts(bd_type_name(t));
            diskfat_debug_puts(" partition (LBA ");
            diskfat_debug_putnum(p->lba);
            diskfat_debug_puts(", ");
            diskfat_debug_putnum(p->sectors);
            diskfat_debug_puts(" sectors) as disk0\n");
        }
    }

    if (!g_mounted)
        return 0;

    strcpy(g_disk_root.name, "disk0");
    g_disk_root.mask = g_disk_root.uid = g_disk_root.gid = 0;
    g_disk_root.inode = 0;
    g_disk_root.length = 0;
    g_disk_root.flags = FS_DIRECTORY;
    g_disk_root.program_type = 0;
    g_disk_root.impl = FS_IMPL_DISKFAT;
    g_disk_root.read = 0;
    g_disk_root.write = 0;
    g_disk_root.open = 0;
    g_disk_root.close = 0;
    g_disk_root.readdir = &diskfat_readdir;
    g_disk_root.finddir = &diskfat_finddir;
    g_disk_root.ptr = 0;

    return 1;
}

fs_node_t *diskfat_get_root(void)
{
    return g_mounted ? &g_disk_root : 0;
}

fs_node_t *diskfat_find(const char *name)
{
    if (!g_mounted || !name) return 0;
    return diskfat_finddir(&g_disk_root, (char *)name);
}
