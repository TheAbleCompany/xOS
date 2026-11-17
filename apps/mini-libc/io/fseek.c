/*
 * io/fseek.c — fseek/ftell/rewind/fgetpos/fsetpos для mini-libc.
 *
 * Требует системный вызов lseek (номер 8, добавлен в ядро ArtyomX64).
 *
 * Логика с буфером (FILE из mini-libc: _p — курсор чтения, _r —
 * байтов в буфере осталось):
 *   - перед seek сбрасываем write-сторону через fflush();
 *   - позиция в файле у fd при прочитанном-but-непотраченном буфере
 *     УШЕДША вперёд на _r байт => ftell = lseek(cur) - _r;
 *   - после seek буфер чтения инвалидация: _r = 0, _p = _buf,
 *     сброс _IO_EOF.
 */

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

int fseek(FILE *stream, long offset, int whence)
{
        long ret;

        if (stream == NULL) {
                errno = EINVAL;
                return -1;
        }

        if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
                errno = EINVAL;
                return -1;
        }

        /* Сначала сбрасываем незаписанный буфер (если есть). */
        if (fflush(stream) != 0)
                return -1;

        /*
         * SEEK_CUR отсчитывается от ЛОГИЧЕСКОЙ позиции, но fd у ядра
         * ушёл вперёд на _r непрочитанных байт буфера чтения.
         */
        if (whence == SEEK_CUR && stream->_r > 0)
                offset -= (long)stream->_r;

        ret = lseek(stream->_fileno, offset, whence);
        if (ret < 0) {
                /* errno уже выставлен lseek(). */
                return -1;
        }

        /* Инвалидация буфера чтения. */
        stream->_r = 0;
        stream->_p = stream->_buf;
        stream->_flags &= (unsigned)~_IO_EOF;
        stream->_flags &= (unsigned)~_IO_ERR;

        return 0;
}

long ftell(FILE *stream)
{
        long cur;

        if (stream == NULL) {
                errno = EINVAL;
                return -1;
        }

        cur = lseek(stream->_fileno, 0, SEEK_CUR);
        if (cur < 0)
                return -1;

        /* Непрочитанный хвост буфера отодвигает логическую позицию назад. */
        if (stream->_r > 0)
                cur -= (long)stream->_r;

        return cur;
}

void rewind(FILE *stream)
{
        if (stream == NULL)
                return;

        (void)fseek(stream, 0L, SEEK_SET);
        stream->_flags &= (unsigned)~_IO_EOF;
}

int fgetpos(FILE *stream, fpos_t *pos)
{
        long v;

        if (pos == NULL) {
                errno = EINVAL;
                return -1;
        }

        v = ftell(stream);
        if (v < 0)
                return -1;

        *pos = (fpos_t)v;
        return 0;
}

int fsetpos(FILE *stream, const fpos_t *pos)
{
        if (pos == NULL) {
                errno = EINVAL;
                return -1;
        }

        return fseek(stream, (long)*pos, SEEK_SET);
}
