#ifndef MESA_PORT_POLL_H
#define MESA_PORT_POLL_H
#ifdef __cplusplus
extern "C" {
#endif
struct pollfd { int fd; short events; short revents; };
int poll(struct pollfd *fds, unsigned long n, int timeout);

#ifdef __cplusplus
}
#endif
#endif
