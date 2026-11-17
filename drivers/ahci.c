/*
 * ============================================================
 * drivers/ahci.c -- polling-овый AHCI (SATA) драйвер.
 * ============================================================
 *
 * Работает с любым числом AHCI-контроллеров, расположенных в любом
 * месте иерархии PCI (blkdev.c находит их через pcix_enum()).
 *
 * Что учтено для реального железа (в QEMU всё это "работает само"):
 *
 *   - ABAR берётся из BAR5 С МАСКОЙ флагов (биты 3:0 -- не адрес);
 *   - BIOS/OS Handoff (CAP2.BOH / BOHC): если прошивка владеет HBA --
 *     корректно забираем управление;
 *   - если PI == 0 (баг прошивок), порты берутся из CAP.NP;
 *   - порты останавливаются ПЕРЕД перепрограммированием CLB/FB;
 *     если остановка не удалась -- HBA reset;
 *   - питание/spin-up (POD/SUD), пробуждение линка (ICC) из
 *     Partial/Slumber, ожидание DET==3 с таймаутом и COMRESET
 *     при неподнявшемся линке;
 *   - ожидание готовности устройства (TFD.BSY/DRQ) до старта порта;
 *   - SIG: берём только ATA (0x101); ATAPI/PM/SEMB пропускаются;
 *   - IDENTIFY с повторами; логический сектор 512/4096 (4Kn) и
 *     LBA48/LBA28 по IDENTIFY, модель диска;
 *   - DMA-структуры ниже 4 ГиБ только если HBA НЕ умеет 64 бит
 *     (CAP.S64A); иначе любая память;
 *   - восстановление порта после ошибки/таймаута команды
 *     (stop -> clear SERR/IS -> start);
 *   - FLUSH CACHE (EXT).
 */

#include "ahci.h"
#include "blkdev.h"
#include "pcix.h"
#include "../lib/common.h"

/* ------------------------------------------------------------------ */
/*  Регистры                                                           */
/* ------------------------------------------------------------------ */

static inline u32int rd32(volatile u8int *base, u32int off)
{ return *(volatile u32int *)(base + off); }
static inline void wr32(volatile u8int *base, u32int off, u32int v)
{ *(volatile u32int *)(base + off) = v; }

#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_IS     0x08
#define HBA_PI     0x0C
#define HBA_VS     0x10
#define HBA_CAP2   0x24
#define HBA_BOHC   0x28

#define CAP_S64A   (1u << 31)
#define CAP_SSS    (1u << 27)
#define GHC_HR     (1u << 0)
#define GHC_IE     (1u << 1)
#define GHC_AE     (1u << 31)
#define BOHC_BOS   (1u << 0)
#define BOHC_OOS   (1u << 1)
#define BOHC_BB    (1u << 4)

#define PORT_OFF(i)  (0x100u + (u32int)(i) * 0x80u)
#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_IE    0x14
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SCTL  0x2C
#define P_SERR  0x30
#define P_CI    0x38

#define CMD_ST   (1u << 0)
#define CMD_SUD  (1u << 1)
#define CMD_POD  (1u << 2)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)
#define CMD_ICC_MASK (0xFu << 28)
#define CMD_ICC_ACTIVE (1u << 28)

#define TFD_ERR  (1u << 0)
#define TFD_DRQ  (1u << 3)
#define TFD_DF   (1u << 5)
#define TFD_BSY  (1u << 7)

#define IS_TFES  (1u << 30)

#define SSTS_DET(v) ((v) & 0xF)
#define SSTS_IPM(v) (((v) >> 8) & 0xF)

#define SIG_ATA    0x00000101u
#define SIG_ATAPI  0xEB140101u
#define SIG_PM     0x96690101u
#define SIG_SEMB   0xC33C0101u

#define BOUNCE_BYTES (64u * 1024u)

/* ------------------------------------------------------------------ */
/*  Состояние                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    volatile u8int *abar;
    u32int cap;
    int    s64a;
} ahci_hba_t;

typedef struct {
    int   used;
    ahci_hba_t *hba;
    int   port;                 /* номер порта в HBA */
    int   lba48;
    u32int lss;
    u32int bounce_sectors;      /* сколько родных секторов за команду */
    u8int *clb;  u64int clb_phys;
    u8int *fb;   u64int fb_phys;
    u8int *ctb;  u64int ctb_phys;
    u8int *id;   u64int id_phys;
    u8int *bounce; u64int bounce_phys;
} ahci_port_t;

