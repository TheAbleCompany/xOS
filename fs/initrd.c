// initrd.c -- Initial ramdisk with runtime-write support.
//             Based on JamesM's kernel development tutorials.
//
// The original ramdisk was strictly read-only.  This version adds
// a RAM copy layer:
//
//   * Files that come from the initrd image stay read-only until
//     the first write.  On the first write the file is "promoted":
//     its image contents are copied into a heap buffer and the
//     node starts pointing at an initrd_ram_file_t via node->ptr.
//
//   * Files created at runtime (open with O_CREAT) get an empty
//     initrd_ram_file_t from the start.
//
//   * initrd_write() supports gaps (write past EOF zero-fills).
//
//   * initrd_truncate() implements O_TRUNC correctly: it resets
//     the RAM copy WITHOUT freeing the initrd_ram_file_t struct
//     itself (which node->ptr points at).
//
// CHANGES-notes context: the old open() used to do
// memset(fsnode->ptr, 0, 65536) which trashed 64 KiB of kernel
// heap when ptr was the small 24-byte initrd_ram_file_t.

#include "initrd.h"
#include "../mm/kheap.h"
#include "../mm/pmm.h"
#include "../lib/common.h"
#include "../dev/console.h"
#include "ix/ix_automount.h"
#define malloc kmalloc
#define free   kfree

initrd_header_t *initrd_header;     // The header.
initrd_file_header_t *file_headers; // The list of file headers.
fs_node_t *initrd_root;             // Our root directory node.
fs_node_t *initrd_dev;              // We also add a directory node for /dev.
fs_node_t *root_nodes;              // List of file nodes.
int nroot_nodes;                    // Number of file nodes.

static u64int initrd_base_addr = 0;

/* Capacity of the root_nodes array (>= nroot_nodes). */
static int root_nodes_cap = 0;

struct dirent dirent;

/* ------------------------------------------------------------
 * RAM file helpers
 * --------------------------------------------------------- */

/*
 * Promote a node to RAM-backed storage.
 *
 *   keep_content != 0  -> copy the current image content into
 *                         the fresh RAM buffer (lazy promotion
 *                         on first write to an image file).
 *   keep_content == 0  -> the RAM file starts empty (O_TRUNC
 *                         or a brand-new file).
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int initrd_promote(fs_node_t *node, int keep_content)
{
    initrd_ram_file_t *ram;
    u8int *data = 0;
    u64int len = 0;
    u64int cap = 0;

    if (node->ptr)
        return 0;   /* already RAM-backed */

    if (keep_content) {
        u32int img_len = 0;
        u64int img_off = 0;

        if (node->inode < (u32int)initrd_header->nfiles) {
            img_len = file_headers[node->inode].length;
            img_off = (u64int)initrd_base_addr
                    + file_headers[node->inode].offset;
        }

        if (img_len > 0) {
            cap = (u64int)img_len;
            data = (u8int *)malloc((u32int)cap);
            if (!data)
                return -1;
            memcpy(data, (u8int *)(uintptr_t)img_off, img_len);
            len = img_len;
        }
    }

    ram = (initrd_ram_file_t *)malloc(sizeof(initrd_ram_file_t));
    if (!ram) {
        if (data) free(data);
        return -1;
    }

    ram->data = data;
    ram->len  = len;
    ram->cap  = cap;

    node->ptr    = ram;
    node->write  = &initrd_write;
    node->length = (u32int)ram->len;

    return 0;
}

/*
 * Make sure the RAM file can hold at least `needed` bytes.
 */
static int ram_ensure(initrd_ram_file_t *ram, u64int needed)
{
    if (needed <= ram->cap)
        return 0;

    u64int ncap = ram->cap ? ram->cap : 4096;
    while (ncap < needed)
        ncap *= 2;

    u8int *nd = (u8int *)malloc((u32int)ncap);
    if (!nd)
        return -1;

    memset(nd, 0, (u32int)ncap);

    if (ram->data) {
        memcpy(nd, ram->data, (u32int)ram->len);
        free(ram->data);
    }

    ram->data = nd;
    ram->cap  = ncap;

    return 0;
}

/* ------------------------------------------------------------
 * Node operations
 * --------------------------------------------------------- */

