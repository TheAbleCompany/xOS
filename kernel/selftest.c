/*
 * kernel/selftest.c -- boot-time self checks for the subsystems that
 * have no userspace trigger yet.
 *
 * Every check prints "[SELFTEST] <name>: PASS|FAIL" to COM1 so a build
 * that links but is broken in a subtle way shows up immediately in the
 * serial log instead of being discovered months later.
 */

#include "../lib/common.h"
#include "../fs/sock.h"
#include "../fs/fs.h"
#include "../fs/ix/ix_automount.h"
#include "../fs/initrd.h"

int open(const char *path, int flags, int mode);
int write(int fd, char *buf, int nbytes);
int read(int fd, char *buf, int nbytes);
int lseek(int fd, int offset, int whence);
int close(int fd);
int unlink(const char *path);
#include "../mm/kheap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../multitask/task.h"
#include "../drivers/net/ixn_net.h"
#include "../drivers/net/ixn_netdev.h"
#include "../drivers/net/ixn_l2l3.h"
#include "../drivers/net/ixn_socket_api.h"
#include "../drivers/net/ixn_compat.h"
#include "isr.h"

#include <stdint.h>
#include <stddef.h>

extern void serial_puts(const char *s);

static int st_pass;
static int st_fail;

static void st_ok(const char *name) {
    serial_puts("[SELFTEST] ");
    serial_puts(name);
    serial_puts(": PASS\n");
    st_pass++;
}

static void st_bad(const char *name) {
    serial_puts("[SELFTEST] ");
    serial_puts(name);
    serial_puts(": FAIL\n");
    st_fail++;
}



static void st_socketpair(void) {
    int sv[2];

    if (ax_sock_pair(AX_AF_UNIX, AX_SOCK_STREAM, 0, sv) != 0) {
        st_bad("socketpair");
        return;
    }

    if (!ax_sock_is_socket(sv[0]) || !ax_sock_is_socket(sv[1])) {
        ax_sock_close(sv[0]);
        ax_sock_close(sv[1]);
        st_bad("socketpair-fd");
        return;
    }

    st_ok("socketpair");

    if (ax_sock_sendto(sv[0], "hello", 5, 0, 0, 0) != 5) {
        st_bad("socketpair-send");
    } else {
        st_ok("socketpair-send");
    }

    ax_sock_close(sv[0]);
    ax_sock_close(sv[1]);

    if (ax_sock_is_socket(sv[0]) || ax_sock_is_socket(sv[1]))
        st_bad("socketpair-close");
    else
        st_ok("socketpair-close");
}

static void st_unix_path(void) {
    ax_sockaddr_un_t path;
    int srv = -1;
    int cli = -1;
    int acc = -1;
    uint32_t len;
    int i;

    for (i = 0; i < AX_UN_PATH_MAX; i++) path.sa_path[i] = 0;
    path.sa_family = AX_AF_UNIX;

    {
        const char *p = "/tmp/selftest.sock";
        for (i = 0; p[i]; i++) path.sa_path[i] = p[i];
        len = (uint32_t)offsetof(ax_sockaddr_un_t, sa_path) + (uint32_t)i + 1u;
    }

    srv = ax_sock_create(AX_AF_UNIX, AX_SOCK_STREAM, 0);
    if (srv < 0) { st_bad("unix-create"); return; }

    if (ax_sock_bind(srv, (const ax_sockaddr_t *)&path, len) != 0) {
        st_bad("unix-bind");
        ax_sock_close(srv);
        return;
    }
    st_ok("unix-bind");

    if (ax_sock_listen(srv, 4) != 0) {
        st_bad("unix-listen");
        ax_sock_close(srv);
        return;
    }
    st_ok("unix-listen");

    cli = ax_sock_create(AX_AF_UNIX, AX_SOCK_STREAM, 0);
    if (cli < 0) { st_bad("unix-client"); ax_sock_close(srv); return; }

    if (ax_sock_connect(cli, (const ax_sockaddr_t *)&path, len) != 0) {
        st_bad("unix-connect");
        ax_sock_close(cli);
        ax_sock_close(srv);
        return;
    }
    st_ok("unix-connect");

    acc = ax_sock_accept(srv, 0, 0);
    if (acc < 0) {
        st_bad("unix-accept");
        ax_sock_close(cli);
        ax_sock_close(srv);
        return;
    }
    st_ok("unix-accept");

    ax_sock_close(acc);
    ax_sock_close(cli);
    ax_sock_close(srv);

    if (ax_sock_create(AX_AF_UNIX, AX_SOCK_DGRAM, 0) >= 0)
        st_bad("unix-reject-dgram");
    else
        st_ok("unix-reject-dgram");
}

