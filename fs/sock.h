#ifndef AX_SOCK_H
#define AX_SOCK_H

#include <stdint.h>

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

struct fs_node;
struct ax_socket;

void ax_sock_init(void);

int ax_sock_create(uint32_t domain, uint32_t type, uint32_t protocol);
int ax_sock_bind(int fd, const ax_sockaddr_t *addr, uint32_t len);
int ax_sock_connect(int fd, const ax_sockaddr_t *addr, uint32_t len);
int ax_sock_listen(int fd, uint32_t backlog);
int ax_sock_accept(int fd, ax_sockaddr_t *addr, uint32_t *len);
int ax_sock_sendto(int fd, const void *buf, uint32_t len, uint32_t flags,
                   const ax_sockaddr_t *addr, uint32_t alen);
int ax_sock_recvfrom(int fd, void *buf, uint32_t len, uint32_t flags,
                     ax_sockaddr_t *addr, uint32_t *alen);
int ax_sock_shutdown(int fd, uint32_t how);
int ax_sock_getsockname(int fd, ax_sockaddr_t *addr, uint32_t *len);
int ax_sock_getpeername(int fd, ax_sockaddr_t *addr, uint32_t *len);
int ax_sock_setsockopt(int fd, uint32_t level, uint32_t optname,
                       const void *optval, uint32_t optlen);
int ax_sock_getsockopt(int fd, uint32_t level, uint32_t optname,
                       void *optval, uint32_t *optlen);
int ax_sock_pair(uint32_t domain, uint32_t type, uint32_t protocol, int sv[2]);

void ax_sock_close(int fd);
int  ax_sock_is_socket(int fd);
void ax_sock_poll(void);

#endif
