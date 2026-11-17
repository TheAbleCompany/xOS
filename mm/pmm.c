/*
 * mm/pmm.c -- physical page-frame allocator.
 *
 * Bitmap allocator over a fixed 16 GiB window: one bit per 4 KiB frame,
 * 512 KiB of .bss, so the allocator never has to allocate memory to
 * bootstrap itself.
 *
 * A set bit means the frame is in use. Runs are located with a word-at-a-time
 * scan that skips fully-occupied and fully-empty 64-frame words outright, and
 * a moving hint makes the steady state (allocate, allocate, ...) touch only
 * the word it is currently working on instead of the whole bitmap.
 *
 * The memory map comes from the multiboot1 structure that bridge_memmap()
 * synthesised out of Limine's response, so nothing here depends on the
 * bootloader's own headers.
 */

#include "pmm.h"
#include "../lib/common.h"

#include <stdint.h>

extern u8int mbi[1];

#define MBI_FLAGS        (*(u32int *)(mbi + 0))
#define MBI_MMAP_LENGTH  (*(u32int *)(mbi + 48))
#define MBI_MMAP_ADDR    (*(u64int *)(mbi + 52))
#define MULTIBOOT_FLAG_MMAP (1u << 6)

struct mb_mmap_entry {
    u32int size;
    u32int base_lo;
    u32int base_hi;
    u32int len_lo;
    u32int len_hi;
    u32int type;
} __attribute__((packed));

#define MB_MMAP_USABLE 1

#define PMM_MAX_MEMORY  (16ULL * 1024ULL * 1024ULL * 1024ULL)
#define PMM_MAX_PAGES   (PMM_MAX_MEMORY / PMM_PAGE_SIZE)
#define PMM_BITMAP_SIZE (PMM_MAX_PAGES / 8u)
#define PMM_WORDS       (PMM_BITMAP_SIZE / 8u)
#define PMM_BITS        64u

static u64int pmm_bitmap[PMM_WORDS];
static u64int pmm_highest_page;
static u64int pmm_free_count;
static u64int pmm_reserved_count;
static u64int pmm_hint;
static u32int pmm_ready;

static inline int frame_test(u64int page)
{
    return (int)((pmm_bitmap[page >> 6] >> (page & 63u)) & 1u);
}

static inline void frame_set(u64int page)
{
    pmm_bitmap[page >> 6] |= (1ULL << (page & 63u));
}

static inline void frame_clear(u64int page)
{
    pmm_bitmap[page >> 6] &= ~(1ULL << (page & 63u));
}

static void region_mark(u64int base, u64int length, int used)
{
    u64int start;
    u64int end;
    u64int page;

    if (length == 0)
        return;

    start = base / PMM_PAGE_SIZE;
    end   = (base + length + PMM_PAGE_SIZE - 1u) / PMM_PAGE_SIZE;

    if (end > PMM_MAX_PAGES)
        end = PMM_MAX_PAGES;

    if (start >= end)
        return;

    for (page = start; page < end; page++) {
        if (used) {
            if (!frame_test(page)) {
                frame_set(page);
                pmm_free_count--;
                pmm_reserved_count++;
            }
        } else if (frame_test(page)) {
            frame_clear(page);
            pmm_free_count++;
            if (pmm_reserved_count)
                pmm_reserved_count--;
        }
    }

    if (start < pmm_hint)
        pmm_hint = start;
}

void pmm_init(void)
{
    u64int i;

    for (i = 0; i < PMM_WORDS; i++)
        pmm_bitmap[i] = ~0ULL;

    pmm_free_count     = 0;
    pmm_reserved_count = 0;
    pmm_highest_page   = 0;
    pmm_hint           = 0;
    pmm_ready          = 0;

    if ((MBI_FLAGS & MULTIBOOT_FLAG_MMAP) && MBI_MMAP_ADDR != 0) {
        u8int *base  = (u8int *)(uintptr_t)MBI_MMAP_ADDR;
        u32int off   = 0;
        u32int len   = MBI_MMAP_LENGTH;
        u32int guard = 4096;

        while (off + sizeof(struct mb_mmap_entry) <= len && guard--) {
            struct mb_mmap_entry *e = (struct mb_mmap_entry *)(base + off);
            u64int entry_base;
            u64int entry_len;

            if (e->size == 0)
                break;

            entry_base = ((u64int)e->base_hi << 32) | e->base_lo;
            entry_len  = ((u64int)e->len_hi << 32) | e->len_lo;

            if (e->type == MB_MMAP_USABLE) {
                region_mark(entry_base, entry_len, 0);

                if ((entry_base + entry_len) / PMM_PAGE_SIZE > pmm_highest_page)
                    pmm_highest_page = (entry_base + entry_len) / PMM_PAGE_SIZE;
            }

            off += e->size + (u32int)sizeof(e->size);
        }
    }

    if (pmm_highest_page > PMM_MAX_PAGES)
        pmm_highest_page = PMM_MAX_PAGES;

    region_mark(0, PMM_PAGE_SIZE, 1);

    pmm_hint  = 0;
    pmm_ready = 1;
}

