#ifndef MESA_PORT_SYS_IOCTL_H
#define MESA_PORT_SYS_IOCTL_H
#ifdef __cplusplus
extern "C" {
#endif
int ioctl(int fd, unsigned long req, ...);

#ifdef __cplusplus
}
#endif
#endif
