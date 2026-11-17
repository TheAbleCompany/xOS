#ifndef IXN_SKB_H
#define IXN_SKB_H

#include "ixn_types.h"
#include "ixn_arch.h"
#include "ixn_netdev.h"

#define IXN_SKB_POOL_SIZE       256u
#define IXN_SKB_DATA_SIZE       2048u
#define IXN_SKB_MAX_HEADROOM    128u
#define IXN_SKB_MAX_FRAGS       8u

#define IXN_SKB_FLAG_RX         (1u << 0)
#define IXN_SKB_FLAG_TX         (1u << 1)
#define IXN_SKB_FLAG_BROADCAST  (1u << 2)
#define IXN_SKB_FLAG_MULTICAST  (1u << 3)
#define IXN_SKB_FLAG_PROMISC    (1u << 4)
#define IXN_SKB_FLAG_LOOPBACK   (1u << 5)
#define IXN_SKB_FLAG_CSUM_OK    (1u << 6)
#define IXN_SKB_FLAG_CSUM_BAD   (1u << 7)
#define IXN_SKB_FLAG_FRAGMENT   (1u << 8)
#define IXN_SKB_FLAG_SHARED     (1u << 9)
#define IXN_SKB_FLAG_CLONED     (1u << 10)

typedef struct {
    ixn_u8 *page;
    ixn_u32 offset;
    ixn_u32 size;
} ixn_skb_frag_t;

struct ixn_skb {
    ixn_netdev_t *dev;

    ixn_u8 *head;
    ixn_u8 *data;
    ixn_u8 *tail;
    ixn_u8 *end;

    ixn_u32 len;
    ixn_u32 data_len;
    ixn_u32 truesize;
    ixn_u32 mac_len;
    ixn_u32 network_header_off;
    ixn_u32 transport_header_off;
    ixn_u32 mac_header_off;

    ixn_u32 flags;
    volatile ixn_u32 refcount;
    ixn_u32 pool_index;
    ixn_u8 in_pool;

    ixn_u32 priority;
    ixn_u32 protocol;
    ixn_u32 csum;
    ixn_u32 hash;
    ixn_u64 timestamp;

    ixn_skb_frag_t frags[IXN_SKB_MAX_FRAGS];
    ixn_u32 nr_frags;

    struct ixn_skb *next;
    struct ixn_skb *prev;

    ixn_u8 storage[IXN_SKB_DATA_SIZE + IXN_SKB_MAX_HEADROOM];
};

void ixn_skb_pool_init(void);
void ixn_skb_pool_shutdown(void);

ixn_skb_t *ixn_skb_alloc(ixn_u32 size);
ixn_skb_t *ixn_skb_alloc_rx(ixn_u32 size);
ixn_skb_t *ixn_skb_clone(ixn_skb_t *skb);
void ixn_skb_free(ixn_skb_t *skb);

void ixn_skb_reserve(ixn_skb_t *skb, ixn_u32 len);
void ixn_skb_put(ixn_skb_t *skb, ixn_u32 len);
void ixn_skb_push(ixn_skb_t *skb, ixn_u32 len);
void ixn_skb_pull(ixn_skb_t *skb, ixn_u32 len);
void ixn_skb_trim(ixn_skb_t *skb, ixn_u32 len);

ixn_u8 *ixn_skb_data(ixn_skb_t *skb);
ixn_u32 ixn_skb_len(ixn_skb_t *skb);
ixn_u8 *ixn_skb_mac_header(ixn_skb_t *skb);
ixn_u8 *ixn_skb_network_header(ixn_skb_t *skb);
ixn_u8 *ixn_skb_transport_header(ixn_skb_t *skb);
void ixn_skb_set_mac_header(ixn_skb_t *skb, ixn_u32 off);
void ixn_skb_set_network_header(ixn_skb_t *skb, ixn_u32 off);
void ixn_skb_set_transport_header(ixn_skb_t *skb, ixn_u32 off);

void ixn_skb_reset_headers(ixn_skb_t *skb);
void ixn_skb_set_dev(ixn_skb_t *skb, ixn_netdev_t *dev);

ixn_skb_t *ixn_skb_queue_pop(ixn_skb_t **head);
void ixn_skb_queue_push(ixn_skb_t **head, ixn_skb_t *skb);
void ixn_skb_queue_purge(ixn_skb_t **head);
ixn_u32 ixn_skb_queue_len(ixn_skb_t *head);

static inline void ixn_skb_get(ixn_skb_t *skb) {
    if (skb) ixn_atomic_inc32(&skb->refcount);
}

#endif
