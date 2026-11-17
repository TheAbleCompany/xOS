#include "../lib/common.h"
#include "../fs/fs.h"
#include "../fs/file.h"
#include "../multitask/task.h"

#define AX_S_IFMT  0170000
#define AX_S_IFREG 0100000
#define AX_S_IFDIR 0040000
#define AX_S_IFCHR 0020000
#define AX_S_IFBLK 0060000
#define AX_S_IFIFO 0010000
#define AX_S_IFLNK 0120000

file_t *get_file(int fd);

int ax_mesa_current_pid(void)
{
    return runningTask ? runningTask->pid : 0;
}

static unsigned int ax_node_mode(const fs_node_t *node)
{
    switch (node->flags & 0x0Fu) {
    case FS_FILE:
        return AX_S_IFREG | 0644u;
    case FS_DIRECTORY:
        return AX_S_IFDIR | 0755u;
    case FS_CHARDEVICE:
        return AX_S_IFCHR | 0666u;
    case FS_BLOCKDEVICE:
        return AX_S_IFBLK | 0660u;
    case FS_PIPE:
        return AX_S_IFIFO | 0600u;
    case FS_SYMLINK:
        return AX_S_IFLNK | 0777u;
    case FS_MOUNTPOINT:
        return AX_S_IFDIR | 0755u;
    default:
        return AX_S_IFREG | 0644u;
    }
}

int ax_mesa_fd_stat(int fd, unsigned long *dev, unsigned long *ino,
                    unsigned int *mode, long *size)
{
    file_t *f = get_file(fd);

    if (!f || !f->node)
        return -1;

    if (dev)
        *dev = 0;

    if (ino)
        *ino = (unsigned long)f->node->inode;

    if (mode)
        *mode = ax_node_mode(f->node);

    if (size)
        *size = (long)f->node->length;

    return 0;
}
