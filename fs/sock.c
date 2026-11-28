#include "sock.h"
#include "../drivers/net/ixn_socket_api.h"
#include "../drivers/net/ixn_compat.h"
#include "../drivers/net/ixn_net.h"
#include "../drivers/net/ixn_l4.h"
#include "../multitask/task.h"
#include "../mm/kheap.h"
#include "../lib/common.h"

#include <stdint.h>
#include <stddef.h>

#define AX_SOCK_UN_CONNECT_MAX 8u

typedef struct ax_socket {
    uint32_t kind;
    uint16_t family;

    int      net_fd;
    ixn_socket_t *net;

    int      un_peer;
    uint32_t un_id;

    uint8_t  nonblock;
    uint8_t  listening;
    uint8_t  bound;
    uint8_t  connected;
    uint8_t  in_use;
    uint8_t  shut_rd;
    uint8_t  shut_wr;

    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_ip;
    uint32_t remote_ip;
    ixn_u8   remote_mac[IXN_ETH_ALEN];
    uint8_t  has_mac;

    uint16_t un_path_len;
    char     un_path[AX_UN_PATH_MAX];

    struct ax_socket *un_accept_queue[AX_SOCK_UN_CONNECT_MAX];
    uint32_t un_accept_count;
} ax_socket_t;

#define AX_KIND_NONE 0u
#define AX_KIND_INET 1u
#define AX_KIND_UNIX 2u

static ax_socket_t  g_axsock_pool[TASK_MAX_FDS * 2];
static uint32_t     g_axsock_next_id = 1u;
static uint32_t     g_axsock_ephemeral = 49152u;

static ax_socket_t *axsock_of(int fd) {
    Task *t = runningTask;

    if (!t || fd < 0 || fd >= TASK_MAX_FDS)
        return 0;

    if (!t->socks[fd])
        return 0;

    if (t->files[fd])
        return 0;

    return t->socks[fd];
}

static ax_socket_t *axsock_new(uint32_t kind, uint16_t family) {
    uint32_t i;

    for (i = 0u; i < sizeof(g_axsock_pool) / sizeof(g_axsock_pool[0]); i++) {
        ax_socket_t *s = &g_axsock_pool[i];
        if (!s->in_use) {
            memset((u8int *)s, 0, (u32int)sizeof(*s));
            s->in_use = 1u;
            s->kind = kind;
            s->family = family;
            s->net_fd = -1;
            s->un_peer = -1;
            s->un_id = g_axsock_next_id++;
            if (!s->un_id) s->un_id = g_axsock_next_id++;
            return s;
        }
    }

    return 0;
}

static void axsock_destroy(ax_socket_t *s) {
    if (!s)
        return;

    if (s->kind == AX_KIND_INET && s->net_fd >= 0) {
        ixn_api_close(s->net_fd);
        s->net_fd = -1;
    }

    if (s->kind == AX_KIND_UNIX && s->un_peer >= 0) {
        uint32_t i;
        for (i = 0u; i < sizeof(g_axsock_pool) / sizeof(g_axsock_pool[0]); i++) {
            ax_socket_t *p = &g_axsock_pool[i];
            if (p == s || !p->in_use || p->kind != AX_KIND_UNIX) continue;
            if (p->un_id == (uint32_t)s->un_peer) {
                p->un_peer = -1;
                p->connected = 0u;
                break;
            }
        }
        s->un_peer = -1;
    }

    s->in_use = 0u;
    s->kind = AX_KIND_NONE;
}

static int axsock_install(ax_socket_t *s) {
    Task *t = runningTask;
    int fd;

    if (!t || !s)
        return -1;

    fd = task_alloc_fd(t);
    if (fd < 0) {
        axsock_destroy(s);
        return -1;
    }

    t->socks[fd] = s;
    return fd;
}

static int axsock_find_by_id(uint32_t id) {
    uint32_t i;

    for (i = 0u; i < sizeof(g_axsock_pool) / sizeof(g_axsock_pool[0]); i++) {
        if (g_axsock_pool[i].in_use && g_axsock_pool[i].un_id == id)
            return (int)i;
    }

    return -1;
}

