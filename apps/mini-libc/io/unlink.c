// SPDX-License-Identifier: BSD-3-Clause
//
// unlink() — syscall 10 ArtyomX64, remove() — ANSI-обёртка над unlink.
//
// rename() -- syscall 82 (ядро: initrd_rename / automount_rename_at).

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <internal/syscall.h>
#include <internal/arch/x86_64/syscall_list.h>

int unlink(const char *pathname)
{
	long ret;

	if (pathname == NULL) {
		errno = EFAULT;
		return -1;
	}

	ret = syscall(__NR_unlink, pathname);

	if (ret < 0) {
		errno = (int)(-ret);
		return -1;
	}

	return 0;
}

int remove(const char *pathname)
{
	return unlink(pathname);
}

int rename(const char *oldpath, const char *newpath)
{
	long ret;

	if (oldpath == NULL || newpath == NULL) {
		errno = EFAULT;
		return -1;
	}

	/* syscall 82 -- ядро: initrd_rename()/automount_rename_at(). */
	ret = syscall(__NR_rename, oldpath, newpath);

	if (ret < 0) {
		errno = (int)(-ret);
		return -1;
	}

	return 0;
}
