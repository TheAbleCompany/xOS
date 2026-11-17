/*
 * strings.h -- совместимость для DOOM на ArtyomX64.
 *
 * mini-libc не предоставляет <strings.h>; doomtype.h включает его
 * ради strcasecmp/strncasecmp.  Реализация -- в doom_artyom.c.
 */

#ifndef AX_COMPAT_STRINGS_H
#define AX_COMPAT_STRINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* AX_COMPAT_STRINGS_H */