static void st_heap(void) {
    void *a = kmalloc(1024);
    void *b = kmalloc(4096);
    void *c;
    size_t i;

    if (!a || !b) { st_bad("kheap-alloc"); return; }

    for (i = 0; i < 1024; i++) ((u8int *)a)[i] = (u8int)(i & 0xFF);
    for (i = 0; i < 4096; i++) ((u8int *)b)[i] = (u8int)((i * 7u) & 0xFF);

    for (i = 0; i < 1024; i++) {
        if (((u8int *)a)[i] != (u8int)(i & 0xFF)) { st_bad("kheap-write-a"); return; }
    }
    for (i = 0; i < 4096; i++) {
        if (((u8int *)b)[i] != (u8int)((i * 7u) & 0xFF)) { st_bad("kheap-write-b"); return; }
    }
    st_ok("kheap-rw");

    kfree(a);

    c = kcalloc(64, 16);
    if (!c) { st_bad("kheap-calloc"); return; }
    for (i = 0; i < 1024; i++) {
        if (((u8int *)c)[i] != 0u) { st_bad("kheap-calloc-zero"); kfree(c); return; }
    }
    st_ok("kheap-calloc-zero");

    kfree(c);
    kfree(b);
}

static void st_heap_reuse(void) {
    void *first = kmalloc(256);
    void *again;

    if (!first) { st_bad("kheap-reuse-alloc"); return; }

    ((u8int *)first)[0] = 0xABu;
    ((u8int *)first)[255] = 0xCDu;

    kfree(first);
    again = kmalloc(256);

    if (!again) { st_bad("kheap-reuse-alloc2"); return; }

    if (((u8int *)again)[0] == 0xABu && ((u8int *)again)[255] == 0xCDu)
        st_ok("kheap-reuse");
    else
        st_bad("kheap-reuse-garbage");

    kfree(again);
}

/*
 * ICMP echo через raw-сокет -- тот же путь, которым ходит userspace ping
 * (socket/sendto/recvfrom).  Возврат: 1 -- ответ с нашими id/seq, иначе 0.
 */
static int st_ping_raw(ixn_netdev_t *dev, ixn_u32 dst, ixn_u16 id, ixn_u16 seq,
                       ixn_u32 timeout_ms)
{
    ixn_u8 pkt[sizeof(ixn_icmp_hdr_t) + 14];
    ixn_u8 rx[128];
    ixn_icmp_hdr_t *icmp = (ixn_icmp_hdr_t *)pkt;
    ixn_sockaddr_in_t sa;
    ixn_fd_t fd;
    ixn_socket_t *s;
    ixn_u64 deadline;
    int rc = 0;

    ixn_memset(pkt, 0, sizeof(pkt));
    icmp->type = IXN_ICMP_ECHO_REQUEST;
    icmp->code = 0u;
    icmp->id = IXN_htons(id);
    icmp->seq = IXN_htons(seq);
    ixn_memcpy(pkt + sizeof(*icmp), "ArtyomX64 ping", 14);

    fd = ixn_api_socket(IXN_AF_INET, IXN_SOCK_RAW, IXN_IP_PROTO_ICMP);
    if (fd < 0) return -1;

    /* Проверяем конкретный интерфейс, а не default. */
    s = ixn_api_get_socket(fd);
    if (!s) { ixn_api_close(fd); return -1; }
    s->dev = dev;

    ixn_memset(&sa, 0, sizeof(sa));
    sa.family = IXN_AF_INET;
    sa.addr = dst;

    if (ixn_api_sendto(fd, pkt, sizeof(pkt), 0u, &sa) < 0) {
        ixn_api_close(fd);
        return -1;
    }

    deadline = ixn_ticks_ms() + timeout_ms;
    for (;;) {
        int n = ixn_api_recvfrom(fd, rx, sizeof(rx), 0u, IXN_NULL);

        if (n >= (int)sizeof(ixn_icmp_hdr_t)) {
            ixn_icmp_hdr_t *r = (ixn_icmp_hdr_t *)rx;

            if (r->type == IXN_ICMP_ECHO_REPLY &&
                IXN_ntohs(r->id) == id && IXN_ntohs(r->seq) == seq) {
                rc = 1;
                break;
            }
        }

        if (ixn_ticks_ms() >= deadline)
            break;
        ixn_pause();
    }

    ixn_api_close(fd);
    return rc;
}

