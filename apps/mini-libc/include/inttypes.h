/*
 * include/inttypes.h — mini-libc (ArtyomX64).
 *
 * Минимальный набор для кода, который тянет <inttypes.h> ради
 * типов фиксированной ширины. Спецификаторы printf("%PRId64")
 * на x86_64 LP64 совпадают с обычными "l"-вариантами.
 */

#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

typedef struct {
        intmax_t quot;
        intmax_t rem;
} imaxdiv_t;

#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "ld"
#define PRIi8   "i"
#define PRIi16  "i"
#define PRIi32  "i"
#define PRIi64  "li"
#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  "lu"
#define PRIx8   "x"
#define PRIx16  "x"
#define PRIx32  "x"
#define PRIx64  "lx"
#define PRIX8   "X"
#define PRIX16  "X"
#define PRIX32  "X"
#define PRIX64  "lX"
#define PRIo8   "o"
#define PRIo16  "o"
#define PRIo32  "o"
#define PRIo64  "lo"

#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIoPTR "lo"
#define PRIdMAX "ld"
#define PRIiMAX "li"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
#define PRIoMAX "lo"

#define SCNd8   "hhd"
#define SCNd16  "hd"
#define SCNd32  "d"
#define SCNd64  "ld"
#define SCNi8   "hhi"
#define SCNi16  "hi"
#define SCNi32  "i"
#define SCNi64  "li"
#define SCNu8   "hhu"
#define SCNu16  "hu"
#define SCNu32  "u"
#define SCNu64  "lu"
#define SCNx8   "hhx"
#define SCNx16  "hx"
#define SCNx32  "x"
#define SCNx64  "lx"

#define SCNdPTR "ld"
#define SCNdMAX "ld"
#define SCNuPTR "lu"
#define SCNuMAX "lu"

#endif /* _INTTYPES_H */
