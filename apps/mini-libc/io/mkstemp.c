/*
 * io/mkstemp.c — mkstemp для mini-libc (ArtyomX64).
 *
 * В ядре нет getpid и O_EXCL, поэтому уникальность имени
 * обеспечивается статическим счётчиком (в .bss задачи) —
 * внутри одной задачи коллизий нет, между задачами имена
 * различаются произвольной частью счётчика с большой базой.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static unsigned int mkstemp_counter = 0x9E3779B9u; /* golden ratio seed */

static const char b64c[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

int mkstemp(char *template)
{
        size_t len;
        int i, fd;
        unsigned int v;
        char *xs;

        if (template == NULL) {
                errno = EINVAL;
                return -1;
        }

        len = strlen(template);
        if (len < 6 ||
            strcmp(template + len - 6, "XXXXXX") != 0) {
                errno = EINVAL;
                return -1;
        }

        xs = template + len - 6;

        for (i = 0; i < 4096; i++) {
                /* Счётчик -> 6 символов base64-подобного алфавита. */
                v = ++mkstemp_counter;
                for (int k = 0; k < 6; k++) {
                        xs[k] = b64c[v & 63];
                        v >>= 6;
                }

                fd = open(template, O_RDWR | O_CREAT, 0600);
                if (fd >= 0)
                        return fd;
                if (errno != EEXIST)
                        return -1;
        }

        errno = EEXIST;
        return -1;
}
