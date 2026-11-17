// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

int fflush(FILE *stream)
{
	if (stream == NULL)
		return 0;	/* остальных буферов у нас нет */

	/* Небуферизованный поток сбрасывать нечего. */
	if (stream->_flags & _IO_UNBUFFERED)
		return 0;

	/* Читающий поток: буфер чтения не сбрасывается. */
	if (!(stream->_flags & _IO_WRITE))
		return 0;

	if (stream->_w > 0 && stream->_buf != NULL) {
		ssize_t written = write(stream->_fileno, stream->_buf,
					stream->_w);

		if (written != (ssize_t)stream->_w) {
			stream->_flags |= _IO_ERR;
			errno = EIO;
			return EOF;
		}
	}

	stream->_p = stream->_buf;
	stream->_w = 0;

	return 0;
}
