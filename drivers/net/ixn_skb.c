#include "ixn_skb.h"
#include "ixn_compat.h"

static ixn_skb_t g_ixn_skb_pool[IXN_SKB_POOL_SIZE];
static volatile ixn_u32 g_ixn_skb_pool_lock = 0u;
static ixn_u8 g_ixn_skb_pool_inited = 0u;

void ixn_skb_pool_init(void) {
    ixn_u32 i;
    if (g_ixn_skb_pool_inited) return;
    ixn_memset(g_ixn_skb_pool, 0, sizeof(g_ixn_skb_pool));
    for (i = 0u; i < IXN_SKB_POOL_SIZE; i++) {
        g_ixn_skb_pool[i].in_pool = 1u;
        g_ixn_skb_pool[i].pool_index = i;
        g_ixn_skb_pool[i].head = g_ixn_skb_pool[i].storage;
        g_ixn_skb_pool[i].end = g_ixn_skb_pool[i].storage + sizeof(g_ixn_skb_pool[i].storage);
        g_ixn_skb_pool[i].data = g_ixn_skb_pool[i].head + IXN_SKB_MAX_HEADROOM;
        g_ixn_skb_pool[i].tail = g_ixn_skb_pool[i].data;
    }
    g_ixn_skb_pool_inited = 1u;
}

void ixn_skb_pool_shutdown(void) {
    ixn_memset(g_ixn_skb_pool, 0, sizeof(g_ixn_skb_pool));
    g_ixn_skb_pool_inited = 0u;
}

static ixn_skb_t *ixn_skb_pool_take(void) {
    ixn_u64 flags;
    ixn_u32 i;
    flags = ixn_irq_lock_acquire(&g_ixn_skb_pool_lock);
    for (i = 0u; i < IXN_SKB_POOL_SIZE; i++) {
        ixn_skb_t *s = &g_ixn_skb_pool[i];
        if (s->in_pool && s->refcount == 0u) {
            s->in_pool = 0u;
            s->refcount = 1u;
            ixn_irq_lock_release(&g_ixn_skb_pool_lock, flags);
            return s;
        }
    }
    ixn_irq_lock_release(&g_ixn_skb_pool_lock, flags);
    return IXN_NULL;
}

ixn_skb_t *ixn_skb_alloc(ixn_u32 size) {
    ixn_skb_t *skb;
    if (!g_ixn_skb_pool_inited) ixn_skb_pool_init();
    if (size > IXN_SKB_DATA_SIZE) return IXN_NULL;
    skb = ixn_skb_pool_take();
    if (!skb) return IXN_NULL;
    skb->head = skb->storage;
    skb->end = skb->storage + sizeof(skb->storage);
    skb->data = skb->head + IXN_SKB_MAX_HEADROOM;
    skb->tail = skb->data;
    skb->len = 0u;
    skb->data_len = 0u;
    skb->truesize = sizeof(ixn_skb_t);
    skb->mac_len = 0u;
    skb->mac_header_off = 0u;
    skb->network_header_off = 0u;
    skb->transport_header_off = 0u;
    skb->flags = 0u;
    skb->priority = 0u;
    skb->protocol = 0u;
    skb->csum = 0u;
    skb->hash = 0u;
    skb->timestamp = ixn_ticks_ms();
    skb->nr_frags = 0u;
    skb->next = IXN_NULL;
    skb->prev = IXN_NULL;
    skb->dev = IXN_NULL;
    return skb;
}

ixn_skb_t *ixn_skb_alloc_rx(ixn_u32 size) {
    ixn_skb_t *skb = ixn_skb_alloc(size);
    if (skb) skb->flags |= IXN_SKB_FLAG_RX;
    return skb;
}

ixn_skb_t *ixn_skb_clone(ixn_skb_t *skb) {
    ixn_skb_t *c;
    if (!skb) return IXN_NULL;
    c = ixn_skb_alloc(skb->len);
    if (!c) return IXN_NULL;
    ixn_memcpy(c->head, skb->head, sizeof(skb->storage));
    c->data = c->head + (ixn_u32)(skb->data - skb->head);
    c->tail = c->head + (ixn_u32)(skb->tail - skb->head);
    c->len = skb->len;
    c->data_len = skb->data_len;
    c->mac_len = skb->mac_len;
    c->mac_header_off = skb->mac_header_off;
    c->network_header_off = skb->network_header_off;
    c->transport_header_off = skb->transport_header_off;
    c->protocol = skb->protocol;
    c->dev = skb->dev;
    c->flags = skb->flags | IXN_SKB_FLAG_CLONED;
    return c;
}

