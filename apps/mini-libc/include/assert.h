/*
 * include/assert.h — mini-libc (ArtyomX64).
 */

#include <stddef.h>     /* size_t */

#ifndef _ASSERT_H
#define _ASSERT_H

#ifndef _ASSERT_H_INTERNAL

void __assert_fail(const char *expr, const char *file, unsigned int line,
                   const char *func);

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
        ((expr) ? (void)0 : \
         __assert_fail(#expr, __FILE__, __LINE__, __func__))
#endif

#endif /* _ASSERT_H_INTERNAL */

#endif /* _ASSERT_H */
