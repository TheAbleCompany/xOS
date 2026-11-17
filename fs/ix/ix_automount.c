// ix_automount.c -- автоматическое обнаружение и монтирование разделов.
//
// Порядок для КАЖДОГО накопителя (ATA/AHCI/NVMe):
//   1. GPT: заголовок в LBA 1 (в секторах ДИСКА -- у 4Kn это байт 4096),
//      с проверкой CRC32 заголовка и массива записей; если основной
//      заголовок повреждён -- резервный в последнем LBA диска.
//   2. Иначе MBR: проверка правдоподобия таблицы (статусы 0x00/0x80,
//      LBA внутри диска), первичные разделы + цепочка EBR (64-бит LBA).
//   3. Иначе (или если ничего не смонтировалось) -- superfloppy:
//      ФС прямо с LBA 0 (флешки без разметки).
//   4. Каждый раздел проходит ix_fs_probe(): распознаются и "чужие" ФС
//      (BitLocker, LUKS, exFAT, swap, LVM, ...) -- они не монтируются,
//      но причина пишется в лог.  Поддерживаемые монтируются как
//      disk0, disk1, ...
//
// Диск каждого монтирования запоминается (am_mount_t.dev) и выбирается
// перед КАЖДОЙ операцией ФС -- иначе при нескольких накопителях чтение
// шло бы с последнего просканированного диска.

#include "ix_automount.h"

#include "ix_fs_detect.h"
#include "ix_fs_fat.h"
#include "ix_fs_ext.h"
#include "ix_fs_ntfs.h"
#include "ixstd_fs.h"
#include "kernel.h"
#include "ixk_string.h"

#include "../../lib/common.h"
#include "../../mm/kheap.h"

/* Имя/размер текущего накопителя (ixk_storage_*). */

#define malloc kmalloc
#define free   kfree

/* Заголовочная lib/common.h даёт debug_puts() (COM1). */
void debug_puts(const char *s);

/* Десятичный u64 в COM1 -- без printf в ядре. */
static void am_debug_u64(uint64_t v) {
    char tmp[24];
    char out[24];
    int  n = 0;
    if (v == 0) { debug_puts("0"); return; }
    while (v) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    debug_puts(out);
}

/*
 * Кэш узлов VFS.  Раньше: 64 узла на том и ПУСТОЙ ответ при переполнении --
 * после открытия 64 различных путей (любой каталог с >64 записями) все
 * остальные файлы "переставали существовать".  Теперь: растущий массив
 * (до AM_NODE_MAX) + хеш-таблица по пути.  Ядро не вызывает node->close,
 * поэтому узлы не освобождаются (иначе повисли бы указатели в открытых fd).
 */
#define AM_NODE_MAX     8192
#define AM_HASH_SIZE    16384          /* степень двойки, > 2 * AM_NODE_MAX */
#define AM_PATH_MAX     512            /* предел длины пути внутри тома */
#define AM_NODE_MAGIC   0x414D4E44u    /* "AMND" */

/* Узел кэша: fs_node_t + полный относительный путь (имя в fs_node_t
 * хранит только последний компонент, он ограничен 127 байтами). */
typedef struct {
    fs_node_t node;                    /* ДОЛЖЕН быть первым */
    uint32_t  magic;
    char      path[1];                 /* [строка переменной длины] */
} am_node_t;

typedef struct {
    int            used;
    int            dev;                  /* индекс накопителя в blkdev */
    int            read_only;            /* монтирование только чтение */
    const char    *fs_name;              /* "FAT32", "ext4", ...       */
    ix_fs_type_t   fs_type;
    uint64_t       part_lba;
    uint64_t       part_sectors;

    fat_context_t  fat;
    ext_context_t  ext;
    ntfs_context_t ntfs;

    fs_node_t      root;                 /* diskN */
    fs_node_t    **nodes;                /* кэш узлов (растёт по мере надобности) */
    int            node_n;
    int            node_cap;
    uint16_t      *hash;                 /* индекс узла + 1 (0 = пусто), AM_HASH_SIZE */
} am_mount_t;

static am_mount_t g_am[AUTOMOUNT_MAX_DISKS];
static int        g_am_n = 0;

static struct dirent g_am_dirent;

/* Вперед объявленные VFS-колбэки (используются в кэше узлов). */
static u32int      am_read_cb(fs_node_t *node, u32int offset, u32int size, u8int *buffer);
static u32int      am_write_cb(fs_node_t *node, u32int offset, u32int size, u8int *buffer);
static fs_node_t  *am_finddir_cb(fs_node_t *node, char *name);
static struct dirent *am_readdir_cb(fs_node_t *node, u32int index);

/* ------------------------------------------------------------------ */
/*  Вспомогательное                                                    */
/* ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* Сравнение байтов (без libc): 0 = равны. */
static int memcmp_bytes(const uint8_t *a, const char *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != (uint8_t)b[i]) return 1;
    return 0;
}

static void am_hex(uint32_t v, int w) {
    static const char hx[] = "0123456789abcdef";
    for (int i = w - 1; i >= 0; i--) debug_putc(hx[(v >> (i * 4)) & 15]);
}

/* "disk0", "disk1", ... */
static void am_make_name(char *dst, int idx) {
    dst[0] = 'd'; dst[1] = 'i'; dst[2] = 's'; dst[3] = 'k';
    int v = idx;
    char tmp[8];
    int  n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) dst[4 + i] = tmp[n - 1 - i];
    dst[4 + n] = 0;
}

const char *automount_fs_label(int idx) {
    if (idx < 0 || idx >= g_am_n) return "";
    return g_am[idx].fs_name ? g_am[idx].fs_name : "unknown";
}

int automount_fs_type(int idx) {
    if (idx < 0 || idx >= g_am_n) return IX_FS_TYPE_UNKNOWN;
    return (int)g_am[idx].fs_type;
}

