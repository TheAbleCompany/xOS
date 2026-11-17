// SPDX-License-Identifier: BSD-3-Clause

#include <fcntl.h>
#include <internal/syscall.h>
#include <stdarg.h>
#include <errno.h>

int open(const char *filename, int flags, ...)
{
	int mode = 0;
	int ret;

	/*
	 * mode читаем только когда он реально передан (O_CREAT) —
	 * иначе va_arg за пределами списка аргументов.
	 */
	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}

	ret = syscall(__NR_open, filename, flags, mode);

	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	return ret;
}
