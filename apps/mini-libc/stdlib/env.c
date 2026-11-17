/*
 * stdlib/env.c — окружение процесса для mini-libc (ArtyomX64).
 *
 * Ядро ArtyomX64 передаёт задаче только argc/argv (exec_args_t),
 * environ не передаётся. Поэтому getenv всегда возвращает NULL,
 * а setenv/putenv/unsetenv/clearenv — безвредные заглушки.
 */

#include <stdlib.h>
#include <string.h>

char *environ[1] = { NULL };

char *getenv(const char *name)
{
        (void)name;
        /* Переменных окружения в ArtyomX64 нет (exec не передаёт envp). */
        return NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
        (void)name;
        (void)value;
        (void)overwrite;
        return -1;
}

int unsetenv(const char *name)
{
        (void)name;
        return -1;
}

int putenv(char *string)
{
        (void)string;
        return -1;
}

int clearenv(void)
{
        return 0;
}

/*
 * getcwd: текущего каталога в ядре ArtyomX64 нет (нет chdir),
 * поэтому "текущий каталог" — это корень "/". Этого достаточно
 * для tcc (comp_dir в DWARF, пути tcov).
 */
char *getcwd(char *buf, size_t size)
{
        if (buf == NULL || size == 0) {
                return NULL;
        }

        if (size < 2)
                return NULL;

        buf[0] = '/';
        buf[1] = '\0';
        return buf;
}

/*
 * realpath: канонический абсолютный путь. Файловая система плоская,
 * символьных ссылок и "."/".." нет, поэтому канонизация — это
 * "схлопнуть повторные '/' и дать абсолютный путь от корня".
 *
 * Если resolved_path == NULL, буфер выделяется malloc'ом
 * (вызывающий обязан free) — так его использует tcc.
 */
char *realpath(const char *path, char *resolved_path)
{
        char *out;
        size_t len;
        const char *p;

        if (path == NULL || path[0] == '\0') {
                return NULL;
        }

        /* Считаем длину со схлопыванием повторных '/'. */
        p = path;
        while (*p == '/')
                p++;
        len = 1;                /* ведущий '/' */
        for (; *p; p++) {
                if (*p == '/' && p[1] == '/')
                        continue;
                len++;
        }

        if (len + 1 > PATH_MAX)
                return NULL;

        if (resolved_path == NULL) {
                out = malloc(len + 1);
                if (out == NULL)
                        return NULL;
        } else {
                out = resolved_path;
        }

        {
                char *w = out;
                p = path;
                while (*p == '/')
                        p++;
                *w++ = '/';
                for (; *p; p++) {
                        if (*p == '/' && p[1] == '/')
                                continue;
                        if (*p == '/' && w[-1] == '/')
                                continue;
                        *w++ = *p;
                }
                /* срезаем хвостовой '/' (кроме корня) */
                if (w > out + 1 && w[-1] == '/')
                        w--;
                *w = '\0';
        }

        return out;
}
