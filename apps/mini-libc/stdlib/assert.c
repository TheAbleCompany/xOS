/*
 * stdlib/assert.c — assert для mini-libc (ArtyomX64).
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void __assert_fail(const char *expr, const char *file, unsigned int line,
                   const char *func)
{
        fprintf(stderr, "Assertion failed: %s (%s: %s: %u)\n",
                expr, file, func ? func : "?", line);
        fflush(NULL);
        abort();
}
