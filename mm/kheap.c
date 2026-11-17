/*
 * mm/kheap.c -- kernel heap.
 *
 * Segregated free lists on top of boundary-tagged blocks.
 *
 * Arenas are physically contiguous page runs taken from the PMM and reached
 * through Limine's HHDM, so nothing here has to touch page tables: physical
 * address X is always readable at (limine_hhdm_offset + X).
 *
 * Block layout, identical for free and allocated blocks:
 *
 *   +--------------+------------------+------------+
 *   | header 24 B  | payload          | footer 8 B |
 *   +--------------+------------------+------------+
 *
 * header.span is the whole block span in bytes with bit 0 set while the block
 * is live; header.requested remembers what the caller asked for. The footer
 * repeats the span, which is what lets kfree coalesce backwards without a
 * back-pointer, in O(1) apart from the merge itself.
 *
 * A free block keeps its list node inside its own header, so bookkeeping for
 * live blocks costs nothing extra. Blocks are bucketed by span: requests up
 * to KHEAP_NUM_CLASSES * KHEAP_CLASS_GRAIN are served out of the first
 * bucket at or above their size, anything larger out of a single big list.
 * Splitting hands the tail straight to its own bucket and coalescing
 * re-indexes the merged block, so a steady alloc/free cycle of one size stays
 * O(1) instead of degrading into a walk over every block ever created.
 */

#include "kheap.h"
#include "pmm.h"
#include "../lib/common.h"

#include <stddef.h>
#include <stdint.h>

extern u64int limine_hhdm_offset;

#define KHEAP_ALIGN        16u
#define KHEAP_MIN_BLOCK    64u
#define KHEAP_ARENA_PAGES  16u
#define KHEAP_ARENA_MAX    1024u
#define KHEAP_CLASS_GRAIN  64u
#define KHEAP_NUM_CLASSES  24u
#define KHEAP_ALLOC_BIT    1u
#define KHEAP_ARENA_HDR    32u

typedef struct kheap_block {
    size_t span;
    size_t requested;
    void  *arena;
    struct kheap_block *next;
    struct kheap_block *prev;
    size_t pad;
} kheap_header_t;


typedef struct {
    size_t span;
} kheap_footer_t;

typedef struct kheap_arena {
    struct kheap_arena *next;
    size_t span;
    size_t block_count;
    size_t pad;
} kheap_arena_t;

_Static_assert(sizeof(kheap_header_t) == 48, "kheap block header must be 48 bytes");
_Static_assert(sizeof(kheap_arena_t) == KHEAP_ARENA_HDR, "arena header size mismatch");

static kheap_arena_t *arena_list;
static kheap_header_t *class_free[KHEAP_NUM_CLASSES];
static kheap_header_t  *big_free;
static size_t         arena_count;
static size_t         total_bytes;
static size_t         used_bytes;
static size_t         free_bytes;
static size_t         block_count;
static size_t         free_block_count;
static int            kheap_ready;

static inline size_t align_up(size_t v)
{
    return (v + (KHEAP_ALIGN - 1u)) & ~(size_t)(KHEAP_ALIGN - 1u);
}

static inline size_t span_for(size_t payload)
{
    size_t span = sizeof(kheap_header_t) + align_up(payload) + sizeof(kheap_footer_t);
    if (span < KHEAP_MIN_BLOCK)
        span = KHEAP_MIN_BLOCK;
    return align_up(span);
}

static inline u8int *block_bytes(void *block)
{
    return (u8int *)block;
}

static inline void *block_payload(void *block)
{
    return (void *)(block_bytes(block) + sizeof(kheap_header_t));
}

static inline kheap_header_t *payload_header(const void *payload)
{
    return (kheap_header_t *)((u8int *)payload - sizeof(kheap_header_t));
}

static inline kheap_arena_t *block_arena(const kheap_header_t *h)
{
    return (kheap_arena_t *)h->arena;
}

