/*
 * ============================================================
 * drivers/nvme.c -- polling-овый NVMe драйвер (PCIe SSD).
 * ============================================================
 *
 * Почти весь современный реальный парк (ноутбуки и ПК после ~2015)
 * имеет единственный диск -- NVMe.  Без этого драйвера ядро вообще
 * не видит такие системы, и никакое "автоопределение ФС" не может
 * сработать.
 *
 * Модель: одна admin-очередь (32 слота) + одна IO-очередь (SQ/CQ #1),
 * ровно одна команда "в полёте" -- простой и надёжный polling.
 * Данные идут через физически непрерывный bounce-буфер 64 КиБ
 * (PRP1 [+ PRP2 / PRP-список]).
 *
 * Поддержано:
 *   - NVMe 1.0+: регистры, сброс CC.EN, ожидание CSTS.RDY с таймаутом
 *     из CAP.TO, любой CAP.DSTRD;
 *   - Identify Controller (модель, MDTS), список активных namespace-ов
 *     (CNS=2, при отказе -- NSID 1), Identify Namespace: ёмкость и
 *     РАЗМЕР LBA (512..4096) из выбранного LBAF;
 *   - Read / Write / Flush;
 *   - namespace-ы с extended-LBA + метаданными пропускаются
 *     (формат метаданных в логике ФС не поддержан).
 *
 * Не поддержано: Intel RST/VMD remap, Apple ANS, мультипуть, PRP
 * при MPSMIN > 4 КиБ (контроллер отклоняется явным сообщением).
 */

#include "blkdev.h"
#include "pcix.h"
#include "../lib/common.h"

#define NV_CAP   0x00
#define NV_VS    0x08
#define NV_INTMS 0x0C
#define NV_CC    0x14
#define NV_CSTS  0x1C
#define NV_AQA   0x24
#define NV_ASQ   0x28
#define NV_ACQ   0x30

#define CC_EN    1u
#define CSTS_RDY 1u
#define CSTS_CFS 2u

#define BOUNCE_BYTES (64u * 1024u)
#define ADMIN_DEPTH  32u
#define IO_DEPTH     32u

typedef struct {
    u8int *sq;  u64int sq_phys;
    u8int *cq;  u64int cq_phys;
    u16int depth, sq_tail, cq_head, qid;
    u8int  phase;
} nvme_q_t;

typedef struct {
    volatile u8int *bar;
    u32int dstrd;               /* шаг doorbell = 4 << dstrd */
    u32int to_ms;
    nvme_q_t adm, io;
    u8int *idbuf;  u64int idbuf_phys;
    u8int *bounce; u64int bounce_phys;
    u8int *prpl;   u64int prpl_phys;
    u32int max_xfer;
    u16int cid;
    int    index;
} nvme_ctrl_t;

typedef struct {
    int    used;
    nvme_ctrl_t *c;
    u32int nsid;
    u32int lss;
} nvme_ns_t;

#define MAX_CTRL 4
#define MAX_NS   16
static nvme_ctrl_t g_ctrl[MAX_CTRL];
static int         g_ctrl_n = 0;
static nvme_ns_t   g_ns[MAX_NS];
static int         g_ns_n = 0;
static char        g_nvme_model_tmp[41];   /* модель текущего контроллера */

/* ------------------------------------------------------------------ */
/*  MMIO                                                               */
/* ------------------------------------------------------------------ */

static inline u32int rd32(nvme_ctrl_t *c, u32int off)
{ return *(volatile u32int *)(c->bar + off); }
static inline void wr32(nvme_ctrl_t *c, u32int off, u32int v)
{ *(volatile u32int *)(c->bar + off) = v; }
static void wr64(nvme_ctrl_t *c, u32int off, u64int v)
{ wr32(c, off, (u32int)v); wr32(c, off + 4, (u32int)(v >> 32)); }

static u32int db_sq(nvme_ctrl_t *c, u16int qid)
{ return 0x1000u + (2u * qid) * (4u << c->dstrd); }
static u32int db_cq(nvme_ctrl_t *c, u16int qid)
{ return 0x1000u + (2u * qid + 1u) * (4u << c->dstrd); }