static void st_net(void) {
    ixn_netdev_t *dev = ixn_netdev_get_default();
    ixn_arp_entry_t *e;
    ixn_u8 mac[IXN_ETH_ALEN];
    ixn_u64 deadline;
    ixn_u32 probe_ip = 0x0A000202u;

    if (!dev) {
        serial_puts("[SELFTEST] net-no-device: SKIP\n");
        return;
    }

    if (ixn_netdev_open(dev) != 0) {
        st_bad("net-open");
        return;
    }
    st_ok("net-open");

    ixn_netdev_set_ipv4(dev, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u);

    {
        int rc = ixn_arp_resolve(dev, probe_ip, mac);
        /* IXN_ARP_RESOLVE_SENT -- запрос ушёл в сеть, ответ ждём в цикле ниже */
        if (rc != IXN_ARP_RESOLVE_OK && rc != IXN_ARP_RESOLVE_SENT) {
            st_bad("net-arp-tx");
            return;
        }
    }
    st_ok("net-arp-tx");

    deadline = ixn_ticks_ms() + 1500u;
    while (ixn_ticks_ms() < deadline) {
        ixn_netdev_poll_all(64u);
        e = ixn_arp_lookup(probe_ip);
        if (e && e->state == IXN_ARP_STATE_RESOLVED) break;
        IXN_CPU_PAUSE();
    }

    e = ixn_arp_lookup(probe_ip);
    if (e && e->state == IXN_ARP_STATE_RESOLVED) {
        st_ok("net-arp-rx");
    } else
        st_bad("net-arp-rx");

    /*
     * ICMP end-to-end через raw-сокет -- тот же путь, которым ходит
     * userspace ping (socket/sendto/recvfrom): пакет собирается здесь,
     * стек считает checksum и шлёт, ответ попадает в rx-очередь сокета.
     * Считаем потери -- в серийный лог печатаем точное число ответов.
     * Контроль: пинг собственного адреса должен ходить через loopback
     * путь в ipv4_tx без участия сети.
     */
    {
        enum { PING_N = 50 };
        int t;
        for (t = 0; t < 2; t++) {
            ixn_u32 dst = t ? dev->ipv4_addr : probe_ip;
            const char *tag = t ? "net-icmp-self " : "net-icmp-gw ";
            char line[64];
            int ok = 0, i, pos;
            for (i = 1; i <= PING_N; i++) {
                if (st_ping_raw(dev, dst, 0x5049u, (ixn_u16)i, 1000u) > 0)
                    ok++;
            }
            {
                const char *suf = "/50 replies\n";
                for (pos = 0; tag[pos]; pos++) line[pos] = tag[pos];
                line[pos++] = (char)('0' + (ok / 10) % 10);
                line[pos++] = (char)('0' + ok % 10);
                for (i = 0; suf[i]; i++) line[pos++] = suf[i];
                line[pos] = 0;
                serial_puts(line);
            }
        }
        serial_puts("[SELFTEST] net-stats done\n");
    }

    /*
     * Loopback-контроль: ping 127.0.0.1 через lo тем же raw-путём.
     * Отдельно от net-icmp-self: тот пингует собственный адрес
     * физического интерфейса, а здесь нужен именно lo (иначе на машине
     * без NIC ping 127.0.0.1 ломался).
     */
    {
        ixn_netdev_t *lo = ixn_netdev_get_loopback();
        int got = 0;

        if (lo && st_ping_raw(lo, 0x7F000001u, 0x4C4Fu, 1u, 1000u) > 0)
            got = 1;

        serial_puts(got ? "net-icmp-loop 1/1 replies\n"
                        : "net-icmp-loop FAIL\n");
    }
}

static void st_disk_rw(void) {
    static const char probe[] = "artyomx64 disk write probe";
    char buf[64];
    int n = automount_count();
    int fd;
    int w;
    int r;
    int i;

    if (n <= 0) {
        serial_puts("[SELFTEST] disk-rw: SKIP (no disk)\n");
        return;
    }

    for (i = 0; i < n; i++) {
        if (!automount_writeable(i)) continue;

        serial_puts("[SELFTEST] disk-rw on ");
        serial_puts(automount_disk_name(i));
        serial_puts("\n");

        fd = open("disk0/WSELFTST.TXT", 0x0041, 0);
        if (fd < 0) { st_bad("disk-create"); return; }
        st_ok("disk-create");

        w = write(fd, (char *)probe, (int)(sizeof(probe) - 1));
        if (w != (int)(sizeof(probe) - 1)) { close(fd); st_bad("disk-write"); return; }
        st_ok("disk-write");

        lseek(fd, 0, 0);
        r = read(fd, buf, (int)sizeof(buf));
        close(fd);

        if (r != w) { st_bad("disk-read"); return; }
        {
            int k;
            for (k = 0; k < w; k++) {
                if (buf[k] != probe[k]) { st_bad("disk-content"); return; }
            }
        }
        st_ok("disk-read");
        return;
    }

    serial_puts("[SELFTEST] disk-rw: SKIP (read-only mounts)\n");
}

