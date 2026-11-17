/*
 * mm/vmm.c -- x86-64 4-level paging.
 *
 * Page tables are ordinary physical pages reached through the HHDM, so the
 * walker below is just pointer arithmetic on (phys + limine_hhdm_offset).
 *
 * Two things make mapping cheap:
 *
 *   - Intermediate tables are created with PRESENT|WRITE|USER, while the
 *     permission the caller asked for lands on the leaf. A mapping therefore
 *     never has to be recreated when a different task maps the same virtual
 *     region with different rights; the leaf decides.
 *
 *   - Kernel mappings (anything with a virtual address in the upper half) are
 *     copied into every address space created afterwards, so driver mappings
 *     made before a task exists stay valid inside it. Lower-half mappings are
 *     per task and never propagate.
 *
 * Address spaces are counted so a task can hand its PML4 back on exit and the
 * frames it owned actually return to the PMM instead of being leaked, which
 * is what the old implementation did by design.
 */

#include "vmm.h"
#include "pmm.h"
#include "../lib/common.h"

#include <stddef.h>

extern u64int limine_hhdm_offset;

#define ENTRIES 512u
#define PT_INDEX(va, level) (((va) >> (12u + 9u * (unsigned)(level))) & 0x1FFu)

#define space_table(phys) ((u64int *)(uintptr_t)((phys) + limine_hhdm_offset))

static vmm_space_t kernel_space;
static u64int       mmio_next;
static u64int       mmio_floor;
static u64int       space_count;

static int table_alloc(u64int *out)
{
    u64int phys = pmm_alloc_page();

    if (!phys)
        return -1;

    u64int *t = space_table(phys);

    for (unsigned i = 0; i < ENTRIES; i++)
        t[i] = 0;

    *out = phys;
    return 0;
}

static void table_free(u64int phys)
{
    if (phys)
        pmm_free_page(phys);
}

void vmm_init(void)
{
    u64int cr3;

    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    kernel_space = cr3 & ~0xFFFULL;
    mmio_next    = VMM_MMIO_BASE;
    mmio_floor   = VMM_MMIO_BASE;
    space_count  = 1;
}

vmm_space_t vmm_kernel_space(void)
{
    return kernel_space;
}

vmm_space_t vmm_current_space(void)
{
    u64int cr3;

    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    return cr3 & ~0xFFFULL;
}

vmm_space_t vmm_new_address_space(void)
{
    u64int pml4_phys;

    if (table_alloc(&pml4_phys) != 0)
        return 0;

    u64int *dst = space_table(pml4_phys);
    u64int *src = space_table(kernel_space);

    for (unsigned i = 256; i < ENTRIES; i++)
        dst[i] = src[i];

    space_count++;

    return pml4_phys;
}

static int walk_create(u64int space, u64int vaddr, u64int **leaf)
{
    u64int *table = space_table(space);

    for (int level = 3; level >= 1; level--) {
        unsigned idx = (unsigned)PT_INDEX(vaddr, level);
        u64int entry = table[idx];

        if (entry & PAGE_PRESENT) {
            if (entry & PAGE_HUGE)
                return -1;

            table = space_table(entry & ~0xFFFULL);
            continue;
        }

        u64int fresh;

        if (table_alloc(&fresh) != 0)
            return -1;

        table[idx] = fresh | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        table = space_table(fresh);
    }

    *leaf = table;
    return 0;
}

static u64int *walk_existing(u64int space, u64int vaddr)
{
    u64int *table = space_table(space);

    for (int level = 3; level >= 1; level--) {
        unsigned idx = (unsigned)PT_INDEX(vaddr, level);
        u64int entry = table[idx];

        if (!(entry & PAGE_PRESENT))
            return 0;

        if (entry & PAGE_HUGE)
            return 0;

        table = space_table(entry & ~0xFFFULL);
    }

    return table;
}

int vmm_map_page(vmm_space_t space, u64int vaddr, u64int paddr, u64int flags)
{
    u64int *leaf = 0;
    unsigned idx;

    if (!space)
        return -1;

    if (vaddr & 0xFFFULL)
        return -1;

    if (walk_create(space, vaddr, &leaf) != 0)
        return -1;

    idx = (unsigned)PT_INDEX(vaddr, 0);
    leaf[idx] = (paddr & ~0xFFFULL) | (flags & ~PAGE_PRESENT) | PAGE_PRESENT;

    vmm_invalidate(vaddr);

    return 0;
}

int vmm_map_range(vmm_space_t space, u64int vaddr, u64int paddr,
                  u64int count, u64int flags)
{
    for (u64int i = 0; i < count; i++) {
        if (vmm_map_page(space, vaddr + i * PMM_PAGE_SIZE,
                         paddr + i * PMM_PAGE_SIZE, flags) != 0)
            return -1;
    }

    return 0;
}

int vmm_map_alloc(vmm_space_t space, u64int vaddr, u64int count, u64int flags)
{
    for (u64int i = 0; i < count; i++) {
        u64int phys = pmm_alloc_page();

        if (!phys)
            return -1;

        u64int *frame = space_table(phys);

        for (unsigned j = 0; j < ENTRIES; j++)
            frame[j] = 0;

        if (vmm_map_page(space, vaddr + i * PMM_PAGE_SIZE, phys, flags) != 0) {
            pmm_free_page(phys);
            return -1;
        }
    }

    return 0;
}

