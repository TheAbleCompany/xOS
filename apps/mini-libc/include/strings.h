/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef __STRINGS_H__
#define __STRINGS_H__    1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int strcasecmp(const char *str1, const char *str2);
int strncasecmp(const char *str1, const char *str2, size_t len);

#ifdef __cplusplus
}
#endif

#endif
