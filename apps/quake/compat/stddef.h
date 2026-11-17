/*
 * stddef.h -- обёртка вокруг mini-libc <stddef.h>.
 *
 * mini-libc определяет только size_t/ssize_t/off_t, а исходникам
 * Quake нужны ptrdiff_t (cl_main.c) и wchar_t.  include_next берёт
 * настоящий заголовок из ../mini-libc/include.
 */
#ifndef AX_COMPAT_STDDEF_H
#define AX_COMPAT_STDDEF_H

#include_next <stddef.h>

#ifndef _PTRDIFF_T_DEFINED
typedef long ptrdiff_t;
#define _PTRDIFF_T_DEFINED
#endif

#ifndef _WCHAR_T_DEFINED
typedef int wchar_t;
#define _WCHAR_T_DEFINED
#endif

#endif /* AX_COMPAT_STDDEF_H */