static int wait_rdy(nvme_ctrl_t *c, u32int want)
{
    u32int us = c->to_ms * 1000u;
    while (us--) {
        u32int s = rd32(c, NV_CSTS);
        if ((s & CSTS_RDY) == want)
            return 1;
        if (want && (s & CSTS_CFS))
            return 0;
        pcix_udelay(1);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Очереди                                                            */
/* ------------------------------------------------------------------ */

/*
 * Отправить команду (16 dword) и дождаться завершения.
 * 1 = успех (status 0).  res0 (опц.) -- DW0 completion.
 */
static int q_cmd(nvme_ctrl_t *c, nvme_q_t *q, u32int cmd[16],
                 u32int timeout_ms, u32int *res0)
{
    cmd[0] = (cmd[0] & 0xFFFFu) | ((u32int)(c->cid++) << 16);

    volatile u32int *slot = (volatile u32int *)(q->sq + (u32int)q->sq_tail * 64u);
    for (int i = 0; i < 16; i++)
        slot[i] = cmd[i];

    q->sq_tail = (u16int)((q->sq_tail + 1u) % q->depth);
    __asm__ volatile("mfence" ::: "memory");
    wr32(c, db_sq(c, q->qid), q->sq_tail);

    volatile u32int *cqe = (volatile u32int *)(q->cq + (u32int)q->cq_head * 16u);
    u32int us = timeout_ms * 1000u;
    u32int dw3 = 0;
    int done = 0;

    while (us--) {
        dw3 = cqe[3];
        if (((dw3 >> 16) & 1u) == q->phase) { done = 1; break; }
        pcix_udelay(1);
    }
    if (!done)
        return 0;

    if (res0) *res0 = cqe[0];

    q->cq_head++;
    if (q->cq_head >= q->depth) { q->cq_head = 0; q->phase ^= 1u; }
    wr32(c, db_cq(c, q->qid), q->cq_head);

    return ((dw3 >> 17) & 0x7FFFu) == 0;
}

static int q_alloc(nvme_q_t *q, u16int qid, u16int depth)
{
    memset((u8int *)q, 0, sizeof(*q));
    q->sq = pcix_dma_alloc(1, 0, &q->sq_phys);
    q->cq = pcix_dma_alloc(1, 0, &q->cq_phys);
    q->depth = depth;
    q->qid = qid;
    q->phase = 1;
    return q->sq && q->cq;
}

/* ------------------------------------------------------------------ */
/*  Admin-команды                                                      */
/* ------------------------------------------------------------------ */

static int admin_identify(nvme_ctrl_t *c, u32int cns, u32int nsid)
{
    u32int cmd[16];
    memset((u8int *)cmd, 0, sizeof(cmd));
    cmd[0]  = 0x06;                         /* Identify */
    cmd[1]  = nsid;
    cmd[6]  = (u32int)(c->idbuf_phys & 0xFFFFFFFFu);
    cmd[7]  = (u32int)(c->idbuf_phys >> 32);
    cmd[10] = cns;
    memset(c->idbuf, 0, 4096);
    return q_cmd(c, &c->adm, cmd, 10000, 0);
}

static u32int ld32(const u8int *p)
{ return (u32int)p[0] | ((u32int)p[1] << 8) | ((u32int)p[2] << 16) | ((u32int)p[3] << 24); }

/* ------------------------------------------------------------------ */
/*  Инициализация контроллера                                          */
/* ------------------------------------------------------------------ */

static void register_ns(nvme_ctrl_t *c, u32int nsid)
{
    if (g_ns_n >= MAX_NS)
        return;
    if (!admin_identify(c, 0, nsid))
        return;

    const u8int *id = c->idbuf;
    u64int nsze  = (u64int)ld32(id) | ((u64int)ld32(id + 4) << 32);
    u8int  flbas = id[26];
    u32int idx   = (u32int)(flbas & 0x0F) | ((u32int)((flbas >> 5) & 3) << 4);
    u32int lbaf  = ld32(id + 128 + 4 * idx);
    u32int ms    = lbaf & 0xFFFFu;
    u32int ds    = (lbaf >> 16) & 0xFFu;

    if (nsze == 0)
        return;
    if (ds < 9 || ds > 12) {
        debug_puts("[NVME] ns "); debug_putnum(nsid);
        debug_puts(": unsupported LBA size 2^"); debug_putnum(ds);
        debug_puts(" -- skipped\n");
        return;
    }
    if ((flbas & 0x10) && ms != 0) {
        debug_puts("[NVME] ns "); debug_putnum(nsid);
        debug_puts(": extended LBA with metadata -- skipped\n");
        return;
    }

    nvme_ns_t *n = &g_ns[g_ns_n];
    n->used = 1; n->c = c; n->nsid = nsid; n->lss = 1u << ds;

    blkdev_t d;
    memset((u8int *)&d, 0, sizeof(d));
    d.type = BLKDEV_NVME;
    d.native_sectors = nsze;
    d.lss  = n->lss;
    d.port = g_ns_n;
    {
        char *p = d.name;
        const char *pre = "nvme";
        while (*pre) *p++ = *pre++;
        *p++ = (char)('0' + (c->index % 10));
        *p++ = 'n';
        u32int v = nsid; char t[12]; int k = 0;
        if (!v) t[k++] = '0';
        while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
        while (k) *p++ = t[--k];
        *p = 0;
    }
    /* Модель контроллера запомнена в model-буфере ctrl (см. ниже). */
    for (int i = 0; i < 40 && g_nvme_model_tmp[i]; i++)
        d.model[i] = g_nvme_model_tmp[i];

    if (blkdev_add(&d) >= 0)
        g_ns_n++;
    else
        n->used = 0;
}

int nvme_init_controller(const pcix_dev_t *dev)
{
    if (g_ctrl_n >= MAX_CTRL)
        return 0;

    u64int bar_phys = 0; int is_io = 0;
    if (!pcix_bar(dev, 0, &bar_phys, &is_io, 0) || is_io) {
        debug_puts("[NVME] BAR0 not assigned -- skipped\n");
        return 0;
    }
    pcix_enable(dev, PCIX_EN_MEM | PCIX_EN_BM | PCIX_DIS_INTX);

    /* Сначала минимум, чтобы прочитать CAP и узнать размер doorbell-ов. */
    volatile u8int *bar0 = (volatile u8int *)pcix_map_mmio(bar_phys, 0x1000);
    if (!bar0) { debug_puts("[NVME] MMIO map failed\n"); return 0; }

    u64int cap = (u64int)(*(volatile u32int *)bar0) |
                 ((u64int)(*(volatile u32int *)(bar0 + 4)) << 32);
    if (cap == 0xFFFFFFFFFFFFFFFFull) {
        debug_puts("[NVME] controller does not respond -- skipped\n");
        return 0;
    }

    u32int dstrd  = (u32int)((cap >> 32) & 0xF);
    u32int mpsmin = (u32int)((cap >> 48) & 0xF);
    u32int to     = (u32int)((cap >> 24) & 0xFF);
    u32int mqes   = (u32int)(cap & 0xFFFF);

    debug_puts("[NVME] controller "); debug_putnum(dev->bus); debug_putc(':');
    debug_putnum(dev->dev); debug_putc('.'); debug_putnum(dev->fn);
    debug_puts(" CAP="); pcix_puthex(cap); debug_putc('\n');

    if (mpsmin != 0) {
        debug_puts("[NVME] MPSMIN > 4 KiB not supported -- skipped\n");
        return 0;
    }
    if (!((cap >> 37) & 1u)) {
        debug_puts("[NVME] NVM command set not supported -- skipped\n");
        return 0;
    }

    u64int map_sz = 0x1000u + 4u * (4u << dstrd);
    if (map_sz < 0x2000u) map_sz = 0x2000u;
    volatile u8int *bar = (volatile u8int *)pcix_map_mmio(bar_phys, map_sz);
    if (!bar) { debug_puts("[NVME] MMIO map failed\n"); return 0; }

    nvme_ctrl_t *c = &g_ctrl[g_ctrl_n];
    memset((u8int *)c, 0, sizeof(*c));
    c->bar   = bar;
    c->dstrd = dstrd;
    c->to_ms = (to ? to : 1u) * 500u;
    c->index = g_ctrl_n;

    /* Выключить контроллер. */
    if (rd32(c, NV_CC) & CC_EN)
        wr32(c, NV_CC, rd32(c, NV_CC) & ~CC_EN);
    if (!wait_rdy(c, 0)) {
        debug_puts("[NVME] controller will not reset -- skipped\n");
        return 0;
    }

    u16int adepth = (u16int)((mqes + 1u) < ADMIN_DEPTH ? (mqes + 1u) : ADMIN_DEPTH);
    u16int idepth = (u16int)((mqes + 1u) < IO_DEPTH ? (mqes + 1u) : IO_DEPTH);
    if (adepth < 2 || idepth < 2) { debug_puts("[NVME] queues too small\n"); return 0; }

    c->idbuf  = pcix_dma_alloc(1, 0, &c->idbuf_phys);
    c->bounce = pcix_dma_alloc(BOUNCE_BYTES / 4096u, 0, &c->bounce_phys);
    c->prpl   = pcix_dma_alloc(1, 0, &c->prpl_phys);
    if (!q_alloc(&c->adm, 0, adepth) || !q_alloc(&c->io, 1, idepth) ||
        !c->idbuf || !c->bounce || !c->prpl) {
        debug_puts("[NVME] no DMA memory -- skipped\n");
        return 0;
    }

    /* PRP-список для передач > 8 КиБ: страницы 1..15 bounce-буфера. */
    {
        u64int *l = (u64int *)c->prpl;
        for (u32int i = 1; i < BOUNCE_BYTES / 4096u; i++)
            l[i - 1] = c->bounce_phys + (u64int)i * 4096u;
    }

    wr32(c, NV_AQA, ((u32int)(adepth - 1) << 16) | (u32int)(adepth - 1));
    wr64(c, NV_ASQ, c->adm.sq_phys);
    wr64(c, NV_ACQ, c->adm.cq_phys);

    /* IOSQES=6 (64B), IOCQES=4 (16B), MPS=0 (4K), CSS=0 (NVM), EN. */
    wr32(c, NV_CC, (6u << 16) | (4u << 20) | CC_EN);
    if (!wait_rdy(c, 1)) {
        debug_puts("[NVME] controller did not become ready -- skipped\n");
        return 0;
    }
    wr32(c, NV_INTMS, 0xFFFFFFFFu);         /* только опрос */

    /* Identify Controller. */
    if (!admin_identify(c, 1, 0)) {
        debug_puts("[NVME] Identify Controller failed\n");
        return 0;
    }
    {
        const u8int *id = c->idbuf;
        int k = 0;
        for (int i = 0; i < 40; i++) {
            char ch = (char)id[24 + i];
            g_nvme_model_tmp[k++] = (ch < 0x20 || ch > 0x7E) ? ' ' : ch;
        }
        while (k > 0 && g_nvme_model_tmp[k - 1] == ' ') k--;
        g_nvme_model_tmp[k] = 0;

        u32int mdts = id[77];
        c->max_xfer = BOUNCE_BYTES;
        if (mdts && (4096u << mdts) < c->max_xfer)
            c->max_xfer = 4096u << mdts;
    }

    /* Set Features: Number of Queues = 1+1 (ошибку игнорируем). */
    {
        u32int cmd[16];
        memset((u8int *)cmd, 0, sizeof(cmd));
        cmd[0] = 0x09; cmd[10] = 0x07; cmd[11] = 0;
        q_cmd(c, &c->adm, cmd, 5000, 0);
    }

    /* Create I/O Completion Queue #1, затем Submission Queue #1. */
    {
        u32int cmd[16];
        memset((u8int *)cmd, 0, sizeof(cmd));
        cmd[0]  = 0x05;
        cmd[6]  = (u32int)(c->io.cq_phys & 0xFFFFFFFFu);
        cmd[7]  = (u32int)(c->io.cq_phys >> 32);
        cmd[10] = ((u32int)(c->io.depth - 1) << 16) | 1u;
        cmd[11] = 1u;                       /* PC=1, IEN=0 */
        if (!q_cmd(c, &c->adm, cmd, 5000, 0)) {
            debug_puts("[NVME] Create IO CQ failed\n");
            return 0;
        }
        memset((u8int *)cmd, 0, sizeof(cmd));
        cmd[0]  = 0x01;
        cmd[6]  = (u32int)(c->io.sq_phys & 0xFFFFFFFFu);
        cmd[7]  = (u32int)(c->io.sq_phys >> 32);
        cmd[10] = ((u32int)(c->io.depth - 1) << 16) | 1u;
        cmd[11] = 1u | (1u << 16);          /* PC=1, CQID=1 */
        if (!q_cmd(c, &c->adm, cmd, 5000, 0)) {
            debug_puts("[NVME] Create IO SQ failed\n");
            return 0;
        }
    }

    g_ctrl_n++;
    int before = g_ns_n;

    /* Активные namespace-ы. */
    if (admin_identify(c, 2, 0)) {
        u32int nsids[16]; int nn = 0;
        for (int i = 0; i < 16; i++) {
            u32int v = ld32(c->idbuf + i * 4);
            if (!v) break;
            nsids[nn++] = v;
        }
        for (int i = 0; i < nn; i++)
            register_ns(c, nsids[i]);
    } else {
        register_ns(c, 1);
    }

    return g_ns_n - before;
}

/* ------------------------------------------------------------------ */
/*  Чтение / запись / flush                                            */
/* ------------------------------------------------------------------ */

int nvme_rw(int ns_idx, uint64_t lba, uint32_t count, void *buf, int write)
{
    if (ns_idx < 0 || ns_idx >= g_ns_n || !g_ns[ns_idx].used)
        return 0;

    nvme_ns_t   *n = &g_ns[ns_idx];
    nvme_ctrl_t *c = n->c;
    u8int *p = (u8int *)buf;

    u32int per = c->max_xfer / n->lss;
    if (per == 0) return 0;
    if (per > 65535u) per = 65535u;

    u32int done = 0;
    while (done < count) {
        u32int chunk = count - done;
        if (chunk > per) chunk = per;
        u32int bytes = chunk * n->lss;

        if (write)
            memcpy(c->bounce, p + (u64int)done * n->lss, bytes);

        u32int cmd[16];
        memset((u8int *)cmd, 0, sizeof(cmd));
        cmd[0]  = write ? 0x01u : 0x02u;
        cmd[1]  = n->nsid;
        cmd[6]  = (u32int)(c->bounce_phys & 0xFFFFFFFFu);
        cmd[7]  = (u32int)(c->bounce_phys >> 32);
        if (bytes > 8192u) {
            cmd[8] = (u32int)(c->prpl_phys & 0xFFFFFFFFu);
            cmd[9] = (u32int)(c->prpl_phys >> 32);
        } else if (bytes > 4096u) {
            u64int p2 = c->bounce_phys + 4096u;
            cmd[8] = (u32int)(p2 & 0xFFFFFFFFu);
            cmd[9] = (u32int)(p2 >> 32);
        }
        cmd[10] = (u32int)(lba & 0xFFFFFFFFu);
        cmd[11] = (u32int)(lba >> 32);
        cmd[12] = chunk - 1u;

        if (!q_cmd(c, &c->io, cmd, 10000, 0))
            return 0;

        if (!write)
            memcpy(p + (u64int)done * n->lss, c->bounce, bytes);

        done += chunk;
        lba  += chunk;
    }
    return 1;
}

int nvme_flush(int ns_idx)
{
    if (ns_idx < 0 || ns_idx >= g_ns_n || !g_ns[ns_idx].used)
        return 0;
    nvme_ns_t *n = &g_ns[ns_idx];
    u32int cmd[16];
    memset((u8int *)cmd, 0, sizeof(cmd));
    cmd[0] = 0x00;                 /* Flush */
    cmd[1] = n->nsid;
    return q_cmd(n->c, &n->c->io, cmd, 30000, 0);
}
