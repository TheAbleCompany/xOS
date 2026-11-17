// fs.h -- Defines the interface for and structures relating to the virtual file system.
//         Written for JamesM's kernel development tutorials.
//         Extended: multi-FS path resolution (initrd + disk0 mount points).

#ifndef FS_H
#define FS_H

#include "../lib/common.h"

#define FS_FILE        0x01
#define FS_DIRECTORY   0x02
#define FS_CHARDEVICE  0x03
#define FS_BLOCKDEVICE 0x04
#define FS_PIPE        0x05
#define FS_SYMLINK     0x06
#define FS_MOUNTPOINT  0x08

/*
 * Тип программы.
 */
#define PROGRAM_NONE    0
#define PROGRAM_CONSOLE 1
#define PROGRAM_GUI     2

/*
 * Идентификатор файловой системы, которой принадлежит узел.
 * Используется open.c для выбора правильной truncate/create-функции,
 * а также любым кодом, которому нужно отличить RAM-файл initrd
 * от файла на реальном FAT-разделе.
 */
#define FS_IMPL_NONE     0
#define FS_IMPL_INITRD   1
#define FS_IMPL_DISKFAT  2
#define FS_IMPL_DISKAUTO 3

struct fs_node;

typedef u32int (*read_type_t)(struct fs_node*,u32int,u32int,u8int*);
typedef u32int (*write_type_t)(struct fs_node*,u32int,u32int,u8int*);
typedef void (*open_type_t)(struct fs_node*);
typedef void (*close_type_t)(struct fs_node*);
typedef struct dirent * (*readdir_type_t)(struct fs_node*,u32int);
typedef struct fs_node * (*finddir_type_t)(struct fs_node*,char *name);

typedef struct fs_node
{
    char name[128];

    u32int mask;
    u32int uid;
    u32int gid;

    u32int flags;
    u32int inode;
    u32int length;

    /* Тип исполняемой программы */
    u32int program_type;

    /* Идентификатор ФС (FS_IMPL_*). */
    u32int impl;

    read_type_t read;
    write_type_t write;
    open_type_t open;
    close_type_t close;
    readdir_type_t readdir;
    finddir_type_t finddir;

    struct fs_node *ptr;

} fs_node_t;

struct dirent
{
    char name[128]; // Filename.
    u32int ino;     // Inode number.
    u32int type;    // FS object type.
};

struct linux_dirent {
    u32int d_ino;
    u16int d_reclen;
    u8int  d_type;
    char   d_name[];
};

extern fs_node_t *fs_root; // The root of the filesystem.

u32int read_fs(fs_node_t *node, u32int offset, u32int size, u8int *buffer);
u32int write_fs(fs_node_t *node, u32int offset, u32int size, u8int *buffer);
void open_fs(fs_node_t *node, u8int read, u8int write);
void close_fs(fs_node_t *node);
struct dirent *readdir_fs(fs_node_t *node, u32int index);
fs_node_t *finddir_fs(fs_node_t *node, char *name);

/*
 * Разбор пути с ведущими '/' и промежуточными каталогами.
 * Возвращает узел конечного компонента либо 0.
 *
 * Примеры:
 *   "foo.txt"          -> initrd/foo.txt
 *   "initrd/foo.txt"   -> initrd/foo.txt
 *   "disk0/foo.txt"    -> disk0/foo.txt
 *   "disk0/sub/bar"    -> disk0/sub/bar (если disk0 умеет подкаталоги)
 *   "/"                -> fs_root
 */
fs_node_t *resolve_path(const char *path);

/*
 * Тип программы по пути (syscall 197 FP_TYPE):
 *   -1 / PROGRAM_NONE / PROGRAM_CONSOLE / PROGRAM_GUI.
 * console-ELF'ы файловый менеджер запускает в терминале.
 */
extern int fp_type(const char *path);

// Внешняя ссылка на корень devfs
extern fs_node_t *devfs_root;

fs_node_t *devfs_get_node(const char *path);
#endif