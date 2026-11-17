// SPDX-License-Identifier: BSD-3-Clause
//
// process/exit.c — завершение задачи (mini-libc, ArtyomX64).
//
// exit() перед выходом запускает обработчики atexit() (LIFO,
// см. stdlib/atexit.c) и сбрасывает пользовательские потоки
// stdio. _exit() завершает немедленно, ничего не трогая.
//

#include <internal/syscall.h>
#include <stdlib.h>
#include <stdio.h>

void __libc_run_atexit(void);

long exit(long exit_code)
{
        /* Порядок по стандарту: atexit-обработчики, потом flush stdio. */
        __libc_run_atexit();
        fflush(NULL);

        return syscall(__NR_exit, exit_code);
}

void _exit(int exit_code)
{
        syscall(__NR_exit, (long)exit_code);

        /* Сюда попасть нельзя. */
        for (;;)
                ;
}
