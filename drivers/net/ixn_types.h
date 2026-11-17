#ifndef IXN_TYPES_H
#define IXN_TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   ixn_u8;
typedef uint16_t  ixn_u16;
typedef uint32_t  ixn_u32;
typedef uint64_t  ixn_u64;
typedef int8_t    ixn_s8;
typedef int16_t   ixn_s16;
typedef int32_t   ixn_s32;
typedef int64_t   ixn_s64;
typedef uintptr_t ixn_uptr;
typedef size_t    ixn_size;

typedef ixn_u8  ixn_bool8;

#define IXN_TRUE  1u
#define IXN_FALSE 0u

#define IXN_NULL  ((void*)0)

#define IXN_ETH_ALEN   6u
#define IXN_ETH_HLEN   14u
#define IXN_IPV4_ALEN  4u

#define IXN_htons(x) (ixn_u16)((((ixn_u16)(x) & 0x00FFu) << 8u) | (((ixn_u16)(x) & 0xFF00u) >> 8u))
#define IXN_htonl(x) (ixn_u32)((((ixn_u32)(x) & 0x000000FFu) << 24u) | (((ixn_u32)(x) & 0x0000FF00u) << 8u) | (((ixn_u32)(x) & 0x00FF0000u) >> 8u) | (((ixn_u32)(x) & 0xFF000000u) >> 24u))
#define IXN_ntohs IXN_htons
#define IXN_ntohl IXN_htonl

static inline ixn_u16 ixn_le16(ixn_u16 x) { return x; }
static inline ixn_u32 ixn_le32(ixn_u32 x) { return x; }
static inline ixn_u16 ixn_be16_load(const ixn_u8* p) { return (ixn_u16)(((ixn_u16)p[0] << 8u) | (ixn_u16)p[1]); }
static inline ixn_u32 ixn_be32_load(const ixn_u8* p) { return ((ixn_u32)p[0] << 24u) | ((ixn_u32)p[1] << 16u) | ((ixn_u32)p[2] << 8u) | (ixn_u32)p[3]; }
static inline void ixn_be16_store(ixn_u8* p, ixn_u16 v) { p[0] = (ixn_u8)(v >> 8u); p[1] = (ixn_u8)(v & 0xFFu); }
static inline void ixn_be32_store(ixn_u8* p, ixn_u32 v) { p[0] = (ixn_u8)(v >> 24u); p[1] = (ixn_u8)(v >> 16u); p[2] = (ixn_u8)(v >> 8u); p[3] = (ixn_u8)(v & 0xFFu); }
static inline ixn_u16 ixn_le16_load(const ixn_u8* p) { return (ixn_u16)((ixn_u16)p[0] | ((ixn_u16)p[1] << 8u)); }
static inline ixn_u32 ixn_le32_load(const ixn_u8* p) { return (ixn_u32)p[0] | ((ixn_u32)p[1] << 8u) | ((ixn_u32)p[2] << 16u) | ((ixn_u32)p[3] << 24u); }
static inline void ixn_le16_store(ixn_u8* p, ixn_u16 v) { p[0] = (ixn_u8)(v & 0xFFu); p[1] = (ixn_u8)(v >> 8u); }
static inline void ixn_le32_store(ixn_u8* p, ixn_u32 v) { p[0] = (ixn_u8)(v & 0xFFu); p[1] = (ixn_u8)(v >> 8u); p[2] = (ixn_u8)(v >> 16u); p[3] = (ixn_u8)(v >> 24u); }

#endif
