#include "axsock.h"

int socket(int domain, int type, int protocol)
{
    return (int)ax_syscall3(200, domain, type, protocol);
}

int bind(int fd, const ax_sockaddr_t *addr, axsock_socklen_t len)
{
    return (int)ax_syscall3(201, fd, addr, len);
}

int connect(int fd, const ax_sockaddr_t *addr, axsock_socklen_t len)
{
    return (int)ax_syscall3(202, fd, addr, len);
}

int listen(int fd, int backlog)
{
    return (int)ax_syscall2(203, fd, backlog);
}

int accept(int fd, ax_sockaddr_t *addr, axsock_socklen_t *len)
{
    return (int)ax_syscall3(204, fd, addr, len);
}

int sendto(int fd, const void *buf, size_t len, int flags,
           const ax_sockaddr_t *addr, axsock_socklen_t alen)
{
    return (int)ax_syscall6(205, (long)fd, (long)buf, (long)len,
                            (long)flags, (long)addr, (long)alen);
}

int recvfrom(int fd, void *buf, size_t len, int flags,
             ax_sockaddr_t *addr, axsock_socklen_t *alen)
{
    return (int)ax_syscall6(206, (long)fd, (long)buf, (long)len,
                            (long)flags, (long)addr, (long)alen);
}

int send(int fd, const void *buf, size_t len, int flags)
{
    return sendto(fd, buf, len, flags, 0, 0);
}

int recv(int fd, void *buf, size_t len, int flags)
{
    return recvfrom(fd, buf, len, flags, 0, 0);
}

int shutdown(int fd, int how)
{
    return (int)ax_syscall2(207, fd, how);
}

int getsockname(int fd, ax_sockaddr_t *addr, axsock_socklen_t *len)
{
    return (int)ax_syscall3(208, fd, addr, len);
}

int getpeername(int fd, ax_sockaddr_t *addr, axsock_socklen_t *len)
{
    return (int)ax_syscall3(209, fd, addr, len);
}

int setsockopt(int fd, int level, int optname, const void *optval, axsock_socklen_t optlen)
{
    return (int)ax_syscall5(210, fd, level, optname, optval, optlen);
}

int getsockopt(int fd, int level, int optname, void *optval, axsock_socklen_t *optlen)
{
    return (int)ax_syscall5(211, fd, level, optname, optval, optlen);
}

int socketpair(int domain, int type, int protocol, int sv[2])
{
    return (int)ax_syscall4(212, domain, type, protocol, sv);
}
