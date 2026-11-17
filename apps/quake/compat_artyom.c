/*
 * compat_artyom.c -- мелочи, которых нет в mini-libc.
 *
 *  * opendir/readdir/closedir -- заглушки: initrd плоский, каталогов
 *    нет, поэтому "нет записей" -- корректный ответ.  Quake всё
 *    равно читает данные из pak0.pak, а не из каталогов.
 */

#include <stddef.h>

#include "dirent.h"

/* ------------------------------------------------------------------ */
/* Каталоги: в initrd их нет.                                         */
/* ------------------------------------------------------------------ */

DIR *
opendir(const char *name)
{
    (void)name;
    return NULL;
}

struct dirent *
readdir(DIR *dirp)
{
    (void)dirp;
    return NULL;
}

int
closedir(DIR *dirp)
{
    (void)dirp;
    return 0;
}
