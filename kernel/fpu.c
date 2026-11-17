#include "fpu.h"

#include <stddef.h>

#define FXSAVE_FCW_OFF    0u
#define FXSAVE_FSW_OFF    2u
#define FXSAVE_FTW_OFF    4u
#define FXSAVE_FOP_OFF    6u
#define FXSAVE_MXCSR_OFF  24u
#define FXSAVE_MXCSR_MASK 28u
#define FXSAVE_ST_OFF     32u
#define FXSAVE_XMM_OFF    160u
#define FXSAVE_SIZE       512u

#define AX_FCW_DEFAULT    0x037Fu
#define AX_MXCSR_DEFAULT  0x1F80u

static inline void store_u16(void *base, size_t off, uint16_t v)
{
    *(volatile uint16_t *)((unsigned char *)base + off) = v;
}

static inline void store_u32(void *base, size_t off, uint32_t v)
{
    *(volatile uint32_t *)((unsigned char *)base + off) = v;
}

void fpu_state_init(void *area)
{
    if (!area)
        return;

    unsigned char *p = (unsigned char *)area;

    for (size_t i = 0; i < FXSAVE_SIZE; i++)
        p[i] = 0;

    store_u16(p, FXSAVE_FCW_OFF, AX_FCW_DEFAULT);
    store_u16(p, FXSAVE_FSW_OFF, 0);
    store_u16(p, FXSAVE_FTW_OFF, 0);
    store_u16(p, FXSAVE_FOP_OFF, 0);
    store_u32(p, FXSAVE_MXCSR_OFF, AX_MXCSR_DEFAULT);
    store_u32(p, FXSAVE_MXCSR_MASK, 0xFFFFu);

    (void)FXSAVE_ST_OFF;
    (void)FXSAVE_XMM_OFF;
}

void fpu_state_copy(void *dst, const void *src)
{
    if (!dst || !src)
        return;

    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    for (size_t i = 0; i < FXSAVE_SIZE; i++)
        d[i] = s[i];
}

void fpu_reset_current(void)
{
    uint32_t mxcsr = AX_MXCSR_DEFAULT;

    __asm__ volatile("fninit");
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}
