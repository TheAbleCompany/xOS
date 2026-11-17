// fs.c -- Defines the interface for and structures relating to the virtual file system.
//         Written for JamesM's kernel development tutorials.

#include "fs.h"

fs_node_t *fs_root = 0; // The root of the filesystem.

u32int read_fs(fs_node_t *node, u32int offset, u32int size, u8int *buffer)
{
    // Has the node got a read callback?
    if (node->read != 0)
        return node->read(node, offset, size, buffer);
    else
        return 0;
}

u32int write_fs(fs_node_t *node, u32int offset, u32int size, u8int *buffer)
{
    // Has the node got a write callback?
    if (node->write != 0)
        return node->write(node, offset, size, buffer);
    else
        return 0;
}

void open_fs(fs_node_t *node, u8int read, u8int write)
{
    // Has the node got an open callback?
    if (node->open != 0)
        return node->open(node);
}

void close_fs(fs_node_t *node)
{
    // Has the node got a close callback?
    if (node->close != 0)
        return node->close(node);
}

struct dirent *readdir_fs(fs_node_t *node, u32int index)
{
    // Is the node a directory, and does it have a callback?
    if ( (node->flags&0x7) == FS_DIRECTORY &&
         node->readdir != 0 )
        return node->readdir(node, index);
    else
        return 0;
}

fs_node_t *finddir_fs(fs_node_t *node, char *name)
{
    // Is the node a directory, and does it have a callback?
    if ( (node->flags&0x7) == FS_DIRECTORY &&
         node->finddir != 0 )
        return node->finddir(node, name);
    else
        return 0;
}


/*
 * Разбирает путь вида "disk0/foo/bar.txt".
 * Возвращает fs_node_t* конечного компонента или 0.
 *
 * Особенности:
 *  - ведущие '/' игнорируются;
 *  - компонент "" (пустой, между двумя '/') игнорируется;
 *  - ".." пока не поддерживается (можно добавить позже);
 *  - финальный компонент может быть каталогом или файлом.
 */
fs_node_t *resolve_path(const char *path)
{
    if (!path || !path[0] || !fs_root)
        return 0;

    /* Пропускаем ведущие '/' */
    while (*path == '/')
        path++;

    if (!*path)
        return fs_root;

    fs_node_t *cur = fs_root;

    char name[128];
    int  n = 0;

    for (;;) {
        char c = *path++;

        if (c == '/' || c == 0) {
            if (n > 0) {
                name[n] = 0;

                fs_node_t *next = finddir_fs(cur, name);
                if (!next)
                    return 0;

                cur = next;
                n = 0;
            }

            if (c == 0)
                return cur;
        } else {
            if (n < (int)sizeof(name) - 1)
                name[n++] = c;
        }
    }
}