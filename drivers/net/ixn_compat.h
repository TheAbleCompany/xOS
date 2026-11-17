#ifndef IXN_COMPAT_H
#define IXN_COMPAT_H

//#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "ixn_types.h"
#include "ixn_arch.h"
#include "ixn_kernel_port.h"

#define ixn_memset(p, v, n)  memset((p), (v), (n))
#define ixn_memcpy(d, s, n)  memcpy((d), (s), (n))
#define ixn_memmove(d, s, n) memmove((d), (s), (n))
#define ixn_memcmp(a, b, n)  memcmp((a), (b), (n))
#define ixn_strlen(s)        ((ixn_size)strlen((s)))
#define ixn_strncpy          strncpy
#define ixn_strcmp(a, b)     strcmp((a), (b))
#define ixn_strncmp(a, b, n) strncmp((a), (b), (n))
#define ixn_strcpy           strcpy
#define ixn_strchr           strchr
#define ixn_strstr           strstr

#endif
