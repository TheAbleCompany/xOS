// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

/*
 * fwrite/fread поверх syscall read/write.
 *
 * Логика буферизации:
 *   - _IO_UNBUFFERED — сразу write();
 *   - запись — копим в _buf, по заполнении сбрасываем;
 *   - чтение — fgetc() (io/fgetc.c) наполняет _buf сам.
 */

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
        size_t total;
        size_t members;
        const unsigned char *p = ptr;

        if (stream == NULL || ptr == NULL) {
                errno = EINVAL;
                return 0;
        }

        if (!(stream->_flags & _IO_WRITE)) {
                errno = EBADF;
                stream->_flags |= _IO_ERR;
                return 0;
        }

        if (size == 0 || nmemb == 0)
                return 0;

        total = size * nmemb;
        members = total / size;

        /* Небуферизованный поток — пишем одним вызовом. */
        if (stream->_flags & _IO_UNBUFFERED) {
                ssize_t w = write(stream->_fileno, p, total);

                if (w < 0) {
                        errno = EIO;
                        stream->_flags |= _IO_ERR;
                        return 0;
                }

                return (size_t)w / size;
        }

        /* Буферизованный поток. */
        {
                size_t done = 0;

                while (done < total) {
                        if (stream->_buf == NULL) {
                                stream->_buf = malloc(stream->_bufsize);
                                if (stream->_buf == NULL) {
                                        errno = ENOMEM;
                                        stream->_flags |= _IO_ERR;
                                        return 0;
                                }
                                stream->_p = stream->_buf;
                                stream->_w = 0;
                        }

                        if (stream->_w >= stream->_bufsize) {
                                if (fflush(stream) == EOF)
                                        return done / size;
                        }

                        stream->_buf[stream->_w++] = p[done++];
                }

                if (fflush(stream) == EOF)
                        return 0;

                return members;
        }
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
        unsigned char *dst = ptr;
        size_t total = size * nmemb;
        size_t got = 0;

        if (stream == NULL || ptr == NULL) {
                errno = EINVAL;
                return 0;
        }

        if (!(stream->_flags & _IO_READ)) {
                errno = EBADF;
                stream->_flags |= _IO_ERR;
                return 0;
        }

        while (got < total) {
                int c = fgetc(stream);

                if (c == EOF)
                        break;

                dst[got++] = (unsigned char)c;
        }

        return (size ? got / size : 0);
}

char *fgets(char *s, int size, FILE *stream)
{
        int i = 0;

        if (s == NULL || stream == NULL || size <= 0)
                return NULL;

        while (i < size - 1) {
                int c = fgetc(stream);

                if (c == EOF) {
                        if (i == 0)
                                return NULL;
                        break;
                }

                s[i++] = (char)c;

                if (c == '\n')
                        break;
        }

        s[i] = '\0';
        return s;
}

int getchar(void)
{
        return fgetc(stdin);
}

int getc(FILE *stream)
{
        return fgetc(stream);
}