void ixn_skb_free(ixn_skb_t *skb) {
    ixn_u64 flags;
    if (!skb) return;
    if (ixn_atomic_add32(&skb->refcount, 0xFFFFFFFFu) != 1u) return;
    flags = ixn_irq_lock_acquire(&g_ixn_skb_pool_lock);
    skb->refcount = 0u;
    skb->in_pool = 1u;
    skb->next = IXN_NULL;
    skb->prev = IXN_NULL;
    skb->dev = IXN_NULL;
    skb->len = 0u;
    skb->data_len = 0u;
    skb->flags = 0u;
    skb->nr_frags = 0u;
    ixn_irq_lock_release(&g_ixn_skb_pool_lock, flags);
}

void ixn_skb_reserve(ixn_skb_t *skb, ixn_u32 len) {
    if (!skb) return;
    skb->data += len;
    skb->tail += len;
}

void ixn_skb_put(ixn_skb_t *skb, ixn_u32 len) {
    if (!skb) return;
    skb->tail += len;
    skb->len += len;
    skb->data_len += len;
}

void ixn_skb_push(ixn_skb_t *skb, ixn_u32 len) {
    if (!skb) return;
    skb->data -= len;
    skb->len += len;
    skb->data_len += len;
}

void ixn_skb_pull(ixn_skb_t *skb, ixn_u32 len) {
    if (!skb || len > skb->len) return;
    skb->data += len;
    skb->len -= len;
    skb->data_len -= len;
}

void ixn_skb_trim(ixn_skb_t *skb, ixn_u32 len) {
    if (!skb) return;
    if (len < skb->len) {
        ixn_u32 diff = skb->len - len;
        skb->tail -= diff;
        skb->len = len;
        skb->data_len = len;
    }
}

ixn_u8 *ixn_skb_data(ixn_skb_t *skb) { return skb ? skb->data : IXN_NULL; }
ixn_u32 ixn_skb_len(ixn_skb_t *skb) { return skb ? skb->len : 0u; }

ixn_u8 *ixn_skb_mac_header(ixn_skb_t *skb) {
    return skb ? (skb->head + skb->mac_header_off) : IXN_NULL;
}
ixn_u8 *ixn_skb_network_header(ixn_skb_t *skb) {
    return skb ? (skb->head + skb->network_header_off) : IXN_NULL;
}
ixn_u8 *ixn_skb_transport_header(ixn_skb_t *skb) {
    return skb ? (skb->head + skb->transport_header_off) : IXN_NULL;
}

void ixn_skb_set_mac_header(ixn_skb_t *skb, ixn_u32 off) {
    if (skb) { skb->mac_header_off = off; skb->mac_len = skb->network_header_off - off; }
}
void ixn_skb_set_network_header(ixn_skb_t *skb, ixn_u32 off) {
    if (skb) skb->network_header_off = off;
}
void ixn_skb_set_transport_header(ixn_skb_t *skb, ixn_u32 off) {
    if (skb) skb->transport_header_off = off;
}

void ixn_skb_reset_headers(ixn_skb_t *skb) {
    if (!skb) return;
    skb->mac_header_off = (ixn_u32)(skb->data - skb->head);
    skb->network_header_off = skb->mac_header_off;
    skb->transport_header_off = skb->mac_header_off;
}

void ixn_skb_set_dev(ixn_skb_t *skb, ixn_netdev_t *dev) {
    if (skb) skb->dev = dev;
}

ixn_skb_t *ixn_skb_queue_pop(ixn_skb_t **head) {
    ixn_skb_t *s;
    if (!head || !*head) return IXN_NULL;
    s = *head;
    *head = s->next;
    if (s->next) s->next->prev = IXN_NULL;
    s->next = IXN_NULL;
    s->prev = IXN_NULL;
    return s;
}

void ixn_skb_queue_push(ixn_skb_t **head, ixn_skb_t *skb) {
    if (!head || !skb) return;
    skb->next = *head;
    skb->prev = IXN_NULL;
    if (*head) (*head)->prev = skb;
    *head = skb;
}

void ixn_skb_queue_purge(ixn_skb_t **head) {
    ixn_skb_t *s;
    if (!head) return;
    while ((s = ixn_skb_queue_pop(head)) != IXN_NULL) ixn_skb_free(s);
}

ixn_u32 ixn_skb_queue_len(ixn_skb_t *head) {
    ixn_u32 n = 0u;
    while (head) { n++; head = head->next; }
    return n;
}
