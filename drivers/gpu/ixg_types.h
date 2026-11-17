#ifndef IXG_TYPES_H
#define IXG_TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   ix_u8;
typedef uint16_t  ix_u16;
typedef uint32_t  ix_u32;
typedef uint64_t  ix_u64;
typedef int8_t    ix_s8;
typedef int16_t   ix_s16;
typedef int32_t   ix_s32;
typedef int64_t   ix_s64;
typedef uintptr_t ix_uptr;
typedef size_t    ix_size;

typedef ix_u8  ix_bool8;

#define IXG_TRUE  1u
#define IXG_FALSE 0u

#endif