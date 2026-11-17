#include "ixn_socket_api.h"
#include "ixn_netdev.h"
#include "ixn_compat.h"

#define IXN_API_MAX_FDS 128u

typedef struct {
    ixn_socket_t *sock;
    ixn_u8 in_use;
    ixn_u8 nonblock;
    ixn_u32 rcvbuf;
    ixn_u32 sndbuf;
} ixn_api_fd_t;

static ixn_api_fd_t g_ixn_fds[IXN_API_MAX_FDS];
static volatile ixn_irq_lock_t g_ixn_api_lock = 0u;

void ixn_socket_api_init(void) {
    ixn_memset(g_ixn_fds, 0, sizeof(g_ixn_fds));
}

static ixn_fd_t ixn_api_fd_alloc(ixn_socket_t *s) {
    ixn_u64 flags;
    ixn_u32 i;
    flags = ixn_irq_lock_acquire(&g_ixn_api_lock);
    for (i = 0u; i < IXN_API_MAX_FDS; i++) {
        if (!g_ixn_fds[i].in_use) {
            g_ixn_fds[i].in_use = 1u;
            g_ixn_fds[i].sock = s;
            g_ixn_fds[i].nonblock = 0u;
            g_ixn_fds[i].rcvbuf = 65536u;
            g_ixn_fds[i].sndbuf = 65536u;
            ixn_irq_lock_release(&g_ixn_api_lock, flags);
            return (ixn_fd_t)(i + 1);
        }
    }
    ixn_irq_lock_release(&g_ixn_api_lock, flags);
    return IXN_E_NOMEM;
}

static ixn_api_fd_t *ixn_api_fd_get(ixn_fd_t fd) {
    ixn_u32 i = (ixn_u32)fd - 1u;
    if (fd <= 0 || i >= IXN_API_MAX_FDS) return IXN_NULL;
    if (!g_ixn_fds[i].in_use) return IXN_NULL;
    return &g_ixn_fds[i];
}

ixn_socket_t *ixn_api_get_socket(ixn_fd_t fd) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    return f ? f->sock : IXN_NULL;
}

ixn_fd_t ixn_api_socket(ixn_u32 domain, ixn_u32 type, ixn_u32 protocol) {
    ixn_socket_t *s;
    ixn_sock_type_t st;
    (void)domain;
    (void)protocol;
    if (type == IXN_SOCK_STREAM) st = IXN_SOCK_TYPE_TCP;
    else if (type == IXN_SOCK_DGRAM) st = IXN_SOCK_TYPE_UDP;
    else if (type == IXN_SOCK_RAW) st = IXN_SOCK_TYPE_RAW;
    else return IXN_E_INVAL;
    s = ixn_socket_alloc(st);
    if (!s) return IXN_E_NOMEM;
    return ixn_api_fd_alloc(s);
}

int ixn_api_bind(ixn_fd_t fd, const ixn_sockaddr_in_t *addr) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    if (!f || !f->sock || !addr) return IXN_E_BADF;
    if (addr->family != IXN_AF_INET) return IXN_E_INVAL;
    return ixn_socket_bind(f->sock, addr->addr, IXN_ntohs(addr->port));
}

int ixn_api_connect(ixn_fd_t fd, const ixn_sockaddr_in_t *addr) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    if (!f || !f->sock || !addr) return IXN_E_BADF;
    if (f->sock->type == IXN_SOCK_TYPE_TCP) {
        return ixn_tcp_connect(f->sock, addr->addr, IXN_ntohs(addr->port));
    }
    if (f->sock->type == IXN_SOCK_TYPE_UDP) {
        f->sock->remote_ip = addr->addr;
        f->sock->remote_port = IXN_ntohs(addr->port);
        return 0;
    }
    return IXN_E_INVAL;
}

int ixn_api_listen(ixn_fd_t fd, ixn_u32 backlog) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    if (!f || !f->sock) return IXN_E_BADF;
    if (f->sock->type != IXN_SOCK_TYPE_TCP) return IXN_E_INVAL;
    return ixn_tcp_listen(f->sock, (ixn_u8)(backlog & 0xFFu));
}

ixn_fd_t ixn_api_accept(ixn_fd_t fd, ixn_sockaddr_in_t *out_addr) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    ixn_socket_t *child;
    if (!f || !f->sock) return IXN_E_BADF;
    child = ixn_tcp_accept(f->sock);
    if (!child) return f->nonblock ? IXN_E_AGAIN : IXN_E_AGAIN;
    if (out_addr) {
        out_addr->family = IXN_AF_INET;
        out_addr->port = IXN_htons(child->remote_port);
        out_addr->addr = child->remote_ip;
    }
    return ixn_api_fd_alloc(child);
}

int ixn_api_send(ixn_fd_t fd, const ixn_u8 *buf, ixn_u32 len, ixn_u32 flags) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    (void)flags;
    if (!f || !f->sock || !buf) return IXN_E_BADF;
    if (f->sock->type == IXN_SOCK_TYPE_TCP) {
        return ixn_tcp_send(f->sock, buf, len);
    }
    if (f->sock->type == IXN_SOCK_TYPE_UDP) {
        return ixn_udp_socket_send(f->sock, f->sock->remote_ip, f->sock->remote_port, buf, len);
    }
    return IXN_E_INVAL;
}

