#ifndef KERNEL_FPU_H
#define KERNEL_FPU_H

#include <stdint.h>

#define AX_FPU_AREA_SIZE 512u

void fpu_state_init(void *area);
void fpu_state_copy(void *dst, const void *src);
void fpu_reset_current(void);

#endif