static inline u8int *arena_data_lo(const kheap_arena_t *a)
{
    return (u8int *)a + KHEAP_ARENA_HDR;
}

static inline u8int *arena_end(const kheap_arena_t *a)
{
    return (u8int *)a + a->span;
}

static void footer_write(kheap_header_t *h, size_t span)
{
    kheap_footer_t *f = (kheap_footer_t *)(block_bytes(h) + span - sizeof(kheap_footer_t));
    f->span = span;
}

static void header_write(kheap_header_t *h, size_t span, size_t requested,
                         int allocated, void *arena)
{
    h->span      = span | (allocated ? KHEAP_ALLOC_BIT : 0u);
    h->requested = requested;
    h->arena     = arena;
    footer_write(h, span);
}

static size_t class_index(size_t span)
{
    if (span < KHEAP_MIN_BLOCK)
        span = KHEAP_MIN_BLOCK;

    size_t idx = span / KHEAP_CLASS_GRAIN;
    if (idx >= KHEAP_NUM_CLASSES)
        return KHEAP_NUM_CLASSES;
    return idx;
}

static kheap_header_t **class_head(size_t span)
{
    size_t idx = class_index(span);
    if (idx >= KHEAP_NUM_CLASSES)
        return &big_free;
    return &class_free[idx];
}

static void freelist_push(kheap_header_t *node)
{
    kheap_header_t **head = class_head(node->span);

    node->prev = 0;
    node->next = *head;

    if (*head)
        (*head)->prev = node;

    *head = node;
}

static void freelist_remove(kheap_header_t *node)
{
    kheap_header_t **head = class_head(node->span);

    if (node->prev)
        node->prev->next = node->next;
    else
        *head = node->next;

    if (node->next)
        node->next->prev = node->prev;

    node->next = 0;
    node->prev = 0;
}

static void block_become_free(kheap_header_t *h, size_t span, void *arena)
{
    header_write(h, span, 0, 0, arena);

    h->next = 0;
    h->prev = 0;

    freelist_push(h);

    free_bytes += span;
    free_block_count++;
}

static void block_become_used(kheap_header_t *h, size_t span, size_t requested, void *arena)
{
    header_write(h, span, requested, 1, arena);
    used_bytes += span;
}

static kheap_header_t *find_block(size_t span)
{
    size_t idx = class_index(span);

    for (size_t i = idx; i < KHEAP_NUM_CLASSES; i++)
        for (kheap_header_t *n = class_free[i]; n; n = n->next)
            if (n->span >= span)
                return (kheap_header_t *)n;

    for (kheap_header_t *n = big_free; n; n = n->next)
        if (n->span >= span)
            return (kheap_header_t *)n;

    return 0;
}

static kheap_header_t *take_block(size_t span)
{
    kheap_header_t *h = find_block(span);

    if (!h)
        return 0;

    freelist_remove((kheap_header_t *)h);

    free_bytes     -= h->span;
    free_block_count--;

    return h;
}

static void *carve(kheap_header_t *h, size_t span, size_t requested)
{
    size_t have  = h->span;
    void  *arena = h->arena;

    if (have >= span + KHEAP_MIN_BLOCK) {
        size_t rest = have - span;
        kheap_header_t *tail = (kheap_header_t *)(block_bytes(h) + span);

        block_become_used(h, span, requested, arena);
        block_become_free(tail, rest, arena);

        block_count++;
        block_arena(h)->block_count++;

        return block_payload(h);
    }

    block_become_used(h, have, requested, arena);
    return block_payload(h);
}

