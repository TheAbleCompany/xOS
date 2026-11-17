/*
 * stdlib/atexit.c — atexit для mini-libc (ArtyomX64).
 *
 * Обработчики вызываются из exit() в порядке, обратном регистрации
 * (LIFO), как требует стандарт. Код выхода, переданный в exit,
 * обработчиками не изменяется.
 */

#include <stdlib.h>
#include <stddef.h>

#define ATEXIT_MAX 32

static void (*atexit_handlers[ATEXIT_MAX])(void);
static int atexit_count = 0;

int atexit(void (*function)(void))
{
        if (function == NULL)
                return -1;

        if (atexit_count >= ATEXIT_MAX)
                return -1;              /* таблица заполнена */

        atexit_handlers[atexit_count++] = function;
        return 0;
}

/* Вызывается из exit() (process/exit.c). */
void __libc_run_atexit(void)
{
        while (atexit_count > 0)
                atexit_handlers[--atexit_count]();
}