static int axsock_find_path(const char *path, uint32_t len) {
    uint32_t i;

    for (i = 0u; i < sizeof(g_axsock_pool) / sizeof(g_axsock_pool[0]); i++) {
        ax_socket_t *s = &g_axsock_pool[i];
        if (!s->in_use || s->kind != AX_KIND_UNIX || !s->bound) continue;
        if (s->un_path_len != len) continue;
        if (ixn_memcmp(s->un_path, path, len) == 0) return (int)i;
    }

    return -1;
}

void ax_sock_init(void) {
    ixn_memset(g_axsock_pool, 0, sizeof(g_axsock_pool));
    g_axsock_next_id = 1u;
    g_axsock_ephemeral = 49152u;
}

int ax_sock_create(uint32_t domain, uint32_t type, uint32_t protocol) {
    ax_socket_t *s;

    (void)protocol;

    if (domain == AX_AF_INET) {
        ixn_fd_t nfd;

        if (type != AX_SOCK_STREAM && type != AX_SOCK_DGRAM && type != AX_SOCK_RAW)
            return -1;

        nfd = ixn_api_socket(AX_AF_INET, type, protocol);
        if (nfd < 0)
            return -1;

        s = axsock_new(AX_KIND_INET, AX_AF_INET);
        if (!s) {
            ixn_api_close(nfd);
            return -1;
        }

        s->net_fd = (int)nfd;
        s->net = ixn_api_get_socket(nfd);
        return axsock_install(s);
    }

    if (domain == AX_AF_UNIX) {
        if (type != AX_SOCK_STREAM)
            return -1;

        s = axsock_new(AX_KIND_UNIX, AX_AF_UNIX);
        if (!s)
            return -1;

        return axsock_install(s);
    }

    return -1;
}

static void axsock_fill_addr(const ax_socket_t *s, ax_sockaddr_t *out, uint32_t *len) {
    if (!out || !len)
        return;

    if (s->family == AX_AF_UNIX) {
        ax_sockaddr_un_t *u = (ax_sockaddr_un_t *)out;
        uint32_t need = (uint32_t)offsetof(ax_sockaddr_un_t, sa_path) + s->un_path_len + 1u;

        if (*len < need)
            need = *len;

        ixn_memset(out, 0, *len);
        u->sa_family = AX_AF_UNIX;
        for (uint32_t i = 0u; i < s->un_path_len && i + 1u < AX_UN_PATH_MAX; i++)
            u->sa_path[i] = s->un_path[i];

        *len = need;
        return;
    }

    if (*len >= sizeof(ax_sockaddr_in_t)) {
        ax_sockaddr_in_t *in = (ax_sockaddr_in_t *)out;
        ixn_memset(out, 0, sizeof(ax_sockaddr_in_t));
        in->sa_family = AX_AF_INET;
        in->sa_port = IXN_htons(s->local_port);
        in->sa_addr = s->local_ip;
        *len = sizeof(ax_sockaddr_in_t);
    }
}

int ax_sock_bind(int fd, const ax_sockaddr_t *addr, uint32_t len) {
    ax_socket_t *s = axsock_of(fd);

    if (!s || !addr)
        return -1;

    if (addr->sa_family == AX_AF_UNIX) {
        const ax_sockaddr_un_t *u = (const ax_sockaddr_un_t *)addr;
        uint32_t base = (uint32_t)offsetof(ax_sockaddr_un_t, sa_path);
        uint32_t plen;

        if (len <= base)
            return -1;

        plen = len - base;
        if (plen > AX_UN_PATH_MAX)
            plen = AX_UN_PATH_MAX;

        if (axsock_find_path(u->sa_path, plen) >= 0)
            return -1;

        for (uint32_t i = 0u; i < plen; i++)
            s->un_path[i] = u->sa_path[i];
        s->un_path_len = (uint16_t)plen;
        s->bound = 1u;
        return 0;
    }

    if (addr->sa_family == AX_AF_INET && s->kind == AX_KIND_INET) {
        const ax_sockaddr_in_t *in = (const ax_sockaddr_in_t *)addr;
        if (ixn_api_bind(s->net_fd, in) != 0)
            return -1;
        s->local_ip = in->sa_addr;
        s->local_port = IXN_ntohs(in->sa_port);
        s->bound = 1u;
        return 0;
    }

    return -1;
}