int automount_writeable(int idx) {
    if (idx < 0 || idx >= g_am_n) return 0;
    return g_am[idx].fs_type != IX_FS_TYPE_NTFS && !g_am[idx].read_only;
}

int automount_read_only(int idx) {
    if (idx < 0 || idx >= g_am_n) return 0;
    return g_am[idx].fs_type == IX_FS_TYPE_NTFS || g_am[idx].read_only;
}

int automount_count(void) {
    return g_am_n;
}

const char *automount_disk_name(int idx) {
    if (idx < 0 || idx >= g_am_n) return "";
    return g_am[idx].root.name;
}

fs_node_t *automount_get_root(int idx) {
    if (idx < 0 || idx >= g_am_n) return 0;
    return &g_am[idx].root;
}

/* Определить смонтированный диск по fs_node (root или кэшированный). */
static am_mount_t *am_mount_of(fs_node_t *node) {
    if (!node) return 0;
    int idx = (int)(uintptr_t)node->ptr - 1;
    if (idx < 0 || idx >= g_am_n) return 0;
    if (!g_am[idx].used) return 0;
    if (node == &g_am[idx].root) return &g_am[idx];

    if (node->impl == FS_IMPL_DISKAUTO &&
        ((am_node_t *)node)->magic == AM_NODE_MAGIC)
        return &g_am[idx];

    return 0;
}

/* Выбрать накопитель этого монтирования для последующих чтений/записей. */
static void am_select(const am_mount_t *m) {
    ixk_storage_set_disk(m->dev);
}

/*
 * Курсор каталога.  VFS вызывает readdir(index) для index = 0,1,2,...;
 * раньше КАЖДЫЙ вызов заново открывал каталог и читал index записей
 * (в ext -- с чтением inode на каждую!), то есть листинг каталога из N
 * записей стоил O(N^2) обращений к диску.  Теперь последовательный вызов
 * продолжает с открытого дескриптора.  Курсор один на всё ядро; запись
 * на любой том его сбрасывает.
 */
typedef struct {
    int      valid;
    int      mount;                 /* индекс тома */
    int      dh;                    /* дескриптор каталога драйвера ФС */
    u32int   next;                  /* индекс следующей записи */
    char     path[AM_PATH_MAX];
} am_cursor_t;

static am_cursor_t g_am_cur;

static int am_fs_opendir(am_mount_t *m, const char *rel) {
    switch (m->fs_type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32: return fat_opendir(&m->fat, rel);
        case IX_FS_TYPE_EXT:   return ext_opendir(&m->ext, rel);
        case IX_FS_TYPE_NTFS:  return ntfs_opendir(&m->ntfs, rel);
        default:               return -1;
    }
}

static int am_fs_readdir(am_mount_t *m, int dh, ix_dirent_t *ent) {
    switch (m->fs_type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32: return fat_readdir(&m->fat, dh, ent);
        case IX_FS_TYPE_EXT:   return ext_readdir(&m->ext, dh, ent);
        case IX_FS_TYPE_NTFS:  return ntfs_readdir(&m->ntfs, dh, ent);
        default:               return 0;
    }
}

static void am_fs_closedir(am_mount_t *m, int dh) {
    switch (m->fs_type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32: fat_closedir(&m->fat, dh); break;
        case IX_FS_TYPE_EXT:   ext_closedir(&m->ext, dh); break;
        case IX_FS_TYPE_NTFS:  ntfs_closedir(&m->ntfs, dh); break;
        default: break;
    }
}

static void am_cursor_drop(void) {
    if (g_am_cur.valid) {
        if (g_am_cur.mount >= 0 && g_am_cur.mount < g_am_n && g_am[g_am_cur.mount].used) {
            am_select(&g_am[g_am_cur.mount]);
            am_fs_closedir(&g_am[g_am_cur.mount], g_am_cur.dh);
        }
        g_am_cur.valid = 0;
    }
}

/* Относительный путь узла: корень -> "/", остальные -> name (относ. путь). */
static const char *am_relpath(am_mount_t *m, fs_node_t *node) {
    /* Корень диска хранит имя ТОЧКИ МОНТИРОВАНИЯ ("disk0"), а внутри драйвера
     * ФС корень -- "/".  Кэшированные узлы хранят полный путь в am_node_t. */
    if (m && node == &m->root)
        return "/";
    return ((am_node_t *)node)->path;
}
/* ------------------------------------------------------------------ */
/*  Кэш узлов                                                          */
/* ------------------------------------------------------------------ */

