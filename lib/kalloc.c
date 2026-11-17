/*
 * lib/kalloc.c -- kernel-only allocator entry points used by the ixg
 * driver and anything else written against hosted C.
 *
 * Kept out of lib/libc.c on purpose: libc.c is also linked into
 * userspace binaries (wserver, apps), which have their own heap, so
 * referencing kmalloc from there would drag kernel symbols into ring 0
 * clients.  Nothing in the kernel build except this file needs them.
 */

#include "common.h"
#include "../mm/kheap.h"

#include <stddef.h>

void *malloc(size_t size)
{
    if (size == 0)
        size = 1;

    return kmalloc(size);
}

void free(void *ptr)
{
    kfree(ptr);
}

void *calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0)
        return kmalloc(1);

    return kcalloc(nmemb, size);
}

void *realloc(void *ptr, size_t size)
{
    return krealloc(ptr, size);
}

void __attribute__((noreturn)) abort(void)
{
    panic("abort()", __FILE__, __LINE__);
    for (;;) { }
}
