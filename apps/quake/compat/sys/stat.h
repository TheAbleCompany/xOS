/*
 * sys/stat.h -- обёртка вокруг mini-libc <sys/stat.h>.
 *
 * mini-libc определяет __S_IFMT/__S_IFDIR, но не S_IFDIR/S_ISDIR,
 * которые использует common.c TyrQuake.  include_next берёт
 * настоящий заголовок из ../mini-libc/include.
 */
#ifndef AX_COMPAT_SYS_STAT_H
#define AX_COMPAT_SYS_STAT_H

#include_next <sys/stat.h>

#ifndef S_IFMT
#define S_IFMT  __S_IFMT
#endif
#ifndef S_IFDIR
#define S_IFDIR __S_IFDIR
#endif
#ifndef S_IFREG
#define S_IFREG __S_IFREG
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & __S_IFMT) == __S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & __S_IFMT) == __S_IFREG)
#endif

#endif /* AX_COMPAT_SYS_STAT_H */