static void st_heap_aligned(void) {
    void *a = kmalloc_aligned(3000, 256);
    void *b = kmalloc_aligned(700, 4096);
    void *c = kmalloc(32);
    size_t i;

    if (!a || !b || !c) { st_bad("kheap-aligned-alloc"); return; }

    if (((uintptr_t)a & 255u) || ((uintptr_t)b & 4095u)) {
        st_bad("kheap-aligned-addr");
        goto out;
    }

    for (i = 0; i < 3000; i++) ((u8int *)a)[i] = (u8int)(i & 0x3F);
    for (i = 0; i < 3000; i++) {
        if (((u8int *)a)[i] != (u8int)(i & 0x3F)) { st_bad("kheap-aligned-rw"); goto out; }
    }

    st_ok("kheap-aligned");

out:
    kfree(a);
    kfree(b);
    kfree(c);
}

static void st_heap_coalesce(void) {
    kheap_stats_t before;
    kheap_stats_t after;
    void *blocks[16];
    int i;

    kheap_stats(&before);

    for (i = 0; i < 16; i++) {
        blocks[i] = kmalloc(2048);
        if (!blocks[i]) { st_bad("kheap-coalesce-alloc"); return; }
        ((u8int *)blocks[i])[0] = (u8int)i;
    }

    for (i = 0; i < 16; i++)
        kfree(blocks[i]);

    kheap_stats(&after);

    if (after.free_block_count == before.free_block_count &&
        after.largest_free == before.largest_free)
        st_ok("kheap-coalesce");
    else
        st_bad("kheap-coalesce");
}

static void st_pmm(void) {
    pmm_stats_t st;
    u64int a;
    u64int b;
    u64int run;

    pmm_get_stats(&st);

    if (!st.total_pages || !st.free_pages) { st_bad("pmm-stats"); return; }
    st_ok("pmm-stats");

    a = pmm_alloc_pages(4);
    if (!a) { st_bad("pmm-run-alloc"); return; }

    if (a & 0xFFFULL) { pmm_free_pages(a, 4); st_bad("pmm-run-align"); return; }

    for (u64int i = 0; i < 4; i++) {
        if (pmm_is_free(a + i * PMM_PAGE_SIZE)) {
            pmm_free_pages(a, 4);
            st_bad("pmm-run-exclusive");
            return;
        }
    }
    st_ok("pmm-run");

    pmm_free_pages(a, 4);
    if (!pmm_is_free(a)) { st_bad("pmm-free"); return; }
    st_ok("pmm-free");

    b = pmm_alloc_pages_aligned(2, 2ULL * 1024 * 1024);
    if (!b) { st_bad("pmm-aligned"); return; }
    if (b & (2ULL * 1024 * 1024 - 1)) { pmm_free_pages(b, 2); st_bad("pmm-aligned-addr"); return; }
    pmm_free_pages(b, 2);
    st_ok("pmm-aligned");

    run = pmm_alloc_pages_below(1, 0x100000000ULL);
    if (!run) { st_bad("pmm-below4g"); return; }
    if (run >= 0x100000000ULL) { pmm_free_pages(run, 1); st_bad("pmm-below4g-addr"); return; }
    pmm_free_pages(run, 1);
    st_ok("pmm-below4g");
}

static void st_vmm(void) {
    vmm_space_t space = vmm_new_address_space();
    u64int phys;
    u64int probe = 0x0000000040000000ULL;

    if (!space) { st_bad("vmm-space"); return; }
    st_ok("vmm-space");

    phys = pmm_alloc_page();
    if (!phys) { vmm_release_space(space, 0); st_bad("vmm-phys"); return; }

    if (vmm_map_page(space, probe, phys, PAGE_WRITE | PAGE_USER) != 0) {
        pmm_free_page(phys);
        vmm_release_space(space, 0);
        st_bad("vmm-map");
        return;
    }
    st_ok("vmm-map");

    if (vmm_translate(space, probe) != phys) {
        vmm_release_space(space, 0);
        pmm_free_page(phys);
        st_bad("vmm-translate");
        return;
    }
    st_ok("vmm-translate");

    if (vmm_translate(space, probe + 0x1000) != 0) {
        vmm_release_space(space, 0);
        pmm_free_page(phys);
        st_bad("vmm-unmapped-read");
        return;
    }
    st_ok("vmm-unmapped-read");

    if (vmm_kernel_space() != 0 &&
        vmm_translate(space, 0xFFFFFFFF80000000ULL) ==
        vmm_translate(vmm_kernel_space(), 0xFFFFFFFF80000000ULL))
        st_ok("vmm-kernel-shared");
    else
        st_bad("vmm-kernel-shared");

    vmm_release_space(space, 0);
    pmm_free_page(phys);
}


