// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

int fputc(int c, FILE *stream)
{
	unsigned char ch = (unsigned char)c;

	if (stream == NULL) {
		errno = EINVAL;
		return EOF;
	}

	if (!(stream->_flags & _IO_WRITE)) {
		errno = EBADF;
		stream->_flags |= _IO_ERR;
		return EOF;
	}

	if (stream->_flags & _IO_ERR)
		return EOF;

	/* Небуферизованный поток — прямой write(). */
	if (stream->_flags & _IO_UNBUFFERED) {
		ssize_t w = write(stream->_fileno, &ch, 1);

		if (w != 1) {
			errno = EIO;
			stream->_flags |= _IO_ERR;
			return EOF;
		}

		return ch;
	}

	/* Буферизованный поток. */
	if (stream->_buf == NULL) {
		stream->_buf = malloc(stream->_bufsize);
		if (stream->_buf == NULL) {
			stream->_flags |= _IO_ERR;
			errno = ENOMEM;
			return EOF;
		}
		stream->_p = stream->_buf;
		stream->_w = 0;
	}

	if (stream->_w >= stream->_bufsize) {
		if (fflush(stream) == EOF)
			return EOF;
	}

	*stream->_p++ = ch;
	stream->_w++;

	/* '\n' в строковом режиме — сбрасываем. */
	if (ch == '\n' && (stream->_flags & _IO_LINE_BUF)) {
		if (fflush(stream) == EOF)
			return EOF;
	}

	return ch;
}
