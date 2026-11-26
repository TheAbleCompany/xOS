#include "ixn_kernel_port.h"
#include "ixn_arch.h"

#include "../timer.h"
#include "../../mm/kheap.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../../lib/common.h"

extern u64int limine_hhdm_offset;

/*
 * Hook кооперативного планировщика: длинные poll-циклы стека
 * (ixn_arp_ensure, ping) между итерациями отдают квант остальным
 * задачам.  Устанавливается ядром после initTasking() (kernel/main.c);
 * до этого NULL -- стек крутится сам, как на раннем буте.
 */
void (*ixn_yield_hook)(void) = 0;

void ixn_port_init(void) {
}

ixn_u64 ixn_ticks_ms(void) {
    return (ixn_u64)timer_ticks_ms();
}

void ixn_log(const char *s) {
    if (!s) return;
    serial_puts_ax(s);
    serial_puts_ax("\n");
}

void ixn_log_hex32(ixn_u32 v) {
    char buf[12];
    static const char hx[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = hx[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\n';
    buf[11] = 0;
    serial_puts_ax(buf);
}

void ixn_logf(const char *fmt, ...) {
    (void)fmt;
}

ixn_u8 volatile *ixn_phys_to_virt(ixn_u64 phys) {
    return (ixn_u8 volatile *)(uintptr_t)(phys + limine_hhdm_offset);
}

ixn_u64 ixn_virt_to_phys(const void *virt) {
    return (ixn_u64)(uintptr_t)virt - limine_hhdm_offset;
}

void *ixn_alloc(ixn_size size) {
    return kmalloc((size_t)size);
}

void *ixn_alloc_zero(ixn_size size) {
    void *p = kmalloc((size_t)size);
    if (p) memset((u8int *)p, 0, (u32int)size);
    return p;
}

void ixn_free(void *p) {
    kfree(p);
}

void *ixn_dma_alloc(ixn_size size, ixn_u64 *out_phys) {
    u64int pages = ((u64int)size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    u64int phys;
    void *virt;

    if (out_phys) *out_phys = 0;
    if (pages == 0) pages = 1;

    phys = pmm_alloc_pages(pages);
    if (!phys) return 0;

    virt = (void *)(uintptr_t)(phys + limine_hhdm_offset);
    memset((u8int *)virt, 0, (u32int)(pages * PMM_PAGE_SIZE));
    if (out_phys) *out_phys = phys;
    return virt;
}

void ixn_dma_free(void *ptr, ixn_size size) {
    u64int pages;
    u64int phys;

    if (!ptr) return;
    pages = ((u64int)size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    if (pages == 0) pages = 1;
    phys = (u64int)(uintptr_t)ptr - limine_hhdm_offset;
    pmm_free_pages(phys, pages);
}