#define MAX_HBA 8
static ahci_hba_t  g_hba[MAX_HBA];
static int         g_hba_n = 0;
static ahci_port_t g_port[AHCI_MAX_PORTS];
static int         g_port_n = 0;
static int         g_sata_name = 0;

/* ------------------------------------------------------------------ */
/*  Ожидания (мс)                                                      */
/* ------------------------------------------------------------------ */

/* Ждать, пока (reg & mask) == want.  1 = дождались, 0 = таймаут. */
static int wait_bits(volatile u8int *abar, u32int off, u32int mask,
                     u32int want, u32int ms)
{
    u32int us = ms * 1000u;
    while (us--) {
        if ((rd32(abar, off) & mask) == want)
            return 1;
        pcix_udelay(1);
    }
    return (rd32(abar, off) & mask) == want;
}

/* ------------------------------------------------------------------ */
/*  Старт / стоп / восстановление порта                                */
/* ------------------------------------------------------------------ */

static int port_stop(volatile u8int *abar, int p)
{
    u32int off = PORT_OFF(p);
    u32int cmd = rd32(abar, off + P_CMD);

    if (cmd & (CMD_ST | CMD_CR)) {
        wr32(abar, off + P_CMD, cmd & ~CMD_ST);
        if (!wait_bits(abar, off + P_CMD, CMD_CR, 0, 500))
            return 0;
    }
    cmd = rd32(abar, off + P_CMD);
    if (cmd & (CMD_FRE | CMD_FR)) {
        wr32(abar, off + P_CMD, cmd & ~CMD_FRE);
        if (!wait_bits(abar, off + P_CMD, CMD_FR, 0, 500))
            return 0;
    }
    return 1;
}

static void port_start(volatile u8int *abar, int p)
{
    u32int off = PORT_OFF(p);
    wait_bits(abar, off + P_CMD, CMD_CR, 0, 500);
    wr32(abar, off + P_CMD, (rd32(abar, off + P_CMD) | CMD_FRE));
    wr32(abar, off + P_CMD, (rd32(abar, off + P_CMD) | CMD_ST));
}

static void port_recover(ahci_port_t *pt)
{
    volatile u8int *abar = pt->hba->abar;
    u32int off = PORT_OFF(pt->port);

    port_stop(abar, pt->port);
    wr32(abar, off + P_SERR, 0xFFFFFFFFu);
    wr32(abar, off + P_IS,   0xFFFFFFFFu);
    port_start(abar, pt->port);
}

/* ------------------------------------------------------------------ */
/*  Выдача команды (слот 0, polling)                                   */
/* ------------------------------------------------------------------ */

