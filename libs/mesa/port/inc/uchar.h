#ifndef MESA_PORT_UCHAR_H
#define MESA_PORT_UCHAR_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef __cplusplus
typedef unsigned short char16_t;
typedef unsigned int char32_t;
#else
#ifndef __CHAR16_TYPE__
#define __CHAR16_TYPE__ char16_t
#endif
#endif
typedef struct { int dummy; } mbstate_t;
size_t mbrtoc16(char16_t *pc, const char *s, size_t n, mbstate_t *ps);
size_t c16rtomb(char *s, char16_t c, mbstate_t *ps);
size_t mbrtoc32(char32_t *pc, const char *s, size_t n, mbstate_t *ps);
size_t c32rtomb(char *s, char32_t c, mbstate_t *ps);
#ifdef __cplusplus
}
#endif
#endif