static kheap_arena_t *arena_create(size_t payload)
{
    size_t need  = KHEAP_ARENA_HDR + span_for(payload);
    size_t pages = (need + (size_t)PMM_PAGE_SIZE - 1u) / (size_t)PMM_PAGE_SIZE;

    if (pages < KHEAP_ARENA_PAGES)
        pages = KHEAP_ARENA_PAGES;

    if (arena_count >= KHEAP_ARENA_MAX)
        return 0;

    u64int phys = pmm_alloc_pages((u64int)pages);
    if (!phys)
        return 0;

    u8int *base  = (u8int *)(uintptr_t)(phys + limine_hhdm_offset);
    size_t bytes = (size_t)pages * (size_t)PMM_PAGE_SIZE;

    kheap_arena_t *a = (kheap_arena_t *)base;
    a->next        = arena_list;
    a->span        = bytes;
    a->block_count = 1;
    a->pad         = 0;

    arena_list = a;
    arena_count++;
    total_bytes += bytes;

    kheap_header_t *h = (kheap_header_t *)arena_data_lo(a);
    block_become_free(h, bytes - KHEAP_ARENA_HDR, a);

    return a;
}

void kheap_init(void)
{
    arena_list       = 0;
    big_free         = 0;
    arena_count      = 0;
    total_bytes      = 0;
    used_bytes       = 0;
    free_bytes       = 0;
    block_count      = 0;
    free_block_count = 0;

    for (size_t i = 0; i < KHEAP_NUM_CLASSES; i++)
        class_free[i] = 0;

    kheap_ready = 1;
}

void *kmalloc(size_t size)
{
    if (!kheap_ready)
        kheap_init();

    if (size == 0)
        size = 1;

    if (size > (size_t)0x40000000u)
        return 0;

    size_t span = span_for(size);

    for (;;) {
        kheap_header_t *h = take_block(span);

        if (!h) {
            if (!arena_create(span))
                return 0;
            continue;
        }

        return carve(h, span, size);
    }
}

void *kmalloc_aligned(size_t size, size_t align)
{
    if (align <= KHEAP_ALIGN)
        return kmalloc(size);

    if (align & (align - 1u))
        return 0;

    if (size == 0)
        size = 1;

    size_t span = span_for(size);
    size_t need = span + align + sizeof(kheap_header_t) + KHEAP_MIN_BLOCK;

    for (;;) {
        kheap_header_t *h = take_block(need);

        if (!h) {
            if (!arena_create(need))
                return 0;
            continue;
        }

        void   *arena   = h->arena;
        size_t have     = h->span;
        u8int  *payload = (u8int *)block_payload(h);
        uintptr_t want  = ((uintptr_t)payload + (uintptr_t)align - 1u) &
                          ~(uintptr_t)(align - 1u);

        size_t front = want - (uintptr_t)payload;

        if (front != 0 && front < sizeof(kheap_header_t) + KHEAP_MIN_BLOCK) {
            want += (uintptr_t)align;
            front = want - (uintptr_t)payload;
        }

        if (front == 0) {
            if (have >= span + KHEAP_MIN_BLOCK) {
                size_t rest = have - span;
                kheap_header_t *tail = (kheap_header_t *)(block_bytes(h) + span);

                block_become_used(h, span, size, arena);
                block_become_free(tail, rest, arena);

                block_count++;
                block_arena(h)->block_count++;

                return block_payload(h);
            }

            block_become_used(h, have, size, arena);
            return block_payload(h);
        }

        size_t fspan = front;

        if (fspan >= have) {
            block_become_free(h, have, arena);
            return 0;
        }

        kheap_header_t *aligned = (kheap_header_t *)(block_bytes(h) + fspan);
        size_t aspan = have - fspan;

        if (aspan < span) {
            block_become_free(h, have, arena);
            return 0;
        }

        size_t use = span;

        if (aspan - use < KHEAP_MIN_BLOCK)
            use = aspan;

        block_become_free(h, fspan, arena);
        block_become_used(aligned, use, size, arena);

        block_count++;
        block_arena(h)->block_count++;
        arena = h->arena;

        if (aspan > use) {
            kheap_header_t *tail = (kheap_header_t *)(block_bytes(aligned) + use);
            block_become_free(tail, aspan - use, arena);
            block_count++;
            block_arena(h)->block_count++;
        }

        return block_payload(aligned);
    }
}

