/*
 * tests-host/syscall_shim.c — хост-шим для тестов mini-libc на Linux.
 *
 * Та же методика, что и в порту lua-1.0 (см. apps/README-PORT.md):
 * код mini-libc собирается на хосте с -DARTYOM_HOST_SHIM=1, а номера
 * системных вызовов ArtyomX64 транслируются в syscall'и Linux.
 *
 * ВАЖНО: шим обращается к ядру НАПРЯМУЮ (инструкция syscall), а не
 * через glibc-функции: иначе имя write/read/close вытянет из архива
 * наш же libc-объект и получится бесконечная рекурсия
 * (write -> syscall -> __artym_host_syscall -> write -> ...).
 *
 * Номера системных вызовов ArtyomX64 (lib/syscalls.c ядра):
 *   1=exit 3=read 4=write 5=open 6=close 8=lseek(новый)
 *   10=unlink 11=exec 54=ioctl 141=getdents 151=yield
 *
 * Возврат — в стиле ядра: отрицательное значение = -errno.
 */

#include <stdlib.h>
#include <stddef.h>
#include <string.h>

static long raw_syscall3(long n, long a1, long a2, long a3)
{
        long ret;

        __asm__ __volatile__ ("syscall"
                              : "=a"(ret)
                              : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                              : "rcx", "r11", "memory");
        return ret;
}

/* см. комментарий в map-функции */
static char axroot_buf[4096];

static const char *axroot_map(const char *path)
{
        /* getenv здесь нельзя: наш env.c из libc-shim перекрывает
         * glibc getenv. Читаем окружение процесса напрямую. */
        extern char **__environ;
        const char *root = NULL;
        {
                char **e;
                for (e = __environ; e && *e; e++) {
                        if (e[0][0] == 'A' && e[0][1] == 'X' &&
                            strncmp(e[0], "AXROOT=", 7) == 0) {
                                root = *e + 7;
                                break;
                        }
                }
        }
        const char *p = path;
        long k = 0;

        if (root == NULL || root[0] == '\0')
                return path;    /* без ремапа */

        while (*p == '/')
                p++;

        {
                const char *r = root;
                while (*r && k < (long)sizeof(axroot_buf) - 2)
                        axroot_buf[k++] = *r++;
        }
        if (k == 0 || axroot_buf[k - 1] != '/')
                axroot_buf[k++] = '/';
        while (*p && k < (long)sizeof(axroot_buf) - 1)
                axroot_buf[k++] = *p++;
        axroot_buf[k] = '\0';
        return axroot_buf;
}

long __artym_host_syscall(long n, long a1, long a2, long a3,
                          long a4, long a5, long a6)
{
        (void)a4;
        (void)a5;
        (void)a6;

        switch (n) {
        case 1: /* exit — ArtyomX64: завершить задачу */
                (void)raw_syscall3(231 /* SYS_exit_group */, a1, 0, 0);
                for (;;)
                        ;

        case 3: /* read */
                return raw_syscall3(0 /* SYS_read */, a1, a2, a3);

        case 4: /* write */
                return raw_syscall3(1 /* SYS_write */, a1, a2, a3);

        case 5: { /* open(path, flags, mode) + AXROOT remap */
                if (a1 && ((const char *)a1)[0] == '/')
                        a1 = (long)axroot_map((const char *)a1);
                return raw_syscall3(2 /* SYS_open */, a1, a2, a3);
        }

        case 6: /* close */
                return raw_syscall3(3 /* SYS_close */, a1, 0, 0);

        case 8: /* lseek(fd, offset, whence) — как в патче ядра */
                return raw_syscall3(8 /* SYS_lseek */, a1, a2, a3);

        case 10: /* unlink */
                return raw_syscall3(87 /* SYS_unlink */, a1, 0, 0);

        case 11: /* exec — на хосте не эмулируем (нет fork/wait) */
                return -38;     /* -ENOSYS */

        case 35: /* nanosleep — номер совпадает с Linux SYS_nanosleep */
                return raw_syscall3(35, a1, a2, 0);

        case 96: /* gettimeofday — номер совпадает с Linux SYS_gettimeofday */
                return raw_syscall3(96, a1, a2, 0);

        case 228: /* clock_gettime — номер совпадает с Linux SYS_clock_gettime */
                return raw_syscall3(228, a1, a2, 0);

        /*
         * mmap (9) сюда НЕ пробрасывается сознательно:
         * 1) на ОС ядро всё равно вернёт -1 — эмуляция в mm/mmap.c
         *    даёт то же поведение, что и на реальной системе;
         * 2) munmap в ArtyomX64 невозможен корректно: номер 11 занят
         *    exec(). Эмулированные отображения освобождаются через
         *    реестр в mm/mmap.c (free), поэтому полный цикл
         *    mmap/munmap run.c работает одинаково на хосте и на ОС.
         */

        case 54: /* ioctl — на хосте не трогаем tty хоста */
                return -25;     /* -ENOTTY */

        case 141: /* getdents — заглушка */
                return -38;     /* -ENOSYS */

        case 151: /* yield */
                return 0;

        default:
                /* Неизвестный syscall — как в ядре: вернуть -1. */
                return -1;
        }
}
