// SPDX-License-Identifier: BSD-3-Clause
//
// stdin/stdout/stderr.
//
// Буферы встроены в .bss — malloc для стандартных потоков не нужен
// (это важно: lua начинает печатать до каких-либо аллокаций).
//
// stdout и stderr — НЕ буферизованные (_IO_UNBUFFERED): интерактивный
// промпт lua "> " печатается без '\n' и обязан появиться до чтения.
// stdin буферизованный.

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

static unsigned char _stdin_buf[BUFSIZ];

static FILE _i_stdin = {
        ._fileno = 0,
        ._buf = _stdin_buf,
        ._p = _stdin_buf,
        ._r = 0,
        ._w = 0,
        ._flags = _IO_READ,
        ._bufsize = BUFSIZ,
};

static FILE _i_stdout = {
        ._fileno = 1,
        ._buf = NULL,
        ._p = NULL,
        ._r = 0,
        ._w = 0,
        ._flags = _IO_WRITE | _IO_UNBUFFERED,
        ._bufsize = 0,
};

static FILE _i_stderr = {
        ._fileno = 2,
        ._buf = NULL,
        ._p = NULL,
        ._r = 0,
        ._w = 0,
        ._flags = _IO_WRITE | _IO_UNBUFFERED,
        ._bufsize = 0,
};

FILE *stdin = &_i_stdin;
FILE *stdout = &_i_stdout;
FILE *stderr = &_i_stderr;

int feof(FILE *stream)
{
        if (stream == NULL)
                return 0;

        return (stream->_flags & _IO_EOF) ? 1 : 0;
}

int ferror(FILE *stream)
{
        if (stream == NULL)
                return 0;

        return (stream->_flags & _IO_ERR) ? 1 : 0;
}

void clearerr(FILE *stream)
{
        if (stream == NULL)
                return;

        stream->_flags &= ~(_IO_EOF | _IO_ERR);
}

int fileno(FILE *stream)
{
        if (stream == NULL) {
                errno = EBADF;
                return -1;
        }

        return stream->_fileno;
}

int fputs(const char *s, FILE *stream)
{
        size_t len = 0;

        if (s == NULL || stream == NULL) {
                errno = EINVAL;
                return EOF;
        }

        while (s[len])
                len++;

        if (len == 0)
                return 0;

        return (fwrite(s, 1, len, stream) == len) ? 0 : EOF;
}

/*
 * puts() осталась в io/puts.c (прямой write на fd 1) — здесь её
 * не дублируем.
 */
