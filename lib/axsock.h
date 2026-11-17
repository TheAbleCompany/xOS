#ifndef AUXSOCK_H
#define AUXSOCK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t axsock_ssize_t;
typedef uint32_t axsock_socklen_t;

#define AX_AF_UNSPEC 0u
#define AX_AF_UNIX   1u
#define AX_AF_INET   2u

#define AX_SOCK_STREAM    1u
#define AX_SOCK_DGRAM     2u
#define AX_SOCK_RAW       3u
#define AX_SOCK_SEQPACKET 5u

#define AX_SOL_SOCKET  0xFFFFu
#define AX_SO_REUSEADDR 0x0004u
#define AX_SO_KEEPALIVE 0x0008u
#define AX_SO_BROADCAST 0x0020u
#define AX_SO_RCVBUF    0x1002u
#define AX_SO_SNDBUF    0x1001u
#define AX_SO_NONBLOCK  0x4000u
#define AX_SO_ERROR     0x1007u

#define AX_MSG_OOB      0x0001u
#define AX_MSG_PEEK     0x0002u
#define AX_MSG_DONTWAIT 0x0040u

#define AX_SHUT_RD   0u
#define AX_SHUT_WR   1u
#define AX_SHUT_RDWR 2u

#define AX_UN_PATH_MAX 108

typedef struct {
    uint16_t sa_family;
    uint16_t sa_port;
    uint32_t sa_addr;
    uint8_t  sa_zero[8];
} ax_sockaddr_in_t;

typedef struct {
    uint16_t sa_family;
    char     sa_path[AX_UN_PATH_MAX];
} ax_sockaddr_un_t;

typedef struct {
    uint16_t sa_family;
    char     sa_data[14];
} ax_sockaddr_t;

#define AX_HTONS(x) ((uint16_t)((((uint16_t)(x) & 0x00FFu) << 8u) | (((uint16_t)(x) & 0xFF00u) >> 8u)))
#define AX_HTONL(x) ((uint32_t)((((uint32_t)(x) & 0x000000FFu) << 24u) | \
                                (((uint32_t)(x) & 0x0000FF00u) << 8u)  | \
                                (((uint32_t)(x) & 0x00FF0000u) >> 8u)  | \
                                (((uint32_t)(x) & 0xFF000000u) >> 24u)))
#define AX_NTOHS AX_HTONS
#define AX_NTOHL AX_HTONL

#ifdef __cplusplus
extern "C" {
#endif

static inline long ax_syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a1;
    register long rsi __asm__("rsi") = a2;
    register long rdx __asm__("rdx") = a3;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("int $0x80"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
                     : "memory", "rcx", "r11");
    return rax;
}

#define ax_syscall0(n)            ax_syscall6((n), 0, 0, 0, 0, 0, 0)
#define ax_syscall1(n,a)          ax_syscall6((n), (long)(a), 0, 0, 0, 0, 0)
#define ax_syscall2(n,a,b)        ax_syscall6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define ax_syscall3(n,a,b,c)      ax_syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define ax_syscall4(n,a,b,c,d)    ax_syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define ax_syscall5(n,a,b,c,d,e)  ax_syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

int     socket(int domain, int type, int protocol);
int     bind(int fd, const ax_sockaddr_t *addr, axsock_socklen_t len);
int     connect(int fd, const ax_sockaddr_t *addr, axsock_socklen_t len);
int     listen(int fd, int backlog);
int     accept(int fd, ax_sockaddr_t *addr, axsock_socklen_t *len);
int     sendto(int fd, const void *buf, size_t len, int flags,
               const ax_sockaddr_t *addr, axsock_socklen_t alen);
int     recvfrom(int fd, void *buf, size_t len, int flags,
                 ax_sockaddr_t *addr, axsock_socklen_t *alen);
int     send(int fd, const void *buf, size_t len, int flags);
int     recv(int fd, void *buf, size_t len, int flags);
int     shutdown(int fd, int how);
int     getsockname(int fd, ax_sockaddr_t *addr, axsock_socklen_t *len);
int     getpeername(int fd, ax_sockaddr_t *addr, axsock_socklen_t *len);
int     setsockopt(int fd, int level, int optname, const void *optval, axsock_socklen_t optlen);
int     getsockopt(int fd, int level, int optname, void *optval, axsock_socklen_t *optlen);
int     socketpair(int domain, int type, int protocol, int sv[2]);

#ifdef __cplusplus
}
#endif

#endif