int ax_sock_connect(int fd, const ax_sockaddr_t *addr, uint32_t len) {
    ax_socket_t *s = axsock_of(fd);

    if (!s || !addr)
        return -1;

    if (addr->sa_family == AX_AF_UNIX) {
        const ax_sockaddr_un_t *u = (const ax_sockaddr_un_t *)addr;
        uint32_t base = (uint32_t)offsetof(ax_sockaddr_un_t, sa_path);
        uint32_t plen;
        int idx;
        ax_socket_t *srv;

        if (len <= base)
            return -1;

        plen = len - base;
        if (plen > AX_UN_PATH_MAX)
            plen = AX_UN_PATH_MAX;

        idx = axsock_find_path(u->sa_path, plen);
        if (idx < 0)
            return -1;

        srv = &g_axsock_pool[idx];
        if (!srv->listening)
            return -1;

        s->un_peer = (int)srv->un_id;
        s->connected = 1u;

        if (srv->un_accept_count < AX_SOCK_UN_CONNECT_MAX)
            srv->un_accept_queue[srv->un_accept_count++] = s;

        return 0;
    }

    if (addr->sa_family == AX_AF_INET && s->kind == AX_KIND_INET) {
        const ax_sockaddr_in_t *in = (const ax_sockaddr_in_t *)addr;
        int rc = ixn_api_connect(s->net_fd, in);

        if (rc != 0)
            return -1;

        s->remote_ip = in->sa_addr;
        s->remote_port = IXN_ntohs(in->sa_port);
        s->connected = 1u;
        return 0;
    }

    return -1;
}

int ax_sock_listen(int fd, uint32_t backlog) {
    ax_socket_t *s = axsock_of(fd);

    if (!s)
        return -1;

    s->listening = 1u;

    if (s->kind == AX_KIND_INET) {
        if (!s->bound) {
            ax_sockaddr_in_t in;
            ixn_memset(&in, 0, sizeof(in));
            in.sa_family = AX_AF_INET;
            in.sa_port = IXN_htons(g_axsock_ephemeral++);
            in.sa_addr = 0u;
            if (ixn_api_bind(s->net_fd, &in) != 0)
                return -1;
            s->local_port = IXN_ntohs(in.sa_port);
            s->bound = 1u;
        }

        if (s->net)
            ixn_tcp_listen(s->net, (uint8_t)(backlog > 255u ? 255u : backlog));
    }

    return 0;
}

int ax_sock_accept(int fd, ax_sockaddr_t *addr, uint32_t *len) {
    ax_socket_t *s = axsock_of(fd);

    if (!s || !s->listening)
        return -1;

    if (s->kind == AX_KIND_UNIX) {
        ax_socket_t *peer;
        int nfd;

        if (s->un_accept_count == 0u)
            return -1;

        peer = s->un_accept_queue[0];
        for (uint32_t i = 1u; i < s->un_accept_count; i++)
            s->un_accept_queue[i - 1u] = s->un_accept_queue[i];
        s->un_accept_count--;

        if (!peer || !peer->in_use)
            return -1;

        peer->un_peer = (int)s->un_id;
        nfd = axsock_install(peer);

        if (nfd < 0)
            return -1;

        if (addr && len)
            axsock_fill_addr(s, addr, len);

        return nfd;
    }

    if (s->kind == AX_KIND_INET && s->net) {
        ixn_socket_t *child = ixn_tcp_accept(s->net);
        ax_socket_t *c;
        int nfd;

        if (!child)
            return -1;

        c = axsock_new(AX_KIND_INET, AX_AF_INET);
        if (!c) {
            ixn_socket_free(child);
            return -1;
        }

        c->net_fd = -1;
        c->net = child;
        c->connected = 1u;
        c->remote_ip = child->remote_ip;
        c->remote_port = child->remote_port;
        c->local_ip = child->local_ip;
        c->local_port = child->local_port;

        child->parent = 0;

        nfd = axsock_install(c);
        if (nfd < 0)
            return -1;

        if (addr && len) {
            ax_sockaddr_in_t in;
            ixn_memset(&in, 0, sizeof(in));
            in.sa_family = AX_AF_INET;
            in.sa_port = IXN_htons(c->remote_port);
            in.sa_addr = c->remote_ip;
            if (*len >= sizeof(in)) {
                ixn_memcpy(addr, &in, sizeof(in));
                *len = sizeof(in);
            }
        }

        return nfd;
    }

    return -1;
}

