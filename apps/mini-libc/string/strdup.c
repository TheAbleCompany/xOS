// SPDX-License-Identifier: BSD-3-Clause

#include <stdlib.h>
#include <string.h>
#include <errno.h>

char *strdup(const char *s)
{
	size_t len;
	char *p;

	if (s == NULL) {
		errno = EFAULT;
		return NULL;
	}

	len = strlen(s) + 1;

	p = malloc(len);
	if (p == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	memcpy(p, s, len);
	return p;
}
