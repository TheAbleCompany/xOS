// SPDX-License-Identifier: BSD-3-Clause
//
// getdents() -- syscall 141 ArtyomX64.
//
// Ядро (lib/syscalls.c:getdents) пишет записи в формате linux_dirent:
// u32 d_ino, u16 d_reclen, u8 d_type, имя с завершающим NUL, длина
// выровнена до 8 байт. Курсор каталога живёт в file_t::pos, поэтому
// повторные вызовы продолжают листинг.

#include <unistd.h>
#include <errno.h>
#include <internal/syscall.h>
#include <internal/arch/x86_64/syscall_list.h>

int getdents(int fd, void *buf, unsigned int size)
{
	long ret;

	if (buf == NULL || size == 0) {
		errno = EINVAL;
		return -1;
	}

	ret = syscall(__NR_getdents, fd, buf, size);

	if (ret < 0) {
		errno = (int)(-ret);
		return -1;
	}

	return (int)ret;
}
