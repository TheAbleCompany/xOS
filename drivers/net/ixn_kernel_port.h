#ifndef IXN_KERNEL_PORT_H
#define IXN_KERNEL_PORT_H

#include "ixn_types.h"

/*
 * Кооперативный планировщик: сетевой стек крутит длинные poll-циклы
 * (arp_ensure, ping-ожидание) в контексте задачи.  Чтобы не замораживать
 * систему, между итерациями драйвер дёргает этот hook.  Ставится в
 * yield() после initTasking(); до планировщика (selftest на раннем
 * буте) остаётся NULL -- тогда стек просто крутится, как раньше.
 */
extern void (*ixn_yield_hook)(void);

void  ixn_port_init(void);

ixn_u64 ixn_ticks_ms(void);

void  ixn_log(const char *s);
void  ixn_logf(const char *fmt, ...);
void  ixn_log_hex32(ixn_u32 v);

ixn_u8 volatile *ixn_phys_to_virt(ixn_u64 phys);
ixn_u64 ixn_virt_to_phys(const void *virt);

void *ixn_alloc(ixn_size size);
void *ixn_alloc_zero(ixn_size size);
void  ixn_free(void *p);

void *ixn_dma_alloc(ixn_size size, ixn_u64 *out_phys);
void  ixn_dma_free(void *ptr, ixn_size size);

#endif
