#ifndef PMM_H
#define PMM_H

#include "../lib/common.h"
#include <stddef.h>

#define PMM_PAGE_SIZE   4096ULL
#define PMM_PAGE_SHIFT  12u

#define PMM_NO_LIMIT    (~0ULL)

#define PMM_FLAG_LOW    0x1u
#define PMM_FLAG_ALIGN  0x2u

typedef struct {
    u64int total_pages;
    u64int free_pages;
    u64int used_pages;
    u64int reserved_pages;
    u64int largest_run;
    u64int bitmap_bytes;
    u64int highest_page;
} pmm_stats_t;

void   pmm_init(void);

u64int pmm_alloc_page(void);
u64int pmm_alloc_pages(u64int count);
u64int pmm_alloc_pages_below(u64int count, u64int limit);
u64int pmm_alloc_pages_aligned(u64int count, u64int align);

void   pmm_free_page(u64int phys_addr);
void   pmm_free_pages(u64int phys_addr, u64int count);

int    pmm_reserve(u64int phys_addr, u64int length);
int    pmm_release(u64int phys_addr, u64int length);
int    pmm_is_free(u64int phys_addr);

u64int pmm_free_page_count(void);
u64int pmm_used_page_count(void);
u64int pmm_total_page_count(void);
void   pmm_get_stats(pmm_stats_t *out);

#endif