static u32int initrd_read(fs_node_t *node, u32int offset, u32int size, u8int *buffer)
{
    /* RAM-backed (promoted or runtime-created) file first. */
    if (node->ptr) {
        initrd_ram_file_t *ram = (initrd_ram_file_t *)node->ptr;

        if (offset > ram->len)
            return 0;
        if ((u64int)offset + size > ram->len)
            size = (u32int)(ram->len - offset);
        if (size && ram->data)
            memcpy(buffer, ram->data + offset, size);
        return size;
    }

    /* Plain image file. */
    if (node->inode >= (u32int)initrd_header->nfiles)
        return 0;

    initrd_file_header_t header = file_headers[node->inode];
    if (offset > header.length)
        return 0;
    if (offset+size > header.length)
        size = header.length-offset;
    u64int addr = initrd_base_addr + header.offset + offset;
    memcpy(buffer, (u8int*)(uintptr_t)addr, size);
    return size;
}

u32int initrd_write(fs_node_t *node, u32int offset, u32int size, u8int *buffer)
{
    if (!node || size == 0)
        return 0;

    /* Lazy promotion keeps the previous content on first write. */
    if (!node->ptr) {
        if (initrd_promote(node, 1) != 0)
            return 0;
    }

    initrd_ram_file_t *ram = (initrd_ram_file_t *)node->ptr;

    u64int needed = (u64int)offset + size;

    if (ram_ensure(ram, needed) != 0)
        return 0;

    memcpy(ram->data + offset, buffer, size);

    if (needed > ram->len)
        ram->len = needed;

    node->length = (u32int)ram->len;

    return size;
}

/*
 * O_TRUNC support.
 */
int initrd_truncate(fs_node_t *node)
{
    if (!node)
        return -1;

    if (node->ptr) {
        initrd_ram_file_t *ram = (initrd_ram_file_t *)node->ptr;
        ram->len = 0;             /* keep data/cap: cheap re-growth */
        node->length = 0;
        return 0;
    }

    /* Image file -> promote to an empty RAM file. */
    if (initrd_promote(node, 0) != 0)
        return -1;

    node->length = 0;

    return 0;
}

/*
 * Разбор каталога initrd.
 *
 * Индексы:
 *   0        -> "dev"
 *   1        -> "initrd"
 *   2        -> "disk0"   (только если смонтирован)
 *   3..      -> root_nodes[]
 */
static struct dirent *initrd_readdir(fs_node_t *node, u32int index)
{
    if (node == initrd_dev)
    {
        /*
         * /dev: единственный узел, который там реально есть.
         * Раньше сюда проваливался общий код ниже и отдавал
         * root_nodes -- то есть все файлы initrd как "dev/...".
         */
        if (index == 0) {
            strcpy(dirent.name, "console");
            dirent.ino  = 0;
            dirent.type = console_node.flags & 0x7;
            return &dirent;
        }
        return 0;
    }

    if (node == initrd_root)
    {
        int ndisks = automount_count();
        u32int base = 2u + (u32int)ndisks;

        if (index == 0) {
            strcpy(dirent.name, "dev");
            dirent.ino  = 0;
            dirent.type = FS_DIRECTORY;
            return &dirent;
        }
        if (index == 1) {
            strcpy(dirent.name, "initrd");
            dirent.ino  = 0;
            dirent.type = FS_DIRECTORY;
            return &dirent;
        }

        /*
         * Точки монтирования: disk0..disk{N-1}.
         * Раньше здесь был жёстко "disk0" через diskfat_get_root(),
         * теперь -- все диски, смонтированные automount.
         */
        if (index >= 2u && index < base) {
            const char *dn = automount_disk_name((int)(index - 2u));
            if (!dn || !dn[0])
                return 0;
            strcpy(dirent.name, dn);
            dirent.ino  = 0;
            dirent.type = FS_DIRECTORY;
            return &dirent;
        }

        /*
         * Смещение к root_nodes зависит от числа смонтированных
         * дисков.  При отсутствии дисков base==2, и листинг
         * продолжается файлами initrd (а не обрывается).
         */
        index -= base;
    }

    if (index >= (u32int)nroot_nodes)
        return 0;

