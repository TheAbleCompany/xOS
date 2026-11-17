/*
 * stdlib/abort.c — abort() для mini-libc (ArtyomX64).
 *
 * Сигналов в ядре нет, поэтому abort() печатает сообщение в stderr
 * и завершает задачу с кодом 134 (128 + SIGABRT — для совместимости
 * с соглашением оболочек).
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

void abort(void)
{
        const char msg[] = "abort() called\n";

        /* stderr небуферизованный, пишем напрямую. */
        (void)write(2, msg, sizeof(msg) - 1);

        /* На всякий случай сбрасываем потоки перед смертью. */
        fflush(NULL);

        exit(134);

        /* exit() не возвращается. */
        for (;;)
                ;
}