int ixn_api_recv(ixn_fd_t fd, ixn_u8 *buf, ixn_u32 cap, ixn_u32 flags) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    (void)flags;
    if (!f || !f->sock || !buf) return IXN_E_BADF;
    if (f->sock->type == IXN_SOCK_TYPE_TCP) {
        return ixn_tcp_socket_recv(f->sock, buf, cap);
    }
    if (f->sock->type == IXN_SOCK_TYPE_UDP) {
        return ixn_udp_socket_recv(f->sock, IXN_NULL, IXN_NULL, buf, cap);
    }
    return IXN_E_INVAL;
}

int ixn_api_sendto(ixn_fd_t fd, const ixn_u8 *buf, ixn_u32 len, ixn_u32 flags, const ixn_sockaddr_in_t *dst) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    (void)flags;
    if (!f || !f->sock || !buf || !dst) return IXN_E_BADF;
    if (f->sock->type == IXN_SOCK_TYPE_UDP) {
        return ixn_udp_socket_send(f->sock, dst->addr, IXN_ntohs(dst->port), buf, len);
    }
    if (f->sock->type == IXN_SOCK_TYPE_TCP) {
        return ixn_tcp_send(f->sock, buf, len);
    }
    return IXN_E_INVAL;
}

int ixn_api_recvfrom(ixn_fd_t fd, ixn_u8 *buf, ixn_u32 cap, ixn_u32 flags, ixn_sockaddr_in_t *out_src) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    ixn_u32 src_ip;
    ixn_u16 src_port;
    int rc;
    (void)flags;
    if (!f || !f->sock || !buf) return IXN_E_BADF;
    if (f->sock->type == IXN_SOCK_TYPE_UDP) {
        rc = ixn_udp_socket_recv(f->sock, &src_ip, &src_port, buf, cap);
        if (rc >= 0 && out_src) {
            out_src->family = IXN_AF_INET;
            out_src->port = IXN_htons(src_port);
            out_src->addr = src_ip;
        }
        return rc;
    }
    if (f->sock->type == IXN_SOCK_TYPE_TCP) {
        return ixn_tcp_socket_recv(f->sock, buf, cap);
    }
    return IXN_E_INVAL;
}

int ixn_api_close(ixn_fd_t fd) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    ixn_u64 flags;
    if (!f) return IXN_E_BADF;
    if (f->sock) {
        if (f->sock->type == IXN_SOCK_TYPE_TCP) ixn_tcp_close(f->sock);
        ixn_socket_free(f->sock);
    }
    flags = ixn_irq_lock_acquire(&g_ixn_api_lock);
    f->in_use = 0u;
    f->sock = IXN_NULL;
    ixn_irq_lock_release(&g_ixn_api_lock, flags);
    return 0;
}

int ixn_api_shutdown(ixn_fd_t fd, ixn_u32 how) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    if (!f || !f->sock) return IXN_E_BADF;
    if (f->sock->type == IXN_SOCK_TYPE_TCP) {
        if (how == IXN_SHUT_WR || how == IXN_SHUT_RDWR) ixn_tcp_close(f->sock);
    }
    return 0;
}

int ixn_api_setsockopt(ixn_fd_t fd, ixn_u32 level, ixn_u32 optname, const ixn_u8 *optval, ixn_u32 optlen) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    (void)level;
    if (!f || !f->sock || !optval) return IXN_E_BADF;
    if (optname == IXN_SO_NONBLOCK) {
        f->nonblock = (optlen >= 1u && optval[0]) ? 1u : 0u;
        if (f->nonblock) f->sock->flags |= IXN_SOCK_F_NONBLOCK;
        else f->sock->flags &= ~IXN_SOCK_F_NONBLOCK;
        return 0;
    }
    if (optname == IXN_SO_REUSEADDR) {
        f->sock->flags |= IXN_SOCK_F_REUSE;
        return 0;
    }
    if (optname == IXN_SO_RCVBUF && optlen >= 4u) {
        f->rcvbuf = ixn_le32_load(optval);
        return 0;
    }
    if (optname == IXN_SO_SNDBUF && optlen >= 4u) {
        f->sndbuf = ixn_le32_load(optval);
        return 0;
    }
    return 0;
}

int ixn_api_getsockopt(ixn_fd_t fd, ixn_u32 level, ixn_u32 optname, ixn_u8 *optval, ixn_u32 *optlen) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    (void)level;
    if (!f || !f->sock || !optval || !optlen) return IXN_E_BADF;
    if (optname == IXN_SO_NONBLOCK) {
        if (*optlen < 1u) return IXN_E_INVAL;
        optval[0] = f->nonblock;
        *optlen = 1u;
        return 0;
    }
    if (optname == IXN_SO_ERROR) {
        if (*optlen < 4u) return IXN_E_INVAL;
        ixn_le32_store(optval, 0u);
        *optlen = 4u;
        return 0;
    }
    return 0;
}

int ixn_api_getpeername(ixn_fd_t fd, ixn_sockaddr_in_t *out) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    if (!f || !f->sock || !out) return IXN_E_BADF;
    out->family = IXN_AF_INET;
    out->port = IXN_htons(f->sock->remote_port);
    out->addr = f->sock->remote_ip;
    return 0;
}

int ixn_api_getsockname(ixn_fd_t fd, ixn_sockaddr_in_t *out) {
    ixn_api_fd_t *f = ixn_api_fd_get(fd);
    if (!f || !f->sock || !out) return IXN_E_BADF;
    out->family = IXN_AF_INET;
    out->port = IXN_htons(f->sock->local_port);
    out->addr = f->sock->local_ip;
    return 0;
}

void ixn_socket_api_poll(void) {
    ixn_netdev_poll_all(IXN_NETDEV_RX_BUDGET);
}

