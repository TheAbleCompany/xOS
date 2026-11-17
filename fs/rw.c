/*
 * ============================================================
 * fs/rw.c -- read() / write() / close() syscalls (3 / 4 / 6)
 *            and the per-task file table accessor get_file().
 * ============================================================
 *
 * All descriptors live in the calling task's file table
 * (Task->files, managed by multitask/task.c).  fd 0..2 are
 * stdio and normally point at the controlling PTY slave.
 *
 * Semantics kept for user-space compatibility:
 *
 *   * read() returns the number of bytes actually read; 0 means
 *     EOF for regular files and "no data yet" for PTYs (the
 *     user-space wrapper in lib/unistd.h loops on fd 0);
 *   * write() returns bytes written or -1;
 *   * close() releases the file_t object.
 */

#include "fs.h"
#include "file.h"
#include "sock.h"
#include "../multitask/task.h"
#include "../lib/common.h"

#include <stdint.h>

/*
 * Resolve a descriptor for the currently running task.
 * Shared with lib/syscalls.c (lseek/getdents/ioctl).
 */
file_t *get_file(int fd)
{
    if (!runningTask)
        return 0;

    if (fd < 0)
        return 0;

    return task_get_file(runningTask, fd);
}

int read(int fd, char *buf, int nbytes)
{
    if (nbytes <= 0)
        return 0;

    if (runningTask && fd >= 0 && fd < TASK_MAX_FDS && runningTask->socks[fd])
        return ax_sock_recvfrom(fd, buf, (u32int)nbytes, 0, 0, 0);

    file_t *f = get_file(fd);

    if (!f || !f->node)
        return -1;

    u32int n = read_fs(f->node, f->pos, (u32int)nbytes, (u8int *)buf);

    f->pos += n;

    return (int)n;
}

int write(int fd, char *buf, int nbytes)
{
    if (nbytes <= 0)
        return 0;

    if (runningTask && fd >= 0 && fd < TASK_MAX_FDS && runningTask->socks[fd])
        return ax_sock_sendto(fd, buf, (u32int)nbytes, 0, 0, 0);

    file_t *f = get_file(fd);

    if (!f || !f->node)
        return -1;

    u32int n = write_fs(f->node, f->pos, (u32int)nbytes, (u8int *)buf);

    f->pos += n;

    return (int)n;
}

int close(int fd)
{
    if (!runningTask)
        return -1;

    if (fd < 0)
        return -1;

    if (!task_get_file(runningTask, fd) &&
        !(fd >= 0 && fd < TASK_MAX_FDS && runningTask->socks[fd]))
        return -1;

    task_close_fd(runningTask, fd);

    return 0;
}
