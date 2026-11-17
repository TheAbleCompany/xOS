#include "fs.h"
#include "file.h"
#include "../fs/ix/ix_automount.h"
#include "../multitask/task.h"
#include "../mm/kheap.h"
#include "../lib/common.h"

#include <stdint.h>

#define malloc kmalloc
#define free   kfree

#ifndef O_RDONLY
#define O_RDONLY 0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY 0x0001
#endif
#ifndef O_RDWR
#define O_RDWR   0x0002
#endif
#ifndef O_CREAT
#define O_CREAT  0x0040
#endif
#ifndef O_TRUNC
#define O_TRUNC  0x0200
#endif

const char *skip_leading_slashes(const char *path);

file_t *make_file(fs_node_t *node, u32int flags);

typedef struct {
    int      idx;
    u32int   mount;
    char     rel[192];
} ax_disk_path_t;

static int ax_disk_split(const char *name, ax_disk_path_t *out)
{
    int num = 0;
    const char *p;

    if (!name || !out)
        return -1;

    if (strncmp(name, "disk", 4) != 0)
        return -1;

    p = name + 4;
    if (*p < '0' || *p > '9')
        return -1;

    while (*p >= '0' && *p <= '9') {
        num = num * 10 + (*p - '0');
        p++;
        if (num > 99)
            return -1;
    }

    if (*p != '/' && *p != 0)
        return -1;
    if (*p == '/')
        p++;

    if (num < 0 || num >= AUTOMOUNT_MAX_DISKS || num >= automount_count())
        return -1;

    out->idx = num;
    out->mount = automount_fs_type(num);

    {
        u32int i = 0;
        while (p[i] && i + 1u < sizeof(out->rel)) {
            out->rel[i] = p[i];
            i++;
        }
        out->rel[i] = 0;
        if (i == 0) {
            out->rel[0] = '.';
            out->rel[1] = 0;
        }
    }

    return 0;
}

int ax_disk_split_path(const char *name, int *idx, char *rel, size_t cap)
{
    ax_disk_path_t dp;
    size_t i = 0;

    if (!rel || cap == 0)
        return -1;

    if (ax_disk_split(name, &dp) != 0)
        return -1;

    while (dp.rel[i] && i + 1u < cap) {
        rel[i] = dp.rel[i];
        i++;
    }

    rel[i] = 0;

    if (idx)
        *idx = dp.idx;

    return 0;
}

static int ax_disk_open_path(const char *name, int flags)
{
    ax_disk_path_t dp;
    fs_node_t *node;
    int fd;
    file_t *f;

    if (ax_disk_split(name, &dp) != 0)
        return -1;

    node = resolve_path(name);

    if (!node) {
        if (!(flags & O_CREAT))
            return -1;

        if (!automount_writeable(dp.idx))
            return -1;

        node = automount_create_at(dp.idx, dp.rel);
        if (!node)
            return -1;
    }

    if ((flags & (O_WRONLY | O_RDWR)) && !automount_writeable(dp.idx) && !node->write)
        return -1;

    if ((flags & O_TRUNC) && (node->flags & 0x7) == FS_FILE) {
        if (!automount_writeable(dp.idx))
            return -1;
        if (automount_truncate_at(dp.idx, node) != 0)
            return -1;
    }

    fd = task_alloc_fd(runningTask);
    if (fd < 0)
        return -1;

    f = make_file(node, (u32int)flags);
    if (!f)
        return -1;

    task_set_file(runningTask, fd, f);
    return fd;
}

int open(const char *path, int flags, int mode)
{
    const char *name;

    (void)mode;

    if (!path || !path[0] || !runningTask)
        return -1;

    name = skip_leading_slashes(path);
    if (!name || !name[0])
        return -1;

    if (strncmp(name, "disk", 4) == 0 && name[4] >= '0' && name[4] <= '9')
        return ax_disk_open_path(name, flags);

    {
        fs_node_t *node = resolve_path(name);
        int fd;
        file_t *f;

        if (!node) {
            if (!(flags & O_CREAT))
                return -1;

            node = initrd_create(name);
            if (!node)
                return -1;
        }

        if ((flags & O_TRUNC) && (node->flags & 0x7) == FS_FILE) {
            if (initrd_truncate(node) != 0)
                return -1;
        }

        fd = task_alloc_fd(runningTask);
        if (fd < 0)
            return -1;

        f = make_file(node, (u32int)flags);
        if (!f)
            return -1;

        task_set_file(runningTask, fd, f);
        return fd;
    }
}

file_t *make_file(fs_node_t *node, u32int flags)
{
    file_t *f = (file_t *)malloc(sizeof(file_t));

    if (!f)
        return 0;

    f->node  = node;
    f->pos   = 0;
    f->flags = flags;

    return f;
}
