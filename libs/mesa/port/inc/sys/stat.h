#ifndef MESA_PORT_SYS_STAT_H
#define MESA_PORT_SYS_STAT_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
typedef unsigned int mode_t2;
struct stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int  st_mode;
    unsigned int  st_nlink;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned long st_rdev;
    long          st_size;
    long          st_blksize;
    long          st_blocks;
    long          st_atime;
    long          st_mtime;
    long          st_ctime;
};
int stat(const char *p, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *p, unsigned int mode);
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

#ifdef __cplusplus
}
#endif
#endif