    strcpy(dirent.name, root_nodes[index].name);
    dirent.ino  = root_nodes[index].inode;
    dirent.type = root_nodes[index].flags & 0x7;
    return &dirent;
}

/*
 * finddir/readdir для каталогов, созданных mkdir в RAM-ФС.
 * Дерево остаётся плоским: дочерние узлы ищутся в root_nodes по
 * имени "родитель/ребёнок".  На образе initrd таких имён нет,
 * поэтому каталог по умолчанию пуст.
 */
static fs_node_t *initrd_dir_holder;

static int initrd_dir_child(const fs_node_t *dir, const char *name,
                            char *out, int cap) {
    int n = 0;

    for (const char *p = (const char *)dir->name; *p; p++)
        if (n < cap - 1) out[n++] = *p;
    if (n < cap - 1) out[n++] = '/';
    for (const char *p = name; *p; p++)
        if (n < cap - 1) out[n++] = *p;
    out[n] = 0;
    return n;
}

static fs_node_t *initrd_dir_finddir(fs_node_t *node, char *name)
{
    char full[256];
    int i;

    if (!node || !name || !name[0])
        return 0;
    if (!strcmp(name, "."))
        return node;
    if (!strcmp(name, ".."))
        return initrd_root;

    initrd_dir_child(node, name, full, (int)sizeof(full));

    for (i = 0; i < nroot_nodes; i++)
        if (!strcmp(full, root_nodes[i].name))
            return &root_nodes[i];

    return 0;
}

static struct dirent *initrd_dir_readdir(fs_node_t *node, u32int index)
{
    char prefix[256];
    u32int seen = 0;
    int plen;

    if (!node)
        return 0;

    plen = initrd_dir_child(node, "", prefix, (int)sizeof(prefix));
    (void)plen;

    for (int i = 0; i < nroot_nodes; i++) {
        const char *nm = (const char *)root_nodes[i].name;
        const char *slash;

        if (strncmp(nm, prefix, strlen(prefix)) != 0)
            continue;
        slash = strchr(nm + strlen(prefix), '/');
        if (slash && slash[1])
            continue;                 /* глубже этого уровня */

        if (seen++ != index)
            continue;

        strcpy(dirent.name, nm + strlen(prefix));
        dirent.ino  = root_nodes[i].inode;
        dirent.type = root_nodes[i].flags & 0x7;
        return &dirent;
    }

    if (index == seen) {
        strcpy(dirent.name, ".");
        dirent.ino = node->inode;
        dirent.type = FS_DIRECTORY;
        return &dirent;
    }
    if (index == seen + 1) {
        strcpy(dirent.name, "..");
        dirent.ino = 0;
        dirent.type = FS_DIRECTORY;
        return &dirent;
    }

    return 0;
}

/*
 * Плоское имя файла initrd.
 *
 * Дерево initrd -- ОДИН уровень: настоящих каталогов нет.
 * Приложения (Doom) склеивают путь как ".savegame/doomsav0.dsg" и
 * зовут open(); ядро обязано превратить это в имя узла
 * "doomsav0.dsg" -- иначе узел создастся с '/' ВНУТРИ имени,
 * finddir по компоненту ".savegame" его не найдёт, и запись
 * файла не будет видна при чтении (слот сохранения "пустой").
 *
 * Берём последний компонент пути; пустой результат заменяем на
 * само имя, чтобы не вернуть "".
 */
static const char *initrd_flat_name(const char *name)
{
    const char *last = name;

    if (!name)
        return name;

    for (const char *p = name; *p; p++)
        if (*p == '/')
            last = p + 1;

    return last;
}