static void st_disk_unlink(void) {
    static const char body[] = "unlink probe";
    char buf[32];
    int fd;
    int w;
    int r;

    if (automount_count() <= 0) {
        serial_puts("[SELFTEST] disk-unlink: SKIP (no disk)\n");
        return;
    }

    if (!automount_writeable(0)) {
        serial_puts("[SELFTEST] disk-unlink: SKIP (read-only)\n");
        return;
    }

    unlink("disk0/WUNLINK.TXT");

    fd = open("disk0/WUNLINK.TXT", 0x0041, 0);
    if (fd < 0) { st_bad("disk-unlink-create"); return; }

    w = write(fd, (char *)body, (int)(sizeof(body) - 1));
    close(fd);

    if (w != (int)(sizeof(body) - 1)) { st_bad("disk-unlink-write"); return; }

    fd = open("disk0/WUNLINK.TXT", 0, 0);
    if (fd < 0) { st_bad("disk-unlink-reopen"); return; }

    r = read(fd, buf, (int)sizeof(buf));
    close(fd);

    if (r != w) { st_bad("disk-unlink-read"); return; }
    st_ok("disk-unlink-create");

    if (unlink("disk0/WUNLINK.TXT") != 0) { st_bad("disk-unlink-call"); return; }

    fd = open("disk0/WUNLINK.TXT", 0, 0);
    if (fd >= 0) { close(fd); st_bad("disk-unlink-gone"); return; }

    st_ok("disk-unlink");
}

static void st_disk_rename(void) {
    static const char body[] = "rename probe payload";
    char buf[32];
    int fd;
    int w;
    int r;

    if (automount_count() <= 0 || !automount_writeable(0)) {
        serial_puts("[SELFTEST] disk-rename: SKIP\n");
        return;
    }

    unlink("disk0/WRENAMEA.TXT");
    unlink("disk0/WRENAMEB.TXT");

    fd = open("disk0/WRENAMEA.TXT", 0x0041, 0);
    if (fd < 0) { st_bad("disk-rename-create"); return; }

    w = write(fd, (char *)body, (int)(sizeof(body) - 1));
    close(fd);

    if (w != (int)(sizeof(body) - 1)) { st_bad("disk-rename-write"); return; }

    if (rename("disk0/WRENAMEA.TXT", "disk0/WRENAMEB.TXT") != 0) {
        st_bad("disk-rename-call");
        return;
    }

    fd = open("disk0/WRENAMEA.TXT", 0, 0);
    if (fd >= 0) { close(fd); st_bad("disk-rename-old-gone"); return; }

    fd = open("disk0/WRENAMEB.TXT", 0, 0);
    if (fd < 0) { st_bad("disk-rename-new-open"); return; }

    r = read(fd, buf, (int)sizeof(buf));
    close(fd);

    if (r != w) { st_bad("disk-rename-read"); return; }

    for (int i = 0; i < w; i++) {
        if (buf[i] != body[i]) { st_bad("disk-rename-content"); return; }
    }

    st_ok("disk-rename");

    unlink("disk0/WRENAMEB.TXT");
}

void ax_selftest_run(void) {
    st_pass = 0;
    st_fail = 0;

    serial_puts("[SELFTEST] begin\n");

    st_heap();
    st_heap_reuse();
    st_heap_aligned();
    st_heap_coalesce();
    st_pmm();
    st_vmm();
    st_socketpair();
    st_unix_path();
    st_net();
    st_disk_rw();
    st_disk_unlink();
    st_disk_rename();

    {
        extern int ax_gl_selftest(void);
        int gl_fail = ax_gl_selftest();

        if (gl_fail)
            st_fail += gl_fail;
        else
            st_ok("mesa-gl");
    }

    serial_puts("[SELFTEST] done: ");
    debug_putnum((uint64_t)st_pass);
    serial_puts(" passed, ");
    debug_putnum((uint64_t)st_fail);
    serial_puts(" failed\n");
}
