/*
 * dirent.h -- заглушка POSIX-каталогов для ArtyomX64.
 *
 * В initrd нет каталогов (имена плоские), поэтому opendir() всегда
 * возвращает NULL.  Объявления нужны, чтобы common.c (COM_ScanDir)
 * компилировался без изменений.
 */
#ifndef AX_COMPAT_DIRENT_H
#define AX_COMPAT_DIRENT_H

#include <stddef.h>

typedef struct ax_dir DIR;

struct dirent {
    unsigned long  d_ino;
    unsigned char  d_type;
    char           d_name[256];
};

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#endif /* AX_COMPAT_DIRENT_H */