int vmm_unmap_page(vmm_space_t space, u64int vaddr)
{
    u64int *leaf = walk_existing(space, vaddr);

    if (!leaf)
        return -1;

    unsigned idx = (unsigned)PT_INDEX(vaddr, 0);

    if (!(leaf[idx] & PAGE_PRESENT))
        return -1;

    leaf[idx] = 0;
    vmm_invalidate(vaddr);

    return 0;
}

int vmm_unmap_range(vmm_space_t space, u64int vaddr, u64int count)
{
    int rc = 0;

    for (u64int i = 0; i < count; i++)
        if (vmm_unmap_page(space, vaddr + i * PMM_PAGE_SIZE) != 0)
            rc = -1;

    return rc;
}

u64int vmm_translate(vmm_space_t space, u64int vaddr)
{
    u64int *leaf = walk_existing(space, vaddr);

    if (!leaf)
        return 0;

    u64int entry = leaf[PT_INDEX(vaddr, 0)];

    if (!(entry & PAGE_PRESENT))
        return 0;

    return (entry & ~0xFFFULL) | (vaddr & 0xFFFULL);
}

int vmm_is_mapped(vmm_space_t space, u64int vaddr)
{
    return vmm_translate(space, vaddr) != 0;
}

void vmm_release_space(vmm_space_t space, int free_user_frames)
{
    if (!space || space == kernel_space)
        return;

    u64int *pml4 = space_table(space);

    for (unsigned i = 0; i < 256; i++) {
        u64int entry = pml4[i];
        u64int *pdpt;
        u64int saved[ENTRIES];

        if (!(entry & PAGE_PRESENT))
            continue;

        pdpt = space_table(entry & ~0xFFFULL);

        for (unsigned j = 0; j < ENTRIES; j++)
            saved[j] = pdpt[j];

        for (unsigned j = 0; j < ENTRIES; j++)
            pdpt[j] = 0;

        if (!free_user_frames) {
            table_free(entry & ~0xFFFULL);
            pml4[i] = 0;
            continue;
        }

        for (unsigned j = 0; j < ENTRIES; j++) {
            if (!(saved[j] & PAGE_PRESENT))
                continue;

            u64int pd_phys = saved[j] & ~0xFFFULL;
            u64int *pd     = space_table(pd_phys);

            for (unsigned k = 0; k < ENTRIES; k++) {
                if (!(pd[k] & PAGE_PRESENT))
                    continue;

                u64int pt_phys = pd[k] & ~0xFFFULL;
                u64int *pt     = space_table(pt_phys);

                for (unsigned l = 0; l < ENTRIES; l++) {
                    if (!(pt[l] & PAGE_PRESENT))
                        continue;

                    pmm_free_page(pt[l] & ~0xFFFULL);
                    pt[l] = 0;
                }

                table_free(pt_phys);
                pd[k] = 0;
            }

            table_free(pd_phys);
        }

        table_free(entry & ~0xFFFULL);
        pml4[i] = 0;
    }

    for (unsigned i = 256; i < ENTRIES; i++)
        pml4[i] = 0;

    table_free(space);

    if (space_count)
        space_count--;
}

void vmm_reap(void)
{
    if (!space_count)
        space_count = 1;
}

void vmm_switch(vmm_space_t space)
{
    __asm__ volatile("mov %0, %%cr3" :: "r"(space) : "memory");
}

void vmm_invalidate(u64int vaddr)
{
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr & ~0xFFFULL) : "memory");
}

void *vmm_phys_to_virt(u64int phys)
{
    return (void *)(uintptr_t)(phys + limine_hhdm_offset);
}

u64int vmm_virt_to_phys(const void *virt)
{
    u64int va = (u64int)(uintptr_t)virt;

    if (va >= limine_hhdm_offset && va < limine_hhdm_offset + 0x400000000ULL)
        return va - limine_hhdm_offset;

    return vmm_translate(vmm_current_space(), va & ~0xFFFULL);
}

void *vmm_map_mmio(u64int phys, u64int size)
{
    u64int space = vmm_kernel_space();
    u64int off   = phys & 0xFFFULL;
    u64int pages = (off + size + 0xFFFULL) >> 12;
    u64int va;

    if (mmio_next == 0)
        mmio_next = mmio_floor;

    if (mmio_next + pages * PMM_PAGE_SIZE > VMM_MMIO_END)
        return 0;

    va = mmio_next;

    for (u64int i = 0; i < pages; i++) {
        if (vmm_map_page(space, va + i * PMM_PAGE_SIZE,
                         (phys & ~0xFFFULL) + i * PMM_PAGE_SIZE,
                         PAGE_WRITE | PAGE_UNCACHED) != 0)
            return 0;
    }

    mmio_next += pages * PMM_PAGE_SIZE;

    return (void *)(uintptr_t)(va + off);
}

void vmm_unmap_mmio(void *virt, u64int size)
{
    u64int va = (u64int)(uintptr_t)virt & ~0xFFFULL;
    u64int pages = (size + 0xFFFULL) >> 12;

    if (va < VMM_MMIO_BASE || va >= VMM_MMIO_END)
        return;

    vmm_unmap_range(vmm_kernel_space(), va, pages);
}