static fs_node_t *initrd_finddir(fs_node_t *node, char *name)
{
    if (node == initrd_root && !strcmp(name, "dev"))
        return initrd_dev;

    /*
     * Точки монтирования.  resolve_path("disk0/foo") вернёт
     * сначала initrd_root, затем initrd_finddir(initrd_root, "foo").
     */
    if (node == initrd_root && !strcmp(name, "initrd"))
        return initrd_root;

    if (node == initrd_root) {
        int ndisks = automount_count();
        for (int i = 0; i < ndisks; i++) {
            const char *dn = automount_disk_name(i);
            if (dn && dn[0] && !strcmp(name, (char *)dn))
                return automount_get_root(i);
        }
    }

    if ((node == initrd_root || node == initrd_dev) && !strcmp(name, "console"))
        return &console_node;

    /*
     * root_nodes -- только файлы самого initrd.  Для initrd_dev
     * их искать нельзя: иначе "initrd/dev/wserver" открывал бы
     * настоящий файл, хотя /dev -- отдельный каталог.
     */
    if (node != initrd_root)
        return 0;

    {
        const char *flat = initrd_flat_name(name);
        int i;

        for (i = 0; i < nroot_nodes; i++)
            if (!strcmp(name, root_nodes[i].name) ||
                (flat && flat[0] && !strcmp(flat, root_nodes[i].name)))
                return &root_nodes[i];
    }

    return 0;
}

fs_node_t *initrd_remove(char *name)
{
    int i;

    if (!name || !name[0])
        return 0;

    {
        const char *flat = initrd_flat_name(name);
        if (flat && flat[0])
            name = (char *)flat;
    }

    for (i = 0; i < nroot_nodes; i++)
        if (!strcmp(name, root_nodes[i].name)) {
            if (root_nodes[i].ptr) {
                initrd_ram_file_t *ram =
                    (initrd_ram_file_t *)root_nodes[i].ptr;
                if (ram->data)
                    free(ram->data);
                free(ram);
                root_nodes[i].ptr = 0;
            }
            memset(&root_nodes[i], 0, sizeof(root_nodes[i]));
            nroot_nodes -= 1;
        }
    return 0;
}

fs_node_t *initrd_move(char *name, char *new_name)
{
    int i;

    if (!name || !new_name || !name[0] || !new_name[0])
        return 0;

    /* Имя должно помещаться в fs_node_t::name. */
    if (strlen(new_name) >= 128)
        return 0;

    for (i = 0; i < nroot_nodes; i++)
        if (!strcmp(name, root_nodes[i].name))
            strcpy(root_nodes[i].name, new_name);

    return 0;
}

/*
 * Переименование с заменой (POSIX rename справляется с уже
 * существующим приёмником).
 *
 * Doom пишет сохранение во временный файл temp.dsg и лишь потом
 * переименовывает его в doomsav0.dsg.  Старое сохранение при этом
 * должно исчезнуть, поэтому: убираем приёмник, если он есть, и
 * имя источника присваиваем свободному узлу.
 */
int initrd_rename(const char *from, const char *to)
{
    int src = -1;
    int i;

    if (!from || !to || !from[0] || !to[0])
        return -1;

    /* Плоское дерево: сравниваем и записываем последний компонент. */
    {
        const char *fa = initrd_flat_name(from);
        const char *fb = initrd_flat_name(to);

        if (fa && fa[0]) from = fa;
        if (fb && fb[0]) to   = fb;
    }

    if (strlen(to) >= 128)
        return -1;

    for (i = 0; i < nroot_nodes; i++)
        if (!strcmp((char *)from, root_nodes[i].name)) {
            src = i;
            break;
        }

    if (src < 0)
        return -1;

    /* Приёмник уже есть -- освобождаем его данные. */
    for (i = 0; i < nroot_nodes; i++) {
        if (i == src)
            continue;

        if (!strcmp((char *)to, root_nodes[i].name)) {

            if (root_nodes[i].ptr) {
                initrd_ram_file_t *ram =
                    (initrd_ram_file_t *)root_nodes[i].ptr;

                if (ram->data)
                    free(ram->data);

                free(ram);
                root_nodes[i].ptr = 0;
            }

            memset(&root_nodes[i], 0, sizeof(root_nodes[i]));
            nroot_nodes -= 1;

            /*
             * После удаления хвост массива сдвинулся: если
             * удалённый узел стоял ПЕРЕД источником, индекс
             * источника уменьшился на единицу.
             */
            if (i < src)
                src--;

            break;
        }
    }

    if (src < 0 || src >= nroot_nodes)
        return -1;

    strcpy(root_nodes[src].name, (char *)to);

    return 0;
}

/*
 * Create a new empty file at runtime (open with O_CREAT).
 */