static uint32_t am_hash_str(const char *s) {
    uint32_t h = 2166136261u;                 /* FNV-1a */
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static fs_node_t *am_cache_lookup(am_mount_t *m, const char *relpath) {
    if (!m->hash) return 0;
    uint32_t i = am_hash_str(relpath) & (AM_HASH_SIZE - 1);
    while (m->hash[i]) {
        fs_node_t *n = m->nodes[m->hash[i] - 1];
        if (strcmp(((am_node_t *)n)->path, (char *)relpath) == 0)
            return n;
        i = (i + 1) & (AM_HASH_SIZE - 1);
    }
    return 0;
}

/*
 * Выкинуть узел пути из кэша, чтобы следующий resolve_path() снова
 * спросил драйвер ФС.  Узел снимается из хэша, а освобождается только
 * при условии, что на него никто не смотрит: после unlink открытые
 * дескрипторы по POSIX обязаны дожить до close(), поэтому узел с
 * ненулевым refs остаётся жить вне индекса и освобождается в close_fs.
 */
static void am_cache_forget(am_mount_t *m, const char *relpath) {
    if (!m->hash) return;

    uint32_t i = am_hash_str(relpath) & (AM_HASH_SIZE - 1);

    while (m->hash[i]) {
        fs_node_t *n = m->nodes[m->hash[i] - 1];
        am_node_t *an = (am_node_t *)n;

        if (strcmp(an->path, (char *)relpath) == 0) {
            uint32_t slot = (uint32_t)m->hash[i] - 1u;
            uint32_t j = i;

            m->hash[i] = 0;

            for (;;) {
                uint32_t next = (j + 1) & (AM_HASH_SIZE - 1);

                if (!m->hash[next]) break;

                {
                    fs_node_t *ln = m->nodes[m->hash[next] - 1];
                    uint32_t home = am_hash_str(((am_node_t *)ln)->path) &
                                    (AM_HASH_SIZE - 1);
                    int fits = (j <= next) ? (home <= j || home > next)
                                           : (home <= j && home > next);

                    if (!fits) {
                        m->hash[j] = m->hash[next];
                        m->hash[next] = 0;
                        j = next;
                    }
                }

                if (next == i) break;
            }

            free(an);
            m->nodes[slot] = 0;
            m->node_n--;

            if (m->node_n > 0 && slot != (uint32_t)m->node_n) {
                m->nodes[slot] = m->nodes[m->node_n];
                m->nodes[m->node_n] = 0;

                for (uint32_t k = 0; k < AM_HASH_SIZE; k++) {
                    if (m->hash[k] == (uint16_t)(m->node_n + 1u)) {
                        m->hash[k] = (uint16_t)(slot + 1u);
                        break;
                    }
                }
            }

            return;
        }

        i = (i + 1) & (AM_HASH_SIZE - 1);
    }
}

static fs_node_t *am_cache_put(am_mount_t *m, int midx, const char *relpath,
                               const ix_file_info_t *info) {
    if (m->node_n >= AM_NODE_MAX) return 0;       /* предел на том */

    size_t plen = strlen((char *)relpath);
    if (plen >= AM_PATH_MAX) return 0;

    if (!m->hash) {
        m->hash = (uint16_t *)malloc(sizeof(uint16_t) * AM_HASH_SIZE);
        if (!m->hash) return 0;
        memset(m->hash, 0, sizeof(uint16_t) * AM_HASH_SIZE);
    }
    if (m->node_n >= m->node_cap) {
        int ncap = m->node_cap ? m->node_cap * 2 : 64;
        if (ncap > AM_NODE_MAX) ncap = AM_NODE_MAX;
        fs_node_t **na = (fs_node_t **)malloc(sizeof(fs_node_t *) * (size_t)ncap);
        if (!na) return 0;
        for (int i = 0; i < m->node_n; i++) na[i] = m->nodes[i];
        if (m->nodes) free(m->nodes);
        m->nodes = na;
        m->node_cap = ncap;
    }

    am_node_t *an = (am_node_t *)malloc(sizeof(am_node_t) + plen);
    if (!an) return 0;
    fs_node_t *n = &an->node;
    an->magic = AM_NODE_MAGIC;
    memcpy(an->path, relpath, plen + 1);

    /* fs_node_t.name -- только последний компонент пути. */
    const char *base = relpath;
    for (const char *q = relpath; *q; q++)
        if (*q == '/') base = q + 1;
    strncpy(n->name, base, sizeof(n->name) - 1);
    n->name[sizeof(n->name) - 1] = 0;

    n->mask = n->uid = n->gid = n->inode = 0;
    n->program_type = 0;
    n->impl = FS_IMPL_DISKAUTO;
    n->ptr  = (void *)(uintptr_t)(midx + 1);

    if (info->attrs & IX_FS_ATTR_DIR) {
        n->flags   = FS_DIRECTORY;
        n->length  = 0;
        n->read    = 0;
        n->write   = 0;
        n->readdir = &am_readdir_cb;
        n->finddir = &am_finddir_cb;
    } else {
        n->flags   = FS_FILE;
        n->length  = (u32int)info->size;
        n->read    = &am_read_cb;
        n->write   = (m->fs_type != IX_FS_TYPE_NTFS && !m->read_only) ? &am_write_cb : 0;
        n->readdir = 0;
        n->finddir = 0;
    }
    n->open  = 0;
    n->close = 0;

    m->nodes[m->node_n] = n;
    m->node_n++;

    uint32_t hi = am_hash_str(an->path) & (AM_HASH_SIZE - 1);
    while (m->hash[hi]) hi = (hi + 1) & (AM_HASH_SIZE - 1);
    m->hash[hi] = (uint16_t)m->node_n;            /* индекс + 1 */
    return n;
}

/* ------------------------------------------------------------------ */
/*  VFS-колбэки                                                        */
/* ------------------------------------------------------------------ */

static u32int am_read_cb(fs_node_t *node, u32int offset, u32int size, u8int *buffer) {
    am_mount_t *m = am_mount_of(node);
    if (!m || !buffer || size == 0) return 0;
    am_select(m);

    const char *rel = am_relpath(m, node);

    switch (m->fs_type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32: {
            int r = fat_read_at(&m->fat, rel, offset, buffer, size);
            return (r > 0) ? (u32int)r : 0;
        }
        case IX_FS_TYPE_EXT: {
            int r = ext_read_at(&m->ext, rel, offset, buffer, size);
            return (r > 0) ? (u32int)r : 0;
        }
        case IX_FS_TYPE_NTFS: {
            int r = ntfs_read_at(&m->ntfs, rel, offset, buffer, size);
            return (r > 0) ? (u32int)r : 0;
        }
        default:
            return 0;
    }
}

static u32int am_write_cb(fs_node_t *node, u32int offset, u32int size, u8int *buffer) {
    am_mount_t *m = am_mount_of(node);
    if (!m || !buffer || size == 0) return 0;
    if (m->fs_type == IX_FS_TYPE_NTFS || m->read_only) return 0;
    am_select(m);
    am_cursor_drop();

    const char *rel = am_relpath(m, node);
    int r;

    switch (m->fs_type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32:
            r = fat_write_at(&m->fat, rel, offset, buffer, size);
            break;
        case IX_FS_TYPE_EXT:
            r = ext_write_at(&m->ext, rel, offset, buffer, size);
            break;
        default:
            return 0;
    }

    if (r > 0)
        ixk_storage_sync();         /* один flush на VFS-запись */

    if (r > 0 && offset + (u32int)r > node->length)
        node->length = offset + (u32int)r;

    return (r > 0) ? (u32int)r : 0;
}

static fs_node_t *am_find_node(am_mount_t *m, const char *relpath) {
    fs_node_t *cached = am_cache_lookup(m, relpath);
    if (cached) return cached;
    am_select(m);

    ix_file_info_t info;

    switch (m->fs_type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32:
            if (fat_stat(&m->fat, relpath, &info) != 0) return 0;
            break;
        case IX_FS_TYPE_EXT:
            if (ext_stat(&m->ext, relpath, &info) != 0) return 0;
            break;
        case IX_FS_TYPE_NTFS:
            if (ntfs_stat(&m->ntfs, relpath, &info) != 0) return 0;
            break;
        default:
            return 0;
    }

    return am_cache_put(m, (int)(m - g_am), relpath, &info);
}

static fs_node_t *am_finddir_cb(fs_node_t *node, char *name) {
    am_mount_t *m = am_mount_of(node);
    if (!m || !name || !name[0]) return 0;

    /* Относительный путь дочернего узла. */
    const char *base = am_relpath(m, node);

    char rel[AM_PATH_MAX];
    int  bl = 0;

    if (base[0] == '/' && base[1] == 0) {
        /* родитель -- корень диска */
        for (int i = 0; name[i] && bl < (int)sizeof(rel) - 1; i++)
            rel[bl++] = name[i];
    } else {
        for (int i = 0; base[i] && bl < (int)sizeof(rel) - 2; i++)
            rel[bl++] = base[i];
        if (bl < (int)sizeof(rel) - 1) rel[bl++] = '/';
        for (int i = 0; name[i] && bl < (int)sizeof(rel) - 1; i++)
            rel[bl++] = name[i];
    }
    rel[bl] = 0;

    return am_find_node(m, rel);
}

static struct dirent *am_readdir_cb(fs_node_t *node, u32int index) {
    am_mount_t *m = am_mount_of(node);
    if (!m) return 0;
    am_select(m);

    const char *rel = am_relpath(m, node);
    int midx = (int)(m - g_am);
    int rl = (int)strlen((char *)rel);
    int dh;
    u32int start;

    if (g_am_cur.valid && g_am_cur.mount == midx && g_am_cur.next == index &&
        rl < AM_PATH_MAX && strcmp(g_am_cur.path, (char *)rel) == 0) {
        /* Последовательное чтение: продолжаем с курсора. */
        dh = g_am_cur.dh;
        start = index;
    } else {
        am_cursor_drop();
        am_select(m);
        dh = am_fs_opendir(m, rel);
        if (dh < 0) return 0;
        start = 0;                       /* пропустить index записей */
    }

    ix_dirent_t ent;
    int rc = 0;
    for (u32int i = start; i <= index; i++) {
        rc = am_fs_readdir(m, dh, &ent);
        if (rc <= 0) break;
    }

    if (rc <= 0) {                       /* конец каталога или ошибка */
        am_fs_closedir(m, dh);
        g_am_cur.valid = 0;
        return 0;
    }

    if (rl < AM_PATH_MAX) {
        g_am_cur.valid = 1;
        g_am_cur.mount = midx;
        g_am_cur.dh    = dh;
        g_am_cur.next  = index + 1;
        memcpy(g_am_cur.path, rel, (size_t)rl + 1);
    } else {
        am_fs_closedir(m, dh);           /* слишком длинный путь: без курсора */
        g_am_cur.valid = 0;
    }

    strncpy(g_am_dirent.name, ent.name, sizeof(g_am_dirent.name) - 1);
    g_am_dirent.name[sizeof(g_am_dirent.name) - 1] = 0;
    g_am_dirent.ino  = index;
    g_am_dirent.type = ent.is_dir ? FS_DIRECTORY : FS_FILE;
    return &g_am_dirent;
}

/* ------------------------------------------------------------------ */
/*  Создание / усечение (O_CREAT / O_TRUNC)                            */
/* ------------------------------------------------------------------ */

fs_node_t *automount_create_at(int idx, const char *relpath) {
    if (idx < 0 || idx >= g_am_n || !relpath || !relpath[0]) return 0;
    am_mount_t *m = &g_am[idx];
    if (m->fs_type == IX_FS_TYPE_NTFS || m->read_only) return 0;
    am_select(m);

    char rel[AM_PATH_MAX];
    strncpy(rel, relpath, sizeof(rel) - 1);
    rel[sizeof(rel) - 1] = 0;
    am_cursor_drop();

    /* Если уже есть -- вернуть узел. */
    fs_node_t *existing = am_find_node(m, rel);
    if (existing) return existing;

    int rc;
    switch (m->fs_type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32: rc = fat_create(&m->fat, rel); break;
        case IX_FS_TYPE_EXT:   rc = ext_create(&m->ext, rel); break;
        default: return 0;
    }
    if (rc != 0) return 0;
    ixk_storage_sync();

    return am_find_node(m, rel);
}

int automount_unlink_at(int idx, const char *relpath) {
    if (idx < 0 || idx >= g_am_n || !relpath || !relpath[0]) return -1;
    am_mount_t *m = &g_am[idx];
    if (!m->used || m->fs_type == IX_FS_TYPE_NTFS || m->read_only) return -1;
    am_select(m);
    am_cursor_drop();

    int rc;
    switch (m->fs_type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32: rc = fat_unlink(&m->fat, relpath); break;
        case IX_FS_TYPE_EXT:   rc = ext_unlink(&m->ext, relpath); break;
        default: return -1;
    }

    if (rc == 0) {
        am_cache_forget(m, relpath);
        ixk_storage_sync();
    }
    return rc;
}

int automount_rename_at(int idx, const char *from, const char *to) {
    if (idx < 0 || idx >= g_am_n || !from || !to || !from[0] || !to[0]) return -1;
    am_mount_t *m = &g_am[idx];
    if (!m->used || m->fs_type == IX_FS_TYPE_NTFS || m->read_only) return -1;
    am_select(m);
    am_cursor_drop();

    int rc;
    switch (m->fs_type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32: rc = fat_rename(&m->fat, from, to); break;
        case IX_FS_TYPE_EXT:   rc = ext_rename(&m->ext, from, to); break;
        default: return -1;
    }

    if (rc == 0) {
        am_cache_forget(m, from);
        am_cache_forget(m, to);
        ixk_storage_sync();
    }
    return rc;
}

fs_node_t *automount_mkdir_at(int idx, const char *relpath) {
    if (idx < 0 || idx >= g_am_n || !relpath || !relpath[0]) return 0;
    am_mount_t *m = &g_am[idx];
    if (!m->used || m->fs_type == IX_FS_TYPE_NTFS || m->read_only) return 0;
    am_select(m);
    am_cursor_drop();

    char rel[AM_PATH_MAX];
    strncpy(rel, relpath, sizeof(rel) - 1);
    rel[sizeof(rel) - 1] = 0;

    int rc;
    switch (m->fs_type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32: rc = fat_mkdir(&m->fat, rel); break;
        default: return 0;
    }
    if (rc != 0) return 0;

    ixk_storage_sync();
    am_cache_forget(m, rel);
    return am_find_node(m, rel);
}

int automount_truncate_at(int idx, fs_node_t *node) {
    if (idx < 0 || idx >= g_am_n || !node) return -1;
    am_mount_t *m = &g_am[idx];
    if (m->fs_type == IX_FS_TYPE_NTFS || m->read_only) return -1;
    am_select(m);

    am_cursor_drop();
    const char *rel = am_relpath(m, node);
    int rc;

    switch (m->fs_type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32: rc = fat_truncate_path(&m->fat, rel); break;
        case IX_FS_TYPE_EXT:   rc = ext_truncate_path(&m->ext, rel); break;
        default: return -1;
    }

    if (rc == 0) {
        node->length = 0;
        ixk_storage_sync();
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Монтирование одного раздела                                        */
/* ------------------------------------------------------------------ */

static int am_cur_dev = 0;     /* накопитель, который сейчас сканируется */

static void am_log_part(const char *src, uint64_t lba, uint64_t sectors,
                        const ix_fs_info_t *info) {
    debug_puts("[AUTOMOUNT]   ");
    debug_puts(src);
    debug_puts(" @LBA ");
    am_debug_u64(lba);
    debug_puts(" (");
    am_debug_u64(sectors);
    debug_puts(" sect): ");
    debug_puts(info->name);
    if (info->label[0]) { debug_puts(" \""); debug_puts(info->label); debug_puts("\""); }
    if (info->note)     { debug_puts(" -- "); debug_puts(info->note); }
    debug_putc('\n');
}

static int am_mount_partition(uint64_t lba, uint64_t sectors, const ix_fs_info_t *info) {
    if (g_am_n >= AUTOMOUNT_MAX_DISKS) {
        debug_puts("[AUTOMOUNT]   mount table full -- skipped\n");
        return 0;
    }
    ix_fs_type_t type = info->type;
    if (!ix_fs_type_mountable(type)) return 0;

    am_mount_t *m = &g_am[g_am_n];
    ix_memset(m, 0, sizeof(*m));
    ixk_storage_set_disk(am_cur_dev);

    int ok = 0;
    switch (type) {
        case IX_FS_TYPE_FAT12:
        case IX_FS_TYPE_FAT16:
        case IX_FS_TYPE_FAT32: ok = fat_mount(&m->fat, lba, sectors, type); break;
        case IX_FS_TYPE_EXT:   ok = ext_mount(&m->ext, lba, sectors);       break;
        case IX_FS_TYPE_NTFS:  ok = ntfs_mount(&m->ntfs, lba, sectors);     break;
        default: ok = 0;
    }

    if (!ok) {
        debug_puts("[AUTOMOUNT]   mount FAILED: ");
        debug_puts(info->name);
        debug_puts(" @ LBA ");
        am_debug_u64(lba);
        debug_putc('\n');
        ix_memset(m, 0, sizeof(*m));
        return 0;
    }

    m->used         = 1;
    m->dev          = am_cur_dev;
    m->read_only    = info->read_only;
    m->fs_name      = info->name;
    m->fs_type      = type;
    m->part_lba     = lba;
    m->part_sectors = sectors;

    fs_node_t *root = &m->root;
    am_make_name(root->name, g_am_n);

    debug_puts("[AUTOMOUNT] ");
    debug_puts(root->name);
    debug_puts(": ");
    debug_puts(info->name);
    debug_puts(" @ LBA ");
    am_debug_u64(lba);
    debug_puts(" (");
    am_debug_u64(sectors);
    debug_puts(" sect)");
    if (automount_read_only(g_am_n)) debug_puts(" [read-only]");
    debug_putc('\n');

    root->mask = root->uid = root->gid = 0;
    root->inode = 0;
    root->length = 0;
    root->flags = FS_DIRECTORY;
    root->program_type = 0;
    root->impl = FS_IMPL_DISKAUTO;
    root->read = 0;
    root->write = 0;
    root->open = 0;
    root->close = 0;
    root->readdir = &am_readdir_cb;
    root->finddir = &am_finddir_cb;
    root->ptr = (void *)(uintptr_t)(g_am_n + 1);

    g_am_n++;
    return 1;
}

/* Опознать раздел и, если ФС поддержана, смонтировать. */
static int am_handle_partition(const char *src, uint64_t lba, uint64_t sectors) {
    ix_fs_info_t info;

    if (!ix_fs_probe(lba, sectors, &info)) {
        debug_puts("[AUTOMOUNT]   ");
        debug_puts(src);
        debug_puts(" @LBA ");
        am_debug_u64(lba);
        debug_puts(": unreadable\n");
        return 0;
    }
    am_log_part(src, lba, sectors, &info);
    if (!info.mountable) return 0;
    return am_mount_partition(lba, sectors, &info);
}

/* ------------------------------------------------------------------ */
/*  Вспомогательное: CRC32 и байтовое чтение                           */
/* ------------------------------------------------------------------ */

static uint32_t am_crc32(uint32_t crc, const uint8_t *p, uint32_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

/* Чтение len байт с байтового смещения диска (через 512-байтные LBA). */
static int am_read_bytes(uint64_t byte_off, void *dst, uint32_t len) {
    uint8_t  sec[512];
    uint8_t *d = (uint8_t *)dst;
    while (len) {
        uint64_t lba = byte_off / 512u;
        uint32_t o   = (uint32_t)(byte_off % 512u);
        uint32_t n   = 512u - o;
        if (n > len) n = len;
        if (!ixk_storage_read(lba, 1, sec)) return 0;
        for (uint32_t i = 0; i < n; i++) d[i] = sec[o + i];
        d += n; byte_off += n; len -= n;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  GPT                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t ent_lba;       /* в РОДНЫХ секторах диска */
    uint32_t num;
    uint32_t size;
} am_gpt_t;

/*
 * Проверить GPT-заголовок в родном LBA hlba.  Раскладка (UEFI 2.x):
 *   +0 "EFI PART", +12 HeaderSize, +16 HeaderCRC32, +24 MyLBA,
 *   +32 AlternateLBA, +40 FirstUsable, +48 LastUsable, +56 DiskGUID[16],
 *   +72 PartitionEntryLBA, +80 NumberOfEntries, +84 SizeOfEntry,
 *   +88 PartitionEntryArrayCRC32.
 */
static int am_gpt_check(uint64_t hlba, uint32_t lss, am_gpt_t *g, const char *which) {
    uint8_t h[512];

    if (!am_read_bytes(hlba * lss, h, 512)) return 0;
    if (memcmp_bytes(h, "EFI PART", 8) != 0) return 0;

    uint32_t hsz = rd32(h + 12);
    if (hsz < 92 || hsz > 512) return 0;

    uint32_t crc_stored = rd32(h + 16);
    h[16] = h[17] = h[18] = h[19] = 0;
    if (am_crc32(0, h, hsz) != crc_stored) {
        debug_puts("[AUTOMOUNT]   GPT "); debug_puts(which);
        debug_puts(" header CRC mismatch\n");
        return 0;
    }
    if (rd64(h + 24) != hlba) return 0;

    g->ent_lba = rd64(h + 72);
    g->num     = rd32(h + 80);
    g->size    = rd32(h + 84);
    uint32_t ent_crc = rd32(h + 88);

    if (g->ent_lba == 0 || g->num == 0 || g->num > 4096) return 0;
    if (g->size < 128 || g->size > 4096 || (g->size & (g->size - 1))) return 0;

    /* CRC массива записей -- потоково. */
    uint64_t base  = g->ent_lba * lss;
    uint64_t total = (uint64_t)g->num * g->size;
    uint32_t crc = 0;
    uint8_t  buf[512];
    for (uint64_t off = 0; off < total; off += 512) {
        uint32_t n = (total - off) < 512 ? (uint32_t)(total - off) : 512u;
        if (!am_read_bytes(base + off, buf, n)) return 0;
        crc = am_crc32(crc, buf, n);
    }
    if (crc != ent_crc) {
        debug_puts("[AUTOMOUNT]   GPT "); debug_puts(which);
        debug_puts(" entries CRC mismatch\n");
        return 0;
    }
    return 1;
}

/* Возвращает число смонтированных; *found = 1, если найдена валидная GPT. */
static int am_scan_gpt(uint32_t lss, uint64_t cap512, int *found) {
    am_gpt_t g;
    uint32_t ratio = lss / 512u;
    uint64_t cap_native = cap512 / ratio;
    const char *src = "primary";

    *found = 0;
    int ok = am_gpt_check(1, lss, &g, "primary");
    if (!ok && cap_native > 2) {
        ok = am_gpt_check(cap_native - 1, lss, &g, "backup");
        if (ok) src = "BACKUP (primary damaged)";
    }
    if (!ok) return 0;

    *found = 1;
    debug_puts("[AUTOMOUNT] GPT partition table (");
    debug_puts(src);
    debug_puts(", sector ");
    am_debug_u64(lss);
    debug_puts(")\n");

    int mounted = 0;
    uint32_t limit = g.num < 256 ? g.num : 256;

    for (uint32_t i = 0; i < limit; i++) {
        uint8_t e[128];
        if (!am_read_bytes(g.ent_lba * lss + (uint64_t)i * g.size, e, 128)) break;

        int empty = 1;
        for (int b = 0; b < 16; b++) if (e[b]) { empty = 0; break; }
        if (empty) continue;

        uint64_t first = rd64(e + 32);
        uint64_t last  = rd64(e + 40);
        uint64_t attr  = rd64(e + 48);
        if (first == 0 || last < first) continue;

        uint64_t first512 = first * ratio;
        uint64_t sec512   = (last - first + 1) * ratio;
        if (cap512 && first512 + sec512 > cap512) {
            debug_puts("[AUTOMOUNT]   GPT entry ");
            am_debug_u64(i + 1);
            debug_puts(" extends past end of disk -- skipped\n");
            continue;
        }

        char src2[16] = "GPT #";
        {   /* "GPT #<n>" */
            char t[8]; int k = 0; uint32_t v = i + 1;
            while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
            int p = 5;
            while (k) src2[p++] = t[--k];
            src2[p] = 0;
        }

        if (attr & (1ull << 63)) {          /* "no automount" (Windows recovery и т.п.) */
            debug_puts("[AUTOMOUNT]   "); debug_puts(src2);
            debug_puts(": flagged no-automount -- skipped\n");
            continue;
        }

        mounted += am_handle_partition(src2, first512, sec512);
    }
    return mounted;
}

/* ------------------------------------------------------------------ */
/*  MBR / EBR                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  type;
    uint32_t first;
    uint32_t sectors;
} am_mbr_ent_t;

static int am_type_is_extended(uint8_t t) {
    return t == 0x05 || t == 0x0F || t == 0x85;
}

/*
 * Правдоподобна ли таблица разделов.  Загрузочный сектор FAT/NTFS тоже
 * заканчивается 0x55AA, а на смещении 446 у него -- код/текст, поэтому
 * одной сигнатуры мало: статус каждой записи обязан быть 0x00 или 0x80,
 * начало раздела -- внутри диска.  Возвращает число непустых записей.
 */
static int am_mbr_plausible(const uint8_t *sec, uint64_t cap512, am_mbr_ent_t *out) {
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;

    int n = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = sec + 446 + i * 16;
        out[i].type    = p[4];
        out[i].first   = rd32(p + 8);
        out[i].sectors = rd32(p + 12);

        if (p[0] != 0x00 && p[0] != 0x80) return 0;
        if (p[4] == 0) { out[i].sectors = 0; continue; }
        if (out[i].sectors == 0) continue;
        if (out[i].first == 0) return 0;
        if (cap512 && out[i].first >= cap512) return 0;
        n++;
    }
    return n;
}

static int am_scan_mbr(const am_mbr_ent_t *ent, uint64_t cap512) {
    int mounted = 0;
    uint64_t ext_base = 0;
    int have_ext = 0;
    int saw_protective = 0;

    for (int i = 0; i < 4; i++) {
        if (ent[i].type == 0 || ent[i].sectors == 0) continue;
        if (ent[i].type == 0xEE) { saw_protective = 1; continue; }
        if (am_type_is_extended(ent[i].type)) {
            if (!have_ext) { ext_base = ent[i].first; have_ext = 1; }
            continue;
        }

        uint64_t sectors = ent[i].sectors;
        if (cap512 && (uint64_t)ent[i].first + sectors > cap512)
            sectors = cap512 - ent[i].first;          /* обрезаем до диска */

        char src[8] = "MBR #0";
        src[5] = (char)('1' + i);
        mounted += am_handle_partition(src, ent[i].first, sectors);
    }

    if (saw_protective)
        debug_puts("[AUTOMOUNT]   protective MBR (0xEE) but GPT is invalid\n");

    /* Логические диски: EBR-цепочка, 64-битные LBA, защита от петель. */
    if (have_ext) {
        uint64_t seen[64];
        int      nseen = 0;
        uint64_t ebr = ext_base;
        int      lnum = 5;

        for (int guard = 0; guard < 64; guard++) {
            int loop = 0;
            for (int k = 0; k < nseen; k++) if (seen[k] == ebr) loop = 1;
            if (loop) { debug_puts("[AUTOMOUNT]   EBR loop -- stop\n"); break; }
            seen[nseen++] = ebr;

            uint8_t esec[512];
            if (!ixk_storage_read(ebr, 1, esec)) break;
            if (esec[510] != 0x55 || esec[511] != 0xAA) break;

            const uint8_t *p0 = esec + 446;
            const uint8_t *p1 = esec + 446 + 16;

            uint8_t  t0 = p0[4];
            uint32_t l0 = rd32(p0 + 8);
            uint32_t s0 = rd32(p0 + 12);
            uint8_t  t1 = p1[4];
            uint32_t l1 = rd32(p1 + 8);

            if (t0 != 0 && s0 != 0 && !am_type_is_extended(t0)) {
                char src[12] = "MBR #";
                int  sp = 5;
                if (lnum >= 10) src[sp++] = (char)('0' + (lnum / 10));
                src[sp++] = (char)('0' + (lnum % 10));
                src[sp] = 0;
                uint64_t lba = ebr + l0;
                uint64_t sectors = s0;
                if (cap512 && lba + sectors > cap512) sectors = cap512 > lba ? cap512 - lba : 0;
                if (sectors)
                    mounted += am_handle_partition(src, lba, sectors);
                lnum++;
            }

            if (t1 == 0 || l1 == 0 || !am_type_is_extended(t1)) break;
            ebr = ext_base + l1;                 /* смещение от НАЧАЛА extended */
        }
    }
    return mounted;
}

/* ------------------------------------------------------------------ */
/*  Точка входа                                                        */
/* ------------------------------------------------------------------ */

int automount_init(void) {
    g_am_n = 0;
    ix_memset(g_am, 0, sizeof(g_am));

    int ndev = ixk_storage_disk_count();
    int mounted = 0;

    debug_puts("[AUTOMOUNT] storage devices: ");
    am_debug_u64((uint64_t)ndev);
    debug_putc('\n');

    for (int d = 0; d < ndev && g_am_n < AUTOMOUNT_MAX_DISKS; d++) {
        am_cur_dev = d;
        ixk_storage_set_disk(d);

        const char *devname = ixk_storage_device_name();
        uint32_t lss   = ixk_storage_native_sector();
        uint64_t cap512 = ixk_storage_capacity();

        debug_puts("[AUTOMOUNT] probing device ");
        debug_puts(devname[0] ? devname : "?");
        if (ixk_storage_device_model()[0]) {
            debug_puts(" \"");
            debug_puts(ixk_storage_device_model());
            debug_puts("\"");
        }
        debug_puts(" (");
        am_debug_u64(cap512 / 2048u);
        debug_puts(" MiB, sector ");
        am_debug_u64(lss);
        debug_puts(")\n");

        uint8_t sec0[512];
        if (!ixk_storage_read(0, 1, sec0)) {
            debug_puts("[AUTOMOUNT]   no readable LBA 0 -- skip\n");
            continue;
        }

        /* Диагностика: сигнатура LBA 0 и сырая таблица MBR. */
        debug_puts("[AUTOMOUNT]   LBA0: boot-sig=");
        am_hex(((uint32_t)sec0[510] << 8) | sec0[511], 4);
        debug_puts(" first-bytes=");
        for (int b = 0; b < 8; b++) { am_hex(sec0[b], 2); }
        debug_puts(" entries(type:start:size)=");
        for (int e = 0; e < 4; e++) {
            const uint8_t *pe = sec0 + 446 + e * 16;
            am_hex(pe[4], 2); debug_putc(':');
            am_hex(rd32(pe + 8), 8); debug_putc(':');
            am_hex(rd32(pe + 12), 8); debug_putc(' ');
        }
        debug_putc('\n');

        int on_disk = 0;
        int gpt_found = 0;

        /* 1. GPT. */
        on_disk = am_scan_gpt(lss, cap512, &gpt_found);

        /* 2. MBR. */
        if (!gpt_found) {
            am_mbr_ent_t ent[4];
            int np = am_mbr_plausible(sec0, cap512, ent);
            if (np > 0) {
                debug_puts("[AUTOMOUNT] MBR partition table\n");
                on_disk = am_scan_mbr(ent, cap512);
            }

            /* 3. Superfloppy: нет таблицы разделов либо она ничего не дала. */
            if (on_disk == 0) {
                ix_fs_info_t info;
                if (ix_fs_probe(0, cap512, &info) && info.mountable) {
                    debug_puts("[AUTOMOUNT] no usable partition table -- superfloppy\n");
                    am_log_part("whole disk", 0, cap512, &info);
                    on_disk = am_mount_partition(0, cap512, &info);
                } else if (np == 0) {
                    debug_puts("[AUTOMOUNT]   no partition table, no filesystem");
                    if (info.type != IX_FS_TYPE_UNKNOWN) {
                        debug_puts(" (");
                        debug_puts(info.name);
                        if (info.note) { debug_puts(": "); debug_puts(info.note); }
                        debug_puts(")");
                    }
                    debug_putc('\n');
                }
            }
        }

        mounted += on_disk;
    }

    debug_puts("[AUTOMOUNT] done: ");
    am_debug_u64((uint64_t)mounted);
    debug_puts(" partition(s) mounted\n");

    return mounted;
}

/* ------------------------------------------------------------------ */
/*  Самотест VFS (только для тестовых сборок: -DFS_TREE_SELFTEST)       */
/* ------------------------------------------------------------------ */
#ifdef FS_TREE_SELFTEST
static u8int st_buf[4096];
static int   st_lines = 0;

static void st_u64(uint64_t v) { am_debug_u64(v); }

static void st_walk(const char *vpath, const char *rel, int depth) {
    fs_node_t *dir = resolve_path(vpath);
    if (!dir) { debug_puts("[TREE] MISSING-DIR "); debug_puts(vpath); debug_putc('\n'); return; }

    for (u32int i = 0; i < 4000; i++) {
        struct dirent *de = readdir_fs(dir, i);
        if (!de) break;

        char name[128];
        strncpy(name, de->name, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        int dirent_dir = ((de->type & 7) == FS_DIRECTORY);

        static char vchild[8][700];
        static char rchild[8][700];
        char *vc = vchild[depth & 7], *rc = rchild[depth & 7];
        int a = 0, b = 0;
        for (int k = 0; vpath[k] && a < 690; k++) vc[a++] = vpath[k];
        vc[a++] = '/';
        for (int k = 0; name[k] && a < 698; k++) vc[a++] = name[k];
        vc[a] = 0;
        if (rel[0]) { for (int k = 0; rel[k] && b < 690; k++) rc[b++] = rel[k]; rc[b++] = '/'; }
        for (int k = 0; name[k] && b < 698; k++) rc[b++] = name[k];
        rc[b] = 0;

        fs_node_t *n = resolve_path(vc);
        if (!n) { debug_puts("[TREE] MISSING "); debug_puts(rc); debug_putc('\n'); continue; }

        int node_dir = ((n->flags & 7) == FS_DIRECTORY);
        if (node_dir != dirent_dir) {
            debug_puts("[TREE] TYPE-MISMATCH "); debug_puts(rc);
            debug_puts(dirent_dir ? " dirent=dir node=file\n" : " dirent=file node=dir\n");
        }

        if (node_dir) {
            debug_puts("[TREE] D "); debug_puts(rc); debug_putc('\n');
            if (depth < 14) st_walk(vc, rc, depth + 1);
        } else {
            uint64_t sum = 0, len = 0;
            u32int off = 0;
            for (;;) {
                u32int got = read_fs(n, off, sizeof(st_buf), st_buf);
                if (!got) break;
                for (u32int k = 0; k < got; k++)
                    sum += (uint64_t)st_buf[k] * (1u + (uint32_t)((len + k) % 251u));
                len += got; off += got;
            }
            debug_puts("[TREE] F "); debug_puts(rc); debug_putc('|');
            st_u64(len); debug_putc('|'); st_u64(sum); debug_putc('\n');
        }
        st_lines++;
    }
}

void automount_selftest(void) {
    for (int d = 0; d < g_am_n; d++) {
        char root[16];
        am_make_name(root, d);
        debug_puts("[TREE] BEGIN "); debug_puts(root); debug_putc(' ');
        debug_puts(g_am[d].fs_name); debug_putc('\n');
        st_walk(root, "", 0);
        debug_puts("[TREE] END "); debug_puts(root); debug_putc('\n');
    }
}
#endif
