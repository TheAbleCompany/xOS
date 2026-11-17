/*
 * stdlib/misc.c — мелкие POSIX-функции, нужные tcc (mini-libc).
 *
 * execvp: в ядре exec(11) ЗАМЕНЯЕТ задачу (нет fork/wait), поэтому
 * для "найти и запустить" его использовать нельзя. tcc вызывает
 * execvp только в утилите кросс-компиляции (tcc -m32), которая
 * на ArtyomX64 не поддерживается — возвращаем ошибку.
 *
 * mprotect: память задач ArtyomX64 RWX, защита не нужна —
 * возвращаем успех.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <internal/syscall.h>
#include <internal/arch/x86_64/syscall_list.h>

int execvp(const char *file, char *const argv[])
{
        (void)file;
        (void)argv;
        errno = ENOSYS;
        return -1;
}

int execv(const char *path, char *const argv[])
{
        (void)path;
        (void)argv;
        errno = ENOSYS;
        return -1;
}

int mprotect(void *addr, size_t len, int prot)
{
        (void)addr;
        (void)len;
        (void)prot;
        /* Страницы задач ArtyomX64 RWX — менять нечего. */
        return 0;
}

/*
 * mkdir: syscall 83.
 *
 * Дерево initrd плоское -- настоящих каталогов ядро не создаёт,
 * но вызов обязан завершаться успехом: Doom зовёт M_MakeDirectory()
 * перед записью сохранения и без успеха отказывается сохраняться.
 */
int mkdir(const char *path, mode_t mode)
{
        long ret;

        (void)mode;

        if (path == NULL) {
                errno = EFAULT;
                return -1;
        }

        ret = syscall(__NR_mkdir, path, (long)mode);

        if (ret < 0) {
                errno = (int)(-ret);
                return -1;
        }

        return 0;
}
