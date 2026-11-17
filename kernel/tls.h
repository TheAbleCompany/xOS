#ifndef KERNEL_TLS_H
#define KERNEL_TLS_H

#include <stdint.h>
#include <stddef.h>

void     ax_tls_write_fs_base(uint64_t base);
uint64_t ax_tls_read_fs_base(void);
size_t   ax_tls_image_size(void);
void    *ax_tls_block_alloc(void);
void     ax_tls_block_free(void *block);
uint64_t ax_tls_block_fs_base(void *block);

#endif
