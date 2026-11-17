// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

int ungetc(int c, FILE *stream)
{
	if (stream == NULL || c == EOF) {
		errno = EINVAL;
		return EOF;
	}

	if (!(stream->_flags & _IO_READ)) {
		errno = EBADF;
		return EOF;
	}

	/*
	 * Классическое ограничение: гарантируется возврат ровно одного
	 * символа. Если буфер чтения не использовался — разворачиваем
	 * буфер на одну позицию.
	 */
	if (stream->_p > stream->_buf) {
		stream->_p--;
		*stream->_p = (unsigned char)c;
		stream->_r++;

		stream->_flags &= ~_IO_EOF;
		return (unsigned char)c;
	}

	/* Буфер пуст и _p == _buf: места для отката нет. */
	errno = EINVAL;
	return EOF;
}
