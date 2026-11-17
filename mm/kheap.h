#ifndef KHEAP_H
#define KHEAP_H

#include <stddef.h>

typedef struct {
    size_t total_bytes;
    size_t used_bytes;
    size_t free_bytes;
    size_t largest_free;
    size_t block_count;
    size_t free_block_count;
    size_t arena_count;
} kheap_stats_t;

void  kheap_init(void);

void *kmalloc(size_t size);
void  kfree(void *ptr);
void *kcalloc(size_t nmemb, size_t size);
void *krealloc(void *ptr, size_t size);
void *kmalloc_aligned(size_t size, size_t align);

size_t kheap_usable_size(const void *ptr);
void   kheap_stats(kheap_stats_t *out);

#endif