int ax_sock_sendto(int fd, const void *buf, uint32_t len, uint32_t flags,
                   const ax_sockaddr_t *addr, uint32_t alen) {
    ax_socket_t *s = axsock_of(fd);

    (void)alen;

    if (!s || !buf)
        return -1;

    if (s->shut_wr)
        return -1;

    if (s->kind == AX_KIND_UNIX) {
        int idx;
        ax_socket_t *peer;

        if (s->un_peer < 0)
            return -1;

        idx = axsock_find_by_id((uint32_t)s->un_peer);
        if (idx < 0)
            return -1;

        peer = &g_axsock_pool[idx];
        if (peer->shut_rd)
            return -1;

        (void)peer;
        return (int)len;
    }

    if (s->kind == AX_KIND_INET) {
        if (addr && addr->sa_family == AX_AF_INET &&
            (s->net == 0 || s->net->type == IXN_SOCK_TYPE_UDP ||
             s->net->type == IXN_SOCK_TYPE_RAW)) {
            const ax_sockaddr_in_t *in = (const ax_sockaddr_in_t *)addr;
            return ixn_api_sendto(s->net_fd, (const ixn_u8 *)buf, len, flags, in);
        }

        return ixn_api_send(s->net_fd, (const ixn_u8 *)buf, len, flags);
    }

    return -1;
}

int ax_sock_recvfrom(int fd, void *buf, uint32_t len, uint32_t flags,
                     ax_sockaddr_t *addr, uint32_t *alen) {
    ax_socket_t *s = axsock_of(fd);

    if (!s || !buf)
        return -1;

    if (s->shut_rd)
        return 0;

    if (s->kind == AX_KIND_UNIX) {
        if (s->un_peer < 0)
            return -1;

        if (alen) *alen = 0u;
        return 0;
    }

    if (s->kind == AX_KIND_INET) {
        if (addr && alen && s->net && s->net->type == IXN_SOCK_TYPE_UDP) {
            ixn_sockaddr_in_t tmp;
            int rc = ixn_api_recvfrom(s->net_fd, (ixn_u8 *)buf, len, flags, &tmp);
            if (rc >= 0) {
                ixn_memcpy(addr, &tmp, sizeof(tmp));
                *alen = sizeof(tmp);
            }
            return rc;
        }

        return ixn_api_recv(s->net_fd, (ixn_u8 *)buf, len, flags);
    }

    return -1;
}

int ax_sock_shutdown(int fd, uint32_t how) {
    ax_socket_t *s = axsock_of(fd);

    if (!s)
        return -1;

    if (how == AX_SHUT_RD || how == AX_SHUT_RDWR) s->shut_rd = 1u;
    if (how == AX_SHUT_WR || how == AX_SHUT_RDWR) s->shut_wr = 1u;

    if (s->kind == AX_KIND_INET)
        return ixn_api_shutdown(s->net_fd, how);

    return 0;
}

