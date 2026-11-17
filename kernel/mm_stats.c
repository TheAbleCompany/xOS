/*
 * kernel/mm_stats.c -- memory subsystem report on COM1.
 *
 * Printed once during boot and again from the shell after an app has been
 * started and killed, so a leak in task teardown shows up as a growing
 * used_pages figure instead of staying invisible until the PMM runs dry.
 */

#include "../lib/common.h"
#include "../mm/pmm.h"
#include "../mm/kheap.h"
#include "../mm/vmm.h"

static void put_size(u64int bytes)
{
    static const char suffix[4] = { 'K', 'M', 'G', 0 };
    u64int value = bytes;
    int unit = 0;

    while ((value >> 10) && unit < 3) {
        value >>= 10;
        unit++;
    }

    debug_putnum(value);

    if (unit > 0) {
        debug_putc(suffix[unit - 1]);
        debug_putc('B');
    } else {
        debug_putc('B');
    }
}

void ax_mm_report(const char *tag)
{
    pmm_stats_t  pmm;
    kheap_stats_t heap;

    pmm_get_stats(&pmm);
    kheap_stats(&heap);

    debug_puts("[MM] ");
    debug_puts(tag);
    debug_puts(" phys total=");
    debug_putnum(pmm.total_pages);
    debug_puts(" free=");
    debug_putnum(pmm.free_pages);
    debug_puts(" used=");
    debug_putnum(pmm.used_pages);
    debug_puts(" largest=");
    debug_putnum(pmm.largest_run);
    debug_puts(" | heap arena=");
    debug_putnum(heap.arena_count);
    debug_puts(" blocks=");
    debug_putnum(heap.block_count);
    debug_puts(" freeblk=");
    debug_putnum(heap.free_block_count);
    debug_puts(" used=");
    put_size(heap.used_bytes);
    debug_puts(" free=");
    put_size(heap.free_bytes);
    debug_puts(" frag=");
    debug_putnum(heap.largest_free);
    debug_puts("\n");
}
