/*
 * arpa/inet.h -- byte-order helpers for the ArtyomX64 Quake port.
 *
 * net_main.c only needs ntohs() for printing an address.  We keep a
 * minimal in-header implementation so no networking library is needed.
 */
#ifndef AX_COMPAT_ARPA_INET_H
#define AX_COMPAT_ARPA_INET_H

#include <stdint.h>

static inline uint16_t ax_bswap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t ax_bswap32(uint32_t v)
{
    return ((v & 0x000000ffu) << 24) |
           ((v & 0x0000ff00u) << 8)  |
           ((v & 0x00ff0000u) >> 8)  |
           ((v & 0xff000000u) >> 24);
}

/* ArtyomX64/x86-64 is little-endian, network order is big-endian. */
static inline uint16_t htons(uint16_t v) { return ax_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return ax_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return ax_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return ax_bswap32(v); }

#endif /* AX_COMPAT_ARPA_INET_H */