int ax_sock_getsockname(int fd, ax_sockaddr_t *addr, uint32_t *len) {
    ax_socket_t *s = axsock_of(fd);

    if (!s || !addr || !len)
        return -1;

    axsock_fill_addr(s, addr, len);
    return 0;
}

int ax_sock_getpeername(int fd, ax_sockaddr_t *addr, uint32_t *len) {
    ax_socket_t *s = axsock_of(fd);

    if (!s || !addr || !len)
        return -1;

    if (s->kind == AX_KIND_INET && *len >= sizeof(ax_sockaddr_in_t)) {
        ax_sockaddr_in_t *in = (ax_sockaddr_in_t *)addr;
        ixn_memset(addr, 0, sizeof(ax_sockaddr_in_t));
        in->sa_family = AX_AF_INET;
        in->sa_port = IXN_htons(s->remote_port);
        in->sa_addr = s->remote_ip;
        *len = sizeof(ax_sockaddr_in_t);
        return 0;
    }

    return -1;
}

int ax_sock_setsockopt(int fd, uint32_t level, uint32_t optname,
                       const void *optval, uint32_t optlen) {
    ax_socket_t *s = axsock_of(fd);

    if (!s)
        return -1;

    if (optname == AX_SO_NONBLOCK) {
        uint8_t on = (optlen >= 1u && optval) ? *(const uint8_t *)optval : 0u;
        s->nonblock = on ? 1u : 0u;

        if (s->kind == AX_KIND_INET)
            return ixn_api_setsockopt(s->net_fd, level, AX_SO_NONBLOCK, &on, 1u);
        return 0;
    }

    if (s->kind == AX_KIND_INET)
        return ixn_api_setsockopt(s->net_fd, level, optname,
                                  (const ixn_u8 *)optval, optlen);

    return 0;
}

int ax_sock_getsockopt(int fd, uint32_t level, uint32_t optname,
                       void *optval, uint32_t *optlen) {
    ax_socket_t *s = axsock_of(fd);

    if (!s || !optval || !optlen)
        return -1;

    if (optname == AX_SO_NONBLOCK) {
        if (*optlen < 1u)
            return -1;
        *(uint8_t *)optval = s->nonblock;
        *optlen = 1u;
        return 0;
    }

    if (s->kind == AX_KIND_INET)
        return ixn_api_getsockopt(s->net_fd, level, optname, (ixn_u8 *)optval, optlen);

    return 0;
}

int ax_sock_pair(uint32_t domain, uint32_t type, uint32_t protocol, int sv[2]) {
    ax_socket_t *a, *b;
    int fa, fb;

    (void)protocol;

    if (!sv || domain != AX_AF_UNIX || type != AX_SOCK_STREAM)
        return -1;

    a = axsock_new(AX_KIND_UNIX, AX_AF_UNIX);
    if (!a)
        return -1;

    b = axsock_new(AX_KIND_UNIX, AX_AF_UNIX);
    if (!b) {
        axsock_destroy(a);
        return -1;
    }

    a->connected = 1u;
    b->connected = 1u;
    a->un_peer = (int)b->un_id;
    b->un_peer = (int)a->un_id;

    fa = axsock_install(a);
    if (fa < 0)
        return -1;

    fb = axsock_install(b);
    if (fb < 0) {
        Task *t = runningTask;
        if (t && fa < TASK_MAX_FDS && t->socks[fa] == a) {
            t->socks[fa] = 0;
            task_close_fd(t, fa);
        }
        axsock_destroy(a);
        return -1;
    }

    sv[0] = fa;
    sv[1] = fb;
    return 0;
}

void ax_sock_close(int fd) {
    Task *t = runningTask;

    if (!t || fd < 0 || fd >= TASK_MAX_FDS)
        return;

    if (!t->socks[fd])
        return;

    axsock_destroy(t->socks[fd]);
    t->socks[fd] = 0;
}

int ax_sock_is_socket(int fd) {
    return axsock_of(fd) != 0;
}

void ax_sock_poll(void) {
    ixn_socket_api_poll();
}
