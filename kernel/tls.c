#include <stdint.h>
#include <stddef.h>

#include "../lib/common.h"
#include "../mm/kheap.h"

#define AX_MSR_FS_BASE 0xC0000100u
#define AX_TCB_SIZE    16u

extern char _tls_start[];
extern char _tls_end[];

void ax_tls_write_fs_base(uint64_t base)
{
    uint32_t lo = (uint32_t)(base & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(base >> 32);

    __asm__ volatile("wrmsr"
                     :
                     : "c"(AX_MSR_FS_BASE), "a"(lo), "d"(hi)
                     : "memory");
}

uint64_t ax_tls_read_fs_base(void)
{
    uint32_t lo, hi;

    __asm__ volatile("rdmsr"
                     : "=a"(lo), "=d"(hi)
                     : "c"(AX_MSR_FS_BASE));

    return ((uint64_t)hi << 32) | lo;
}

size_t ax_tls_image_size(void)
{
    return (size_t)(_tls_end - _tls_start);
}

void *ax_tls_block_alloc(void)
{
    size_t size = ax_tls_image_size();
    size_t span = (size + 15u) & ~(size_t)15u;
    char *block;
    uint64_t fs_base;

    if (size == 0)
        return 0;

    block = (char *)kmalloc_aligned(span + AX_TCB_SIZE, 16);

    if (!block)
        return 0;

    memset(block, 0, span + AX_TCB_SIZE);
    memcpy(block + (span - size), _tls_start, size);

    fs_base = (uint64_t)(uintptr_t)(block + span);

    *(uint64_t *)(uintptr_t)fs_base = fs_base;

    return block;
}

void ax_tls_block_free(void *block)
{
    if (block)
        kfree(block);
}

uint64_t ax_tls_block_fs_base(void *block)
{
    size_t size = ax_tls_image_size();

    if (!block || size == 0)
        return 0;

    return (uint64_t)(uintptr_t)block + ((size + 15u) & ~(size_t)15u);
}
