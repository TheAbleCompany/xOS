#ifndef MESA_PORT_SYS_MMAN_H
#define MESA_PORT_SYS_MMAN_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);

#ifdef __cplusplus
}
#endif
#endif
