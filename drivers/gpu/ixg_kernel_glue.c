/*
 * ixg_kernel_glue.c -- hosted-C shims for the ixg GPU driver.
 *
 * The driver is written against hosted C (malloc/free) but links into
 * the freestanding kernel.  Allocator entry points live in lib/libc.c
 * (they forward to kmalloc/kfree); this object stays in the link list so
 * the GPU build rule keeps producing a consistent object set, and is the
 * place for any further hosted-C glue (getenv, time, assertions).
 */

#include "../../mm/kheap.h"

#include <stddef.h>

void *ixg_hosted_calloc(size_t nmemb, size_t size)
{
    return kcalloc(nmemb, size);
}

void *ixg_hosted_realloc(void *ptr, size_t size)
{
    return krealloc(ptr, size);
}
