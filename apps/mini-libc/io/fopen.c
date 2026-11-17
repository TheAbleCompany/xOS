// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

FILE *fopen(const char *path, const char *mode)
{
        int flags = 0;
        int fd;
        FILE *file;

        if (path == NULL || mode == NULL) {
                errno = EINVAL;
                return NULL;
        }

        /* Разбираем режим открытия ('b' игнорируется — ОС бинарно-прозрачна). */
        switch (mode[0]) {
        case 'r':
                flags = (mode[1] == '+') ? O_RDWR : O_RDONLY;
                break;

        case 'w':
                flags = (mode[1] == '+') ?
                        (O_RDWR | O_CREAT | O_TRUNC) :
                        (O_WRONLY | O_CREAT | O_TRUNC);
                break;

        case 'a':
                flags = (mode[1] == '+') ?
                        (O_RDWR | O_CREAT | O_APPEND) :
                        (O_WRONLY | O_CREAT | O_APPEND);
                break;

        default:
                errno = EINVAL;
                return NULL;
        }

        fd = open(path, flags, 0644);
        if (fd < 0)
                return NULL;

        file = malloc(sizeof(FILE));
        if (file == NULL) {
                close(fd);
                errno = ENOMEM;
                return NULL;
        }

        file->_buf = malloc(BUFSIZ);
        if (file->_buf == NULL) {
                /* Работаем без буфера, чем зря терять файл. */
                free(file);
                close(fd);
                errno = ENOMEM;
                return NULL;
        }

        file->_fileno = fd;
        file->_p = file->_buf;
        file->_r = 0;
        file->_w = 0;
        file->_flags = 0;

        if (mode[0] == 'r')
                file->_flags |= _IO_READ;
        else
                file->_flags |= _IO_WRITE;

        /* режим '+' — и чтение, и запись (w+b/r+ и т.п.) */
        if (mode[1] == '+')
                file->_flags |= (_IO_READ | _IO_WRITE);

        file->_bufsize = BUFSIZ;

        return file;
}

FILE *fdopen(int fd, const char *mode)
{
        FILE *file;

        if (mode == NULL) {
                errno = EINVAL;
                return NULL;
        }

        file = malloc(sizeof(FILE));
        if (file == NULL) {
                errno = ENOMEM;
                return NULL;
        }

        file->_buf = malloc(BUFSIZ);
        if (file->_buf == NULL) {
                free(file);
                errno = ENOMEM;
                return NULL;
        }

        file->_fileno = fd;
        file->_p = file->_buf;
        file->_r = 0;
        file->_w = 0;
        file->_flags = 0;

        if (mode[0] == 'r')
                file->_flags |= _IO_READ;
        else
                file->_flags |= _IO_WRITE;

        /* режим '+' — и чтение, и запись */
        if (mode[1] == '+')
                file->_flags |= (_IO_READ | _IO_WRITE);

        file->_bufsize = BUFSIZ;

        return file;
}

int fclose(FILE *stream)
{
        int result;

        if (stream == NULL) {
                errno = EBADF;
                return EOF;
        }

        fflush(stream);

        if (stream->_buf != NULL) {
                free(stream->_buf);
                stream->_buf = NULL;
        }

        result = close(stream->_fileno);
        free(stream);

        return (result == 0) ? 0 : EOF;
}