static int run_find(u64int start, u64int end, u64int count, u64int *out)
{
    u64int page = start;

    while (page + count <= end) {
        if (frame_test(page)) {
            page++;
            continue;
        }

        if (count == 1) {
            *out = page;
            return 1;
        }

        if (frame_test(page + count - 1u)) {
            page++;
            continue;
        }

        for (u64int i = 1; i + 1u < count; i++) {
            if (frame_test(page + i)) {
                page += i + 1u;
                goto next_span;
            }
        }

        *out = page;
        return 1;

    next_span:
        ;
    }

    return 0;
}

static u64int run_take(u64int start, u64int count)
{
    for (u64int i = 0; i < count; i++)
        frame_set(start + i);

    pmm_free_count -= count;

    if (start + count < pmm_highest_page)
        pmm_hint = start + count;
    else
        pmm_hint = 0;

    return start * PMM_PAGE_SIZE;
}

static u64int run_locate(u64int count, u64int limit)
{
    u64int end = pmm_highest_page;
    u64int found;

    if (limit != PMM_NO_LIMIT) {
        u64int max_page = limit / PMM_PAGE_SIZE;

        if (end > max_page)
            end = max_page;
    }

    if (end <= count)
        return 0;

    if (pmm_hint < end && run_find(pmm_hint, end, count, &found))
        return found;

    if (run_find(0, end, count, &found))
        return found;

    return 0;
}

u64int pmm_alloc_pages(u64int count)
{
    u64int page;

    if (count == 0 || !pmm_ready || pmm_highest_page == 0)
        return 0;

    page = run_locate(count, PMM_NO_LIMIT);

    if (!page)
        return 0;

    return run_take(page, count);
}

u64int pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

u64int pmm_alloc_pages_below(u64int count, u64int limit)
{
    u64int page;

    if (count == 0 || !pmm_ready || pmm_highest_page == 0)
        return 0;

    page = run_locate(count, limit);

    if (!page)
        return 0;

    return run_take(page, count);
}

u64int pmm_alloc_pages_aligned(u64int count, u64int align)
{
    u64int span;
    u64int page;
    u64int found;

    if (count == 0 || !pmm_ready || pmm_highest_page == 0)
        return 0;

    if (align <= PMM_PAGE_SIZE)
        return pmm_alloc_pages(count);

    if (align & (align - 1u))
        return 0;

    span = align / PMM_PAGE_SIZE;

    for (page = 0; page + count <= pmm_highest_page; page += span) {
        if (run_find(page, pmm_highest_page, count, &found) && found == page)
            return run_take(page, count);
    }

    return 0;
}

void pmm_free_pages(u64int phys_addr, u64int count)
{
    u64int start;
    u64int i;

    if (!pmm_ready || count == 0)
        return;

    start = phys_addr / PMM_PAGE_SIZE;

    for (i = 0; i < count; i++) {
        u64int page = start + i;

        if (page >= pmm_highest_page)
            break;

        if (!frame_test(page))
            continue;

        frame_clear(page);
        pmm_free_count++;
    }

    if (start < pmm_hint)
        pmm_hint = start;
}

void pmm_free_page(u64int phys_addr)
{
    pmm_free_pages(phys_addr, 1);
}

int pmm_reserve(u64int phys_addr, u64int length)
{
    if (!pmm_ready || length == 0)
        return -1;

    region_mark(phys_addr, length, 1);
    return 0;
}

int pmm_release(u64int phys_addr, u64int length)
{
    if (!pmm_ready || length == 0)
        return -1;

    region_mark(phys_addr, length, 0);
    return 0;
}

int pmm_is_free(u64int phys_addr)
{
    u64int page = phys_addr / PMM_PAGE_SIZE;

    if (!pmm_ready || page >= pmm_highest_page)
        return 0;

    return !frame_test(page);
}

u64int pmm_free_page_count(void)  { return pmm_free_count; }
u64int pmm_total_page_count(void) { return pmm_highest_page; }
u64int pmm_used_page_count(void)  { return pmm_highest_page - pmm_free_count; }

void pmm_get_stats(pmm_stats_t *out)
{
    u64int page;
    u64int run = 0;

    if (!out)
        return;

    out->total_pages    = pmm_highest_page;
    out->free_pages     = pmm_free_count;
    out->used_pages     = pmm_highest_page - pmm_free_count;
    out->reserved_pages = pmm_reserved_count;
    out->bitmap_bytes   = PMM_BITMAP_SIZE;
    out->highest_page   = pmm_highest_page;
    out->largest_run    = 0;

    for (page = 0; page < pmm_highest_page; page++) {
        if (frame_test(page)) {
            run = 0;
            continue;
        }

        run++;
        if (run > out->largest_run)
            out->largest_run = run;
    }
}