fs_node_t *initrd_create(const char *name)
{
    if (!name || !name[0])
        return 0;

    /*
     * Плоское дерево: узлу достаётся ТОЛЬКО последний компонент
     * пути.  Иначе open(".savegame/doomsav0.dsg") создавал узел с
     * '/' внутри имени, finddir по компоненту ".savegame" его не
     * находил, и записанный файл "не существовал" для чтения --
     * ровно та причина, по которой Doom не видел слот сохранения.
     */
    {
        const char *flat = initrd_flat_name(name);

        if (flat && flat[0])
            name = flat;
    }

    /* Refuse duplicates: use the existing node instead. */
    for (int i = 0; i < nroot_nodes; i++)
        if (!strcmp(name, root_nodes[i].name))
            return &root_nodes[i];

    if (nroot_nodes >= root_nodes_cap) {
        int ncap = root_nodes_cap ? root_nodes_cap * 2 : 16;
        fs_node_t *nn = (fs_node_t *)malloc(sizeof(fs_node_t) * ncap);
        if (!nn)
            return 0;
        if (root_nodes) {
            memcpy(nn, root_nodes, sizeof(fs_node_t) * root_nodes_cap);
            free(root_nodes);
        }
        root_nodes     = nn;
        root_nodes_cap = ncap;
    }

    fs_node_t *node = &root_nodes[nroot_nodes];

    strcpy(node->name, (char *)name);
    node->mask = node->uid = node->gid = 0;
    node->length = 0;
    node->inode = nroot_nodes;
    node->flags = FS_FILE;
    node->program_type = 0;
    node->impl = FS_IMPL_INITRD;
    node->read   = &initrd_read;
    node->write  = &initrd_write;
    node->readdir = 0;
    node->finddir = 0;
    node->open   = 0;
    node->close  = 0;
    node->ptr    = 0;

    /* Start with an empty RAM file so write-before-read works. */
    if (initrd_promote(node, 0) != 0)
        return 0;

    nroot_nodes++;

    debug_puts("INITRD: created node: ");
    debug_puts(node->name);
    debug_putc('\n');

    return node;
}

/*
 * Создать узел-каталог в root_nodes (для mkdir в RAM-ФС).
 * У каталога свой finddir/readdir: дочерние записи ищутся
 * непосредственно в root_nodes по имени == "родитель/ребёнок".
 * На initrd-образе таких имён нет, поэтому узел пуст, но
 * созданные в нём файлы (open с O_CREAT) ложатся в плоский
 * root_nodes и остаются доступны по своему имени.
 */
static fs_node_t *initrd_create_dir(const char *name)
{
    fs_node_t *node;

    if (!name || !name[0] || strlen(name) >= 128)
        return 0;

    if (nroot_nodes >= root_nodes_cap) {
        int ncap = root_nodes_cap ? root_nodes_cap * 2 : 16;
        fs_node_t *nn = (fs_node_t *)malloc(sizeof(fs_node_t) * ncap);
        if (!nn)
            return 0;
        if (root_nodes) {
            memcpy(nn, root_nodes, sizeof(fs_node_t) * root_nodes_cap);
            free(root_nodes);
        }
        root_nodes     = nn;
        root_nodes_cap = ncap;
    }

    node = &root_nodes[nroot_nodes];
    memset(node, 0, sizeof(*node));
    strcpy(node->name, (char *)name);
    node->length = 0;
    node->inode = nroot_nodes;
    node->flags = FS_DIRECTORY;
    node->program_type = 0;
    node->impl = FS_IMPL_INITRD;
    node->read   = 0;
    node->write  = 0;
    node->readdir = &initrd_dir_readdir;
    node->finddir = &initrd_dir_finddir;
    node->open   = 0;
    node->close  = 0;
    node->ptr    = 0;

    nroot_nodes++;
    return node;
}

