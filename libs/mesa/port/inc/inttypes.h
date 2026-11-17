#ifndef MESA_PORT_INTTYPES_H
#define MESA_PORT_INTTYPES_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#define PRId8 "d"
#define PRIi8 "i"
#define PRIu8 "u"
#define PRIx8 "x"
#define PRId16 "d"
#define PRIi16 "i"
#define PRIu16 "u"
#define PRIx16 "x"
#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIX64 "llX"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
typedef intmax_t imaxdiv_t;

#ifndef __INTMAX_TYPE__
typedef long long intmax_t;
#endif
#ifndef __UINTMAX_TYPE__
typedef unsigned long long uintmax_t;
#endif
#ifndef __INTMAX_TYPE__
typedef struct { intmax_t quot; intmax_t rem; } imaxdiv_t;
#endif
intmax_t imaxabs(intmax_t n);
imaxdiv_t imaxdiv(intmax_t n, intmax_t d);
intmax_t strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);
intmax_t wcstoimax(const __WCHAR_TYPE__ *s, __WCHAR_TYPE__ **end, int base);
uintmax_t wcstoumax(const __WCHAR_TYPE__ *s, __WCHAR_TYPE__ **end, int base);

#define PRIdMAX "lld"
#define PRIiMAX "lli"
#define PRIoMAX "llo"
#define PRIuMAX "llu"
#define PRIxMAX "llx"
#define PRIXMAX "llX"
#define SCNdMAX "lld"
#define SCNiMAX "lli"
#define SCNuMAX "llu"
#define SCNxMAX "llx"
#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#ifdef __cplusplus
}
#endif
#endif
