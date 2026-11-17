#ifndef IXN_SOCKET_API_H
#define IXN_SOCKET_API_H

#include "ixn_types.h"
#include "ixn_l4.h"

#define IXN_AF_INET   2u
#define IXN_AF_INET6  10u
#define IXN_AF_UNSPEC 0u

#define IXN_SOCK_STREAM 1u
#define IXN_SOCK_DGRAM  2u
#define IXN_SOCK_RAW    3u

#define IXN_SOL_SOCKET    0xFFFFu
#define IXN_SOL_IP        0u
#define IXN_SOL_TCP       6u
#define IXN_SOL_UDP       17u

#define IXN_SO_REUSEADDR  0x0004u
#define IXN_SO_KEEPALIVE  0x0008u
#define IXN_SO_BROADCAST  0x0020u
#define IXN_SO_LINGER     0x0080u
#define IXN_SO_RCVBUF     0x1002u
#define IXN_SO_SNDBUF     0x1001u
#define IXN_SO_NONBLOCK   0x4000u
#define IXN_SO_ERROR      0x1007u

#define IXN_MSG_OOB       0x0001u
#define IXN_MSG_PEEK      0x0002u
#define IXN_MSG_WAITALL   0x0100u
#define IXN_MSG_DONTWAIT  0x0040u

#define IXN_SHUT_RD     0u
#define IXN_SHUT_WR     1u
#define IXN_SHUT_RDWR   2u

#define IXN_E_OK         0
#define IXN_E_BADF      -9
#define IXN_E_AGAIN     -11
#define IXN_E_NOMEM     -12
#define IXN_E_ACCES     -13
#define IXN_E_FAULT     -14
#define IXN_E_BUSY      -16
#define IXN_E_EXIST     -17
#define IXN_E_INVAL     -22
#define IXN_E_PIPE      -32
#define IXN_E_CONNREF   -111
#define IXN_E_CONNRESET -104
#define IXN_E_TIMEOUT   -110
#define IXN_E_NETUNREACH -101

typedef struct {
    ixn_u16 family;
    ixn_u16 port;
    ixn_u32 addr;
    ixn_u8 zero[8];
} ixn_sockaddr_in_t;

typedef int ixn_fd_t;

void ixn_socket_api_init(void);
ixn_socket_t *ixn_api_get_socket(ixn_fd_t fd);

ixn_fd_t ixn_api_socket(ixn_u32 domain, ixn_u32 type, ixn_u32 protocol);
int ixn_api_bind(ixn_fd_t fd, const ixn_sockaddr_in_t *addr);
int ixn_api_connect(ixn_fd_t fd, const ixn_sockaddr_in_t *addr);
int ixn_api_listen(ixn_fd_t fd, ixn_u32 backlog);
ixn_fd_t ixn_api_accept(ixn_fd_t fd, ixn_sockaddr_in_t *out_addr);
int ixn_api_send(ixn_fd_t fd, const ixn_u8 *buf, ixn_u32 len, ixn_u32 flags);
int ixn_api_recv(ixn_fd_t fd, ixn_u8 *buf, ixn_u32 cap, ixn_u32 flags);
int ixn_api_sendto(ixn_fd_t fd, const ixn_u8 *buf, ixn_u32 len, ixn_u32 flags, const ixn_sockaddr_in_t *dst);
int ixn_api_recvfrom(ixn_fd_t fd, ixn_u8 *buf, ixn_u32 cap, ixn_u32 flags, ixn_sockaddr_in_t *out_src);
int ixn_api_close(ixn_fd_t fd);
int ixn_api_shutdown(ixn_fd_t fd, ixn_u32 how);
int ixn_api_setsockopt(ixn_fd_t fd, ixn_u32 level, ixn_u32 optname, const ixn_u8 *optval, ixn_u32 optlen);
int ixn_api_getsockopt(ixn_fd_t fd, ixn_u32 level, ixn_u32 optname, ixn_u8 *optval, ixn_u32 *optlen);
int ixn_api_getpeername(ixn_fd_t fd, ixn_sockaddr_in_t *out);
int ixn_api_getsockname(ixn_fd_t fd, ixn_sockaddr_in_t *out);

void ixn_socket_api_poll(void);

#endif