fs_node_t *initialise_initrd(u64int location)
{
    initrd_base_addr = location;
    initrd_header = (initrd_header_t *)(uintptr_t)location;
    file_headers = (initrd_file_header_t *)(uintptr_t)(location+sizeof(initrd_header_t));

    initrd_root = (fs_node_t*)malloc(sizeof(fs_node_t));
    strcpy(initrd_root->name, "initrd");
    initrd_root->mask = initrd_root->uid = initrd_root->gid = initrd_root->inode = initrd_root->length = 0;
    initrd_root->flags = FS_DIRECTORY;
    initrd_root->program_type = 0;
    initrd_root->impl = FS_IMPL_INITRD;
    initrd_root->read = 0;
    initrd_root->write = 0;
    initrd_root->open = 0;
    initrd_root->close = 0;
    initrd_root->readdir = &initrd_readdir;
    initrd_root->finddir = &initrd_finddir;
    initrd_root->ptr = 0;

    initrd_dev = (fs_node_t*)malloc(sizeof(fs_node_t));
    strcpy(initrd_dev->name, "dev");
    initrd_dev->mask = initrd_dev->uid = initrd_dev->gid = initrd_dev->inode = initrd_dev->length = 0;
    initrd_dev->flags = FS_DIRECTORY;
    initrd_dev->program_type = 0;
    initrd_dev->impl = FS_IMPL_INITRD;
    initrd_dev->read = 0;
    initrd_dev->write = 0;
    initrd_dev->open = 0;
    initrd_dev->close = 0;
    initrd_dev->readdir = &initrd_readdir;
    initrd_dev->finddir = &initrd_finddir;
    initrd_dev->ptr = 0;

    root_nodes_cap = (int)initrd_header->nfiles;
    if (root_nodes_cap < 16)
        root_nodes_cap = 16;

    root_nodes = (fs_node_t*)malloc(sizeof(fs_node_t) * root_nodes_cap);
    nroot_nodes = initrd_header->nfiles;

    int i;
    for (i = 0; i < initrd_header->nfiles; i++)
    {
        strcpy(root_nodes[i].name, (char *)&file_headers[i].name);
        root_nodes[i].mask = root_nodes[i].uid = root_nodes[i].gid = 0;
        root_nodes[i].length = file_headers[i].length;
        root_nodes[i].inode = i;
        root_nodes[i].flags = FS_FILE;
        root_nodes[i].program_type = file_headers[i].app_type;
        root_nodes[i].impl = FS_IMPL_INITRD;
        root_nodes[i].read = &initrd_read;
        root_nodes[i].write = 0;
        root_nodes[i].readdir = 0;
        root_nodes[i].finddir = 0;
        root_nodes[i].open = 0;
        root_nodes[i].close = 0;
        root_nodes[i].ptr = 0;
    }
    return initrd_root;
}

/*
 * mkdir для initrd.
 *
 * Ищем первый компонент пути.  Если он уже есть:
 *   - файл     -- ошибка (EEXIST), как в POSIX;
 *   - каталог  -- успех (повторный mkdir -p идемпотентен).
 * Создаём узел-каталог в root_nodes: у него свои finddir/readdir,
 * поэтому вложенные пути вида "docs" реально открываются, а не
 * сваливаются в плоское имя.
 *
 * Регистр компонента сохраняется; finddir дополнительно ищет без
 * учёта регистра, чтобы "Docs" и "docs" не размножались.
 */
int initrd_mkdir(const char *path)
{
    const char *p;
    char  name[128];
    int   n = 0;
    fs_node_t *node;
    int i;

    if (!path || !path[0])
        return -1;

    p = path;
    while (*p == '/')
        p++;
    if (!*p)
        return 0;               /* "/" уже существует */

    while (*p && *p != '/' && n < (int)sizeof(name) - 1)
        name[n++] = *p++;
    name[n] = 0;
    if (n == 0)
        return -1;

    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return 0;

    if (!strcmp(name, "dev") || !strcmp(name, "initrd"))
        return !*p ? 0 : -1;

    for (i = 0; i < nroot_nodes; i++) {
        if (root_nodes[i].name[0] == 0)
            continue;
        if (strcmp(name, root_nodes[i].name) != 0)
            continue;

        if ((root_nodes[i].flags & 0x7) == FS_DIRECTORY)
            return 0;           /* уже каталог */
        return -1;              /* есть файл с таким именем */
    }

    node = initrd_create_dir(name);
    if (!node)
        return -1;

    return 0;
}
