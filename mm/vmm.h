#ifndef VMM_H
#define VMM_H

/*
 * mm/vmm.h -- virtual memory manager (x86-64 4-level paging).
 *
 * A vmm_space_t is the physical address of a PML4, i.e. exactly what CR3
 * holds. The kernel address space is the PML4 the bootloader handed over;
 * every user task gets a fresh one whose upper half (PML4 entries 256..511)
 * is a verbatim copy of the kernel's, so kernel code, the HHDM and any
 * kernel-side device mappings stay reachable from every address space.
 */

#include "../lib/common.h"

typedef u64int vmm_space_t;

#define PAGE_PRESENT   0x001ULL
#define PAGE_WRITE     0x002ULL
#define PAGE_USER      0x004ULL
#define PAGE_PWT       0x008ULL
#define PAGE_PCD       0x010ULL
#define PAGE_ACCESSED  0x020ULL
#define PAGE_DIRTY     0x040ULL
#define PAGE_HUGE      0x080ULL
#define PAGE_GLOBAL    0x100ULL
#define PAGE_NX        (1ULL << 63)

#define PAGE_UNCACHED  (PAGE_PWT | PAGE_PCD)

#define VMM_MMIO_BASE      0xFFFF9E0000000000ULL
#define VMM_MMIO_END       0xFFFFB00000000000ULL

void        vmm_init(void);
vmm_space_t vmm_kernel_space(void);
vmm_space_t vmm_current_space(void);

vmm_space_t vmm_new_address_space(void);
void        vmm_release_space(vmm_space_t space, int free_user_frames);
void        vmm_reap(void);

int     vmm_map_page(vmm_space_t space, u64int vaddr, u64int paddr, u64int flags);
int     vmm_map_range(vmm_space_t space, u64int vaddr, u64int paddr,
                      u64int count, u64int flags);
int     vmm_map_alloc(vmm_space_t space, u64int vaddr, u64int count, u64int flags);
int     vmm_unmap_page(vmm_space_t space, u64int vaddr);
int     vmm_unmap_range(vmm_space_t space, u64int vaddr, u64int count);

u64int  vmm_translate(vmm_space_t space, u64int vaddr);
int     vmm_is_mapped(vmm_space_t space, u64int vaddr);

void    vmm_switch(vmm_space_t space);
void    vmm_invalidate(u64int vaddr);

void   *vmm_phys_to_virt(u64int phys);
u64int  vmm_virt_to_phys(const void *virt);

void   *vmm_map_mmio(u64int phys, u64int size);
void    vmm_unmap_mmio(void *virt, u64int size);

#endif
