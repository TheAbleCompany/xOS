/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef __UNISTD_H__
#define __UNISTD_H__    1

#ifdef __cplusplus
extern "C" {
#endif

#include <internal/io.h>
#include <internal/types.h>
/*
 * glibc тянет типы фиксированной ширины (int8_t и др.) в
 * пользовательский код через sys/types.h; у run.c (llama2.c)
 * int8_t приходит именно так — из <unistd.h>.
 */
#include <stdint.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define SEEK_DATA 3
#define SEEK_HOLE 4

int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
char *getcwd(char *buf, size_t size);
int truncate(const char *path, off_t length);
int ftruncate(int fd, off_t length);
unsigned int sleep(unsigned int seconds);
int unlink(const char *pathname);
int access(const char *pathname, int mode);
int isatty(int fd);

struct ax_dirent {
        unsigned int   d_ino;
        unsigned short d_reclen;
        unsigned char  d_type;
        char           d_name[];
};

int getdents(int fd, void *buf, unsigned int size);

#ifdef __cplusplus
}
#endif

#endif
