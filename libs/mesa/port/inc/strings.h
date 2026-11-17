#ifndef MESA_PORT_STRINGS_H
#define MESA_PORT_STRINGS_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);


#ifdef __cplusplus
}
#endif
#endif