static void merge_with_neighbours(kheap_header_t *h)
{
    kheap_arena_t *a = block_arena(h);
    u8int *lo  = arena_data_lo(a);
    u8int *end = arena_end(a);

    for (;;) {
        u8int *raw = block_bytes(h) + h->span;

        if (raw + sizeof(kheap_header_t) + sizeof(kheap_footer_t) > end)
            break;

        kheap_header_t *n = (kheap_header_t *)raw;

        if (n->span & KHEAP_ALLOC_BIT)
            break;

        size_t nspan = n->span & ~(size_t)KHEAP_ALLOC_BIT;

        freelist_remove((kheap_header_t *)n);
        freelist_remove((kheap_header_t *)h);

        h->span += nspan;
        footer_write(h, h->span);

        ((kheap_header_t *)h)->span = h->span;
        freelist_push((kheap_header_t *)h);

        free_block_count--;
        block_count--;
        a->block_count--;
    }

    for (;;) {
        if (block_bytes(h) <= lo + sizeof(kheap_footer_t))
            break;

        kheap_footer_t *f = (kheap_footer_t *)(block_bytes(h) - sizeof(kheap_footer_t));
        size_t pspan = f->span;

        if (pspan < KHEAP_MIN_BLOCK || pspan > (size_t)(block_bytes(h) - lo))
            break;

        kheap_header_t *p = (kheap_header_t *)(block_bytes(h) - pspan);

        if (p->span & KHEAP_ALLOC_BIT)
            break;

        freelist_remove((kheap_header_t *)p);
        freelist_remove((kheap_header_t *)h);

        p->span += h->span;
        footer_write(p, p->span);

        ((kheap_header_t *)p)->span = p->span;
        freelist_push((kheap_header_t *)p);

        free_block_count--;
        block_count--;
        a->block_count--;

        h = p;
    }
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    kheap_header_t *h = payload_header(ptr);

    if (!(h->span & KHEAP_ALLOC_BIT))
        return;

    size_t span = h->span & ~(size_t)KHEAP_ALLOC_BIT;

    block_become_free(h, span, h->arena);
    merge_with_neighbours(h);
}

size_t kheap_usable_size(const void *ptr)
{
    if (!ptr)
        return 0;

    const kheap_header_t *h = payload_header(ptr);
    size_t span = h->span & ~(size_t)KHEAP_ALLOC_BIT;

    if (span < sizeof(kheap_header_t) + sizeof(kheap_footer_t))
        return 0;

    return span - sizeof(kheap_header_t) - sizeof(kheap_footer_t);
}

void *kcalloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0)
        return kmalloc(1);

    if (nmemb > (size_t)0x40000000u / size)
        return 0;

    size_t total = nmemb * size;
    void *p = kmalloc(total);

    if (p)
        memset(p, 0, total);

    return p;
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);

    if (size == 0) {
        kfree(ptr);
        return 0;
    }

    size_t have = kheap_usable_size(ptr);

    if (have >= size)
        return ptr;

    void *np = kmalloc(size);

    if (!np)
        return 0;

    memcpy(np, ptr, have);
    kfree(ptr);

    return np;
}

void kheap_stats(kheap_stats_t *out)
{
    if (!out)
        return;

    out->total_bytes      = total_bytes;
    out->used_bytes       = used_bytes + arena_count * KHEAP_ARENA_HDR;
    out->free_bytes       = free_bytes;
    out->largest_free     = 0;
    out->block_count      = block_count;
    out->free_block_count = free_block_count;
    out->arena_count      = arena_count;

    for (kheap_header_t *n = big_free; n; n = n->next)
        if (n->span > out->largest_free)
            out->largest_free = n->span;

    for (size_t i = 0; i < KHEAP_NUM_CLASSES; i++)
        for (kheap_header_t *n = class_free[i]; n; n = n->next)
            if (n->span > out->largest_free)
                out->largest_free = n->span;
}
