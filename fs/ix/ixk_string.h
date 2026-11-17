// ixk_string.h -- ix_memset/ix_memcpy/ixk_string_copy_n.
//
// Восстановлено под ArtyomX64 (см. примечание в ixstd_fs.h).
// Сделаны как самостоятельные static inline реализации (не переиспользуют
// lib/common.h memcpy/memset ArtyomX64, у которых немного другая сигнатура
// -- u8int* вместо void*), чтобы ix_fs_fat.c компилировался без изменений.

#pragma once

#include <stdint.h>
#include <stddef.h>

static inline void *ix_memset(void *dst, int val, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < len; i++)
        d[i] = (uint8_t)val;
    return dst;
}

static inline void *ix_memcpy(void *dst, const void *src, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < len; i++)
        d[i] = s[i];
    return dst;
}

// Аналог memcmp: 0 при равенстве, <0/>0 при первом различии.
static inline int ix_memcmp(const void *a, const void *b, uint32_t len)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (uint32_t i = 0; i < len; i++) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }
    return 0;
}

// Копирует src в dst как C-строку, гарантированно завершая нулём,
// не выходя за границу cap байт (аналог strlcpy).
static inline void ixk_string_copy_n(char *dst, const char *src, uint32_t cap)
{
    if (cap == 0)
        return;
    uint32_t i = 0;
    for (; i < cap - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}