static int issue_command(ahci_port_t *pt, const u8int *fis, int w,
                         u64int buf_phys, u32int len, u32int timeout_ms)
{
    volatile u8int *abar = pt->hba->abar;
    u32int off = PORT_OFF(pt->port);

    if (len > BOUNCE_BYTES)
        return 0;

    if (!wait_bits(abar, off + P_CI, 1u, 0, 200)) {
        port_recover(pt);
        if (!wait_bits(abar, off + P_CI, 1u, 0, 200))
            return 0;
    }

    wr32(abar, off + P_IS, 0xFFFFFFFFu);

    volatile u32int *hdr = (volatile u32int *)pt->clb;
    hdr[0] = 5u | (w ? (1u << 6) : 0u) | (len ? (1u << 16) : 0u);
    hdr[1] = 0;
    hdr[2] = (u32int)(pt->ctb_phys & 0xFFFFFFFFu);
    hdr[3] = (u32int)(pt->ctb_phys >> 32);
    hdr[4] = hdr[5] = hdr[6] = hdr[7] = 0;

    for (int i = 0; i < 20; i++)
        pt->ctb[i] = (i < 16) ? fis[i] : 0;

    if (len) {
        volatile u32int *prdt = (volatile u32int *)(pt->ctb + 0x80);
        prdt[0] = (u32int)(buf_phys & 0xFFFFFFFFu);
        prdt[1] = (u32int)(buf_phys >> 32);
        prdt[2] = 0;
        prdt[3] = (len - 1) & 0x3FFFFFu;
    }

    __asm__ volatile("mfence" ::: "memory");
    wr32(abar, off + P_CI, 1u);

    u32int us = timeout_ms * 1000u;
    int done = 0;
    while (us--) {
        u32int is = rd32(abar, off + P_IS);
        if (is & IS_TFES)
            break;                              /* ошибка устройства */
        if (!(rd32(abar, off + P_CI) & 1u)) {
            done = 1;
            break;
        }
        pcix_udelay(1);
    }

    if (done) {
        u32int tfd = rd32(abar, off + P_TFD);
        if (!(tfd & (TFD_ERR | TFD_DF)) &&
            !(rd32(abar, off + P_IS) & IS_TFES))
            return 1;
    }

    port_recover(pt);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Линк: COMRESET и ожидание                                          */
/* ------------------------------------------------------------------ */

/* COMRESET (SCTL.DET=1 >= 1 мс, затем 0).  FIS receive (FRE) должен быть
 * включён ДО вызова -- иначе ответный D2H-FIS с сигнатурой теряется. */
static void port_comreset(volatile u8int *abar, u32int off)
{
    u32int sctl = rd32(abar, off + P_SCTL);
    wr32(abar, off + P_SCTL, (sctl & ~0xFu) | 1u);
    pcix_mdelay(3);
    wr32(abar, off + P_SCTL, sctl & ~0xFu);
}

/* Ждать DET==3 до ms миллисекунд. */
static int wait_det3(volatile u8int *abar, u32int off, u32int ms)
{
    for (u32int i = 0; i <= ms; i++) {
        if (SSTS_DET(rd32(abar, off + P_SSTS)) == 3)
            return 1;
        pcix_mdelay(1);
    }
    return 0;
}

/*
 * 1 = устройство подключено и линк поднят.
 * Требует: FRE включён, ST выключен.
 *
 * На машинах с Fast Boot прошивка не всегда успевает/хочет поднять SATA-
 * линк, поэтому DET==0 при старте НЕ означает "пусто": как и Linux
 * (libata), выполняем COMRESET и ждём отклик.
 */
static int port_link_up(volatile u8int *abar, int p)
{
    u32int off = PORT_OFF(p);

#ifdef AHCI_FORCE_COMRESET     /* отладка: проверка пути reset на любом железе */
    if (1) {
#else
    if (SSTS_DET(rd32(abar, off + P_SSTS)) != 3) {
#endif
        u32int det0 = SSTS_DET(rd32(abar, off + P_SSTS));

        port_comreset(abar, off);

        /* Присутствие (DET!=0) проявляется за десятки мс. */
        u32int i;
        for (i = 0; i < 150; i++) {
            if (SSTS_DET(rd32(abar, off + P_SSTS)) != 0) break;
            pcix_mdelay(1);
        }
        if (SSTS_DET(rd32(abar, off + P_SSTS)) == 0) {
            (void)det0;
            return 0;                   /* порт действительно пуст */
        }
        /* Устройство есть -- дать до 2 с на согласование скорости. */
        if (!wait_det3(abar, off, 2000))
            return 0;
    }

    /*
     * Выйти из Partial/Slumber.
     *
     * r14-vboxfix: VirtualBox DHX держит PxCMD.ICC не в состоянии Active,
     * и ждать обнуления поля бессмысленно: старый код ловил 10 мс таймаут,
     * после чего ICC оставался как был.  Порт при этом формально жив
     * (DET=3), но IDENTIFY не проходит и диск пропадает.  Выставляем
     * ICC=1 напрямую (как libata) и ждём IPM=1 с запасом.
     */
    {
        u32int ipm0 = SSTS_IPM(rd32(abar, off + P_SSTS));

        if (ipm0 != 1) {
            u32int cmd = rd32(abar, off + P_CMD);

            if ((cmd & CMD_ICC_MASK) != CMD_ICC_ACTIVE)
                wr32(abar, off + P_CMD,
                     (cmd & ~CMD_ICC_MASK) | CMD_ICC_ACTIVE);

            for (int i = 0; i < 200; i++) {
                if (SSTS_IPM(rd32(abar, off + P_SSTS)) == 1)
                    break;
                pcix_mdelay(1);
            }

            /* HBA может обновить IPM только после первого FIS: если линк
             * установлен (DET==3), IDENTIFY ниже проверит устройство. */
            if (SSTS_IPM(rd32(abar, off + P_SSTS)) != 1 &&
                SSTS_DET(rd32(abar, off + P_SSTS)) != 3)
                return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  IDENTIFY                                                           */
/* ------------------------------------------------------------------ */

static int ahci_identify(ahci_port_t *pt, uint64_t *sectors, uint32_t *lss,
                         int *lba48, char model[41])
{
    u8int fis[16];
    for (int attempt = 0; attempt < 3; attempt++) {
        memset(fis, 0, sizeof(fis));
        fis[0] = 0x27; fis[1] = 0x80; fis[2] = 0xEC; fis[7] = 0x40;
        memset(pt->id, 0, 512);

        if (issue_command(pt, fis, 0, pt->id_phys, 512, 5000))
            return blkdev_ata_parse_identify((const u16int *)pt->id, sectors,
                                             lss, lba48, model);
        pcix_mdelay(50);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Инициализация одного порта                                         */
/* ------------------------------------------------------------------ */

static void port_log(int p, const char *msg)
{
    debug_puts("[AHCI] port "); debug_putnum((uint64_t)p);
    debug_puts(": "); debug_puts(msg); debug_putc('\n');
}

static void port_regs_log(volatile u8int *abar, int p, const char *tag)
{
    u32int off = PORT_OFF(p);
    debug_puts("[AHCI] port "); debug_putnum((uint64_t)p);
    debug_puts(": "); debug_puts(tag);
    debug_puts(" SSTS="); pcix_puthexw(rd32(abar, off + P_SSTS), 8);
    debug_puts(" SERR="); pcix_puthexw(rd32(abar, off + P_SERR), 8);
    debug_puts(" TFD=");  pcix_puthexw(rd32(abar, off + P_TFD), 8);
    debug_puts(" SIG=");  pcix_puthexw(rd32(abar, off + P_SIG), 8);
    debug_puts(" CMD=");  pcix_puthexw(rd32(abar, off + P_CMD), 8);
    debug_putc('\n');
}

static void port_free_dma(ahci_port_t *pt)
{
    if (pt->clb)    pcix_dma_free(pt->clb, pt->clb_phys, 1);
    if (pt->fb)     pcix_dma_free(pt->fb, pt->fb_phys, 1);
    if (pt->ctb)    pcix_dma_free(pt->ctb, pt->ctb_phys, 1);
    if (pt->id)     pcix_dma_free(pt->id, pt->id_phys, 1);
    if (pt->bounce) pcix_dma_free(pt->bounce, pt->bounce_phys, BOUNCE_BYTES / 4096u);
    memset((u8int *)pt, 0, sizeof(*pt));
}

static void init_port(ahci_hba_t *h, int p)
{
    volatile u8int *abar = h->abar;
    u32int off = PORT_OFF(p);

    if (g_port_n >= AHCI_MAX_PORTS)
        return;

    ahci_port_t *pt = &g_port[g_port_n];
    memset((u8int *)pt, 0, sizeof(*pt));
    pt->hba  = h;
    pt->port = p;

    /* Питание и spin-up. */
    wr32(abar, off + P_CMD, rd32(abar, off + P_CMD) | CMD_POD | CMD_SUD);

    /* 1. Остановить движок и подготовить структуры -- ДО любого reset-а. */
    if (!port_stop(abar, p)) {
        port_log(p, "cannot stop engine -- skipped");
        return;
    }

    int low = !h->s64a;
    pt->clb = pcix_dma_alloc(1, low, &pt->clb_phys);
    pt->fb  = pcix_dma_alloc(1, low, &pt->fb_phys);
    pt->ctb = pcix_dma_alloc(1, low, &pt->ctb_phys);
    pt->id  = pcix_dma_alloc(1, low, &pt->id_phys);
    if (!pt->clb || !pt->fb || !pt->ctb || !pt->id) {
        port_log(p, "no DMA memory -- skipped");
        port_free_dma(pt);
        return;
    }

    wr32(abar, off + P_CLB,  (u32int)(pt->clb_phys & 0xFFFFFFFFu));
    wr32(abar, off + P_CLBU, (u32int)(pt->clb_phys >> 32));
    wr32(abar, off + P_FB,   (u32int)(pt->fb_phys & 0xFFFFFFFFu));
    wr32(abar, off + P_FBU,  (u32int)(pt->fb_phys >> 32));
    wr32(abar, off + P_IE,   0);                 /* только опрос */
    wr32(abar, off + P_SERR, 0xFFFFFFFFu);
    wr32(abar, off + P_IS,   0xFFFFFFFFu);

    /* 2. FIS receive ON: теперь D2H-FIS после reset будет принят. */
    wr32(abar, off + P_CMD, rd32(abar, off + P_CMD) | CMD_FRE);

    /* 3. Линк (COMRESET при необходимости). */
    if (!port_link_up(abar, p)) {
        u32int ss = rd32(abar, off + P_SSTS);
        debug_puts("[AHCI] port "); debug_putnum((uint64_t)p);
        debug_puts(": empty/no link (SSTS="); pcix_puthexw(ss, 8);
        debug_puts(" DET="); debug_putnum(SSTS_DET(ss));
        debug_puts(" IPM="); debug_putnum(SSTS_IPM(ss));
        debug_puts(")\n");
        port_stop(abar, p);
        port_free_dma(pt);
        return;
    }
    wr32(abar, off + P_SERR, 0xFFFFFFFFu);
    port_regs_log(abar, p, "link up");

    /* 4. Устройство готово (BSY/DRQ=0): spin-up HDD до ~10 с;
     *    если не отвечает -- один hard reset и ещё ожидание. */
    if (!wait_bits(abar, off + P_TFD, TFD_BSY | TFD_DRQ, 0, 10000)) {
        port_log(p, "device busy -- COMRESET retry");
        port_comreset(abar, off);
        wait_det3(abar, off, 2000);
        wr32(abar, off + P_SERR, 0xFFFFFFFFu);
        if (!wait_bits(abar, off + P_TFD, TFD_BSY | TFD_DRQ, 0, 8000)) {
            port_regs_log(abar, p, "stays busy -- skipped");
            port_stop(abar, p);
            port_free_dma(pt);
            return;
        }
    }

    /* 5. Сигнатура. */
    u32int sig = 0xFFFFFFFFu;
    for (int i = 0; i < 2000; i++) {
        sig = rd32(abar, off + P_SIG);
        if (sig != 0xFFFFFFFFu)
            break;
        pcix_mdelay(1);
    }

    if (sig != SIG_ATA) {
        debug_puts("[AHCI] port "); debug_putnum((uint64_t)p);
        if (sig == SIG_ATAPI)      debug_puts(": ATAPI (optical) -- skipped\n");
        else if (sig == SIG_PM)    debug_puts(": port multiplier -- skipped\n");
        else if (sig == SIG_SEMB)  debug_puts(": enclosure -- skipped\n");
        else { debug_puts(": unknown SIG "); pcix_puthex(sig); debug_puts(" -- skipped\n"); }
        port_stop(abar, p);
        port_free_dma(pt);
        return;
    }

    /* 6. Старт движка и IDENTIFY (до 3 попыток; между ними recovery). */
    port_start(abar, p);

    uint64_t sectors = 0; uint32_t lss = 512; int lba48 = 0; char model[41];
    if (!ahci_identify(pt, &sectors, &lss, &lba48, model)) {
        port_regs_log(abar, p, "IDENTIFY failed");
        port_stop(abar, p);
        port_free_dma(pt);
        return;
    }

    pt->bounce = pcix_dma_alloc(BOUNCE_BYTES / 4096u, low, &pt->bounce_phys);
    if (!pt->bounce) {
        port_log(p, "no DMA memory for data buffer -- skipped");
        port_stop(abar, p);
        port_free_dma(pt);
        return;
    }

    pt->lss   = lss;
    pt->lba48 = lba48;
    pt->bounce_sectors = BOUNCE_BYTES / lss;
    if (pt->bounce_sectors > 128) pt->bounce_sectors = 128;
    pt->used = 1;

    blkdev_t d;
    memset((u8int *)&d, 0, sizeof(d));
    d.type = BLKDEV_AHCI;
    d.native_sectors = sectors;
    d.lss  = lss;
    d.port = g_port_n;
    d.lba48 = lba48;
    {
        char *n = d.name;
        const char *pre = "sata";
        while (*pre) *n++ = *pre++;
        int v = g_sata_name++;
        char t[8]; int k = 0;
        if (v == 0) t[k++] = '0';
        while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
        while (k) *n++ = t[--k];
        *n = 0;
    }
    for (int i = 0; i < 40 && model[i]; i++) d.model[i] = model[i];

    if (blkdev_add(&d) >= 0)
        g_port_n++;
    else {
        port_stop(abar, p);
        port_free_dma(pt);
    }
}

/* ------------------------------------------------------------------ */
/*  Контроллер                                                         */
/* ------------------------------------------------------------------ */

int ahci_init_controller(const pcix_dev_t *d)
{
    if (g_hba_n >= MAX_HBA)
        return 0;

    u64int abar_phys = 0;
    int is_io = 0;
    if (!pcix_bar(d, 5, &abar_phys, &is_io, 0) || is_io) {
        debug_puts("[AHCI] BAR5 (ABAR) not assigned -- skipped\n");
        return 0;
    }

    pcix_enable(d, PCIX_EN_MEM | PCIX_EN_BM | PCIX_DIS_INTX);

    volatile u8int *abar = (volatile u8int *)pcix_map_mmio(abar_phys, 0x1100);
    if (!abar) {
        debug_puts("[AHCI] MMIO map failed\n");
        return 0;
    }

    u32int cap = rd32(abar, HBA_CAP);
    if (cap == 0xFFFFFFFFu) {
        debug_puts("[AHCI] HBA does not respond -- skipped\n");
        return 0;
    }
    u32int vs = rd32(abar, HBA_VS);

    debug_puts("[AHCI] controller "); debug_putnum(d->bus); debug_putc(':');
    debug_putnum(d->dev); debug_putc('.'); debug_putnum(d->fn);
    debug_puts(" ABAR="); pcix_puthex(abar_phys);
    debug_puts(" VS="); pcix_puthex(vs);
    debug_puts(" CAP="); pcix_puthex(cap);
    debug_putc('\n');

    /* BIOS/OS Handoff. */
    if (vs >= 0x00010200u && (rd32(abar, HBA_CAP2) & 1u)) {
        u32int bohc = rd32(abar, HBA_BOHC);
        if (bohc & BOHC_BOS) {
            debug_puts("[AHCI] firmware owns HBA -- requesting handoff\n");
            wr32(abar, HBA_BOHC, bohc | BOHC_OOS);
            wait_bits(abar, HBA_BOHC, BOHC_BOS, 0, 2000);
            if (rd32(abar, HBA_BOHC) & BOHC_BB)
                wait_bits(abar, HBA_BOHC, BOHC_BB, 0, 2000);
        }
    }

    /* AHCI enable, прерывания выключены. */
    wr32(abar, HBA_GHC, rd32(abar, HBA_GHC) | GHC_AE);
    wr32(abar, HBA_GHC, rd32(abar, HBA_GHC) & ~GHC_IE);
    wr32(abar, HBA_IS, 0xFFFFFFFFu);

    u32int np = (cap & 0x1Fu) + 1;
    u32int pi = rd32(abar, HBA_PI);
    if (pi == 0)
        pi = (np >= 32) ? 0xFFFFFFFFu : ((1u << np) - 1u);

    debug_puts("[AHCI] ports impl="); debug_putnum(np);
    debug_puts(" PI="); pcix_puthex(pi); debug_putc('\n');

    /* Остановить все порты; если какой-то не остановился -- HBA reset. */
    int need_reset = 0;
    for (u32int i = 0; i < 32; i++)
        if ((pi & (1u << i)) && !port_stop(abar, (int)i))
            need_reset = 1;

    if (need_reset) {
        debug_puts("[AHCI] port engine stuck -- HBA reset\n");
        wr32(abar, HBA_GHC, rd32(abar, HBA_GHC) | GHC_HR);
        if (!wait_bits(abar, HBA_GHC, GHC_HR, 0, 1000))
            debug_puts("[AHCI] HBA reset timeout\n");
        wr32(abar, HBA_GHC, rd32(abar, HBA_GHC) | GHC_AE);
        wr32(abar, HBA_GHC, rd32(abar, HBA_GHC) & ~GHC_IE);
        pcix_mdelay(100);
        pi = rd32(abar, HBA_PI);
        if (pi == 0)
            pi = (np >= 32) ? 0xFFFFFFFFu : ((1u << np) - 1u);
    }

    ahci_hba_t *h = &g_hba[g_hba_n++];
    h->abar = abar;
    h->cap  = cap;
    h->s64a = (cap & CAP_S64A) ? 1 : 0;

    int before = g_port_n;
    for (u32int i = 0; i < 32; i++)
        if (pi & (1u << i))
            init_port(h, (int)i);

    if (g_port_n == before)
        debug_puts("[AHCI] controller: no usable disk on any implemented port\n");

    return g_port_n - before;
}

/* ------------------------------------------------------------------ */
/*  Чтение / запись / flush                                            */
/* ------------------------------------------------------------------ */

int ahci_rw(int port_idx, uint64_t lba, uint32_t count, void *buf, int write)
{
    if (port_idx < 0 || port_idx >= g_port_n)
        return 0;

    ahci_port_t *pt = &g_port[port_idx];
    if (!pt->used)
        return 0;

    u8int *p = (u8int *)buf;
    u32int done = 0;

    while (done < count) {
        u32int chunk = count - done;
        if (chunk > pt->bounce_sectors)
            chunk = pt->bounce_sectors;

        u32int bytes = chunk * pt->lss;
        u8int fis[16];
        int ok = 0;

        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            memset(fis, 0, sizeof(fis));
            fis[0] = 0x27;
            fis[1] = 0x80;
            if (pt->lba48) {
                fis[2]  = write ? 0x35u : 0x25u;   /* (WRITE|READ) DMA EXT */
                fis[7]  = 0x40;
                fis[8]  = (u8int)(lba >> 24);
                fis[9]  = (u8int)(lba >> 32);
                fis[10] = (u8int)(lba >> 40);
            } else {
                if (lba > 0x0FFFFFFFull)
                    return 0;
                fis[2]  = write ? 0xCAu : 0xC8u;   /* (WRITE|READ) DMA */
                fis[7]  = (u8int)(0x40u | ((lba >> 24) & 0x0F));
            }
            fis[4]  = (u8int)(lba);
            fis[5]  = (u8int)(lba >> 8);
            fis[6]  = (u8int)(lba >> 16);
            fis[12] = (u8int)(chunk & 0xFF);
            fis[13] = (u8int)((chunk >> 8) & 0xFF);

            if (write)
                memcpy(pt->bounce, p + (u64int)done * pt->lss, bytes);

            ok = issue_command(pt, fis, write, pt->bounce_phys, bytes, 10000);
        }
        if (!ok)
            return 0;

        if (!write)
            memcpy(p + (u64int)done * pt->lss, pt->bounce, bytes);

        done += chunk;
        lba  += chunk;
    }
    return 1;
}

int ahci_flush(int port_idx)
{
    if (port_idx < 0 || port_idx >= g_port_n || !g_port[port_idx].used)
        return 0;

    ahci_port_t *pt = &g_port[port_idx];
    u8int fis[16];
    memset(fis, 0, sizeof(fis));
    fis[0] = 0x27; fis[1] = 0x80;
    fis[2] = pt->lba48 ? 0xEA : 0xE7;     /* FLUSH CACHE (EXT) */
    return issue_command(pt, fis, 0, 0, 0, 30000);
}
