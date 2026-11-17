/*
 * ============================================================
 * drivers/blkdev.c -- унифицированный слой блочных устройств.
 * ============================================================
 *
 * Порядок инициализации (blkdev_init):
 *
 *   1. pcix_enum(): обходим ВЕСЬ PCI (все шины за мостами, все
 *      функции) и собираем контроллеры накопителей:
 *        - 0x0106 (SATA/AHCI) и Intel RAID-режим 0x0104 -> AHCI;
 *        - 0x0108/prog_if 0x02 -> NVMe;
 *        - 0x0101 (IDE) -> каналы: legacy-порты (compat mode) либо
 *          BAR0..3 (native mode).
 *   2. IDE: IDENTIFY на master/slave каждого канала.
 *   3. AHCI и NVMe: каждый контроллер инициализируется своим
 *      драйвером и регистрирует диски через blkdev_add().
 *   4. Если PCI вообще ничего не показал (экзотика / сломанный
 *      конфиг-доступ) -- запасной опрос legacy-портов 0x1F0/0x170.
 *
 * Все ожидания -- с таймаутами; отсутствие дисков на любом этапе
 * безопасно.
 */

#include "blkdev.h"
#include "ahci.h"
#include "disk.h"
#include "pcix.h"
#include "../lib/common.h"

/* ------------------------------------------------------------------ */
/*  Состояние                                                          */
/* ------------------------------------------------------------------ */

static blkdev_t g_dev[BLKDEV_MAX];
static u8int    g_dirty[BLKDEV_MAX];
static int      g_dev_n  = 0;
static int      g_inited = 0;

int blkdev_count(void) { return g_dev_n; }

const blkdev_t *blkdev_get(int idx)
{
    if (idx < 0 || idx >= g_dev_n) return 0;
    return &g_dev[idx];
}

uint64_t blkdev_sectors(int idx)
{
    if (idx < 0 || idx >= g_dev_n) return 0;
    return g_dev[idx].sectors;
}

uint32_t blkdev_lss(int idx)
{
    if (idx < 0 || idx >= g_dev_n) return 512;
    return g_dev[idx].lss ? g_dev[idx].lss : 512;
}

int blkdev_add(const blkdev_t *proto)
{
    if (!proto || g_dev_n >= BLKDEV_MAX)
        return -1;

    blkdev_t *d = &g_dev[g_dev_n];
    memcpy((u8int *)d, (const u8int *)proto, sizeof(*d));

    if (d->lss != 512 && d->lss != 1024 && d->lss != 2048 && d->lss != 4096)
        d->lss = 512;
    d->present = 1;
    d->sectors = d->native_sectors * (uint64_t)(d->lss / 512u);

    debug_puts("[BLKDEV] registered ");
    debug_puts(d->name);
    if (d->model[0]) { debug_puts(" \""); debug_puts(d->model); debug_puts("\""); }
    debug_puts(" (");
    debug_putnum(d->sectors / 2048u);
    debug_puts(" MiB, sector ");
    debug_putnum(d->lss);
    debug_puts(")\n");

    return g_dev_n++;
}

static void name_num(char *dst, const char *prefix, int n)
{
    char tmp[12];
    int  k = 0;
    while (*prefix) *dst++ = *prefix++;
    if (n == 0) tmp[k++] = '0';
    while (n > 0) { tmp[k++] = (char)('0' + n % 10); n /= 10; }
    while (k) *dst++ = tmp[--k];
    *dst = 0;
}

/* ------------------------------------------------------------------ */
/*  ATA IDENTIFY (общий для IDE и AHCI)                                */
/* ------------------------------------------------------------------ */

int blkdev_ata_parse_identify(const u16int *id, uint64_t *native_sectors,
                              uint32_t *lss, int *lba48, char model[41])
{
    /* Слово 0 бит 15 -- ATAPI/не-ATA. */
    if (id[0] & 0x8000u)
        return 0;

    int l48 = (id[83] & (1u << 10)) != 0 && (id[83] & 0xC000u) == 0x4000u;

    uint64_t sectors;
    if (l48)
        sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                  ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    else
        sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    if (sectors == 0)
        return 0;

    /*
     * Логический сектор: слово 106 валидно при бите 14=1 и 15=0;
     * бит 12 -- логический сектор больше 512, размер в СЛОВАХ
     * в 117..118.  Без этого 4Kn-диски читались бы неверно.
     */
    uint32_t ss = 512;
    u16int   w106 = id[106];
    if ((w106 & 0xC000u) == 0x4000u && (w106 & (1u << 12))) {
        uint32_t words = (uint32_t)id[117] | ((uint32_t)id[118] << 16);
        if (words >= 256 && words <= 2048)
            ss = words * 2u;
    }
    if (ss != 512 && ss != 1024 && ss != 2048 && ss != 4096)
        ss = 512;

    /* Модель: слова 27..46, байты в слове переставлены. */
    int k = 0;
    for (int i = 27; i <= 46; i++) {
        model[k++] = (char)(id[i] >> 8);
        model[k++] = (char)(id[i] & 0xFF);
    }
    model[k] = 0;
    while (k > 0 && (model[k - 1] == ' ' || model[k - 1] == 0))
        model[--k] = 0;
    for (int i = 0; i < k; i++)
        if ((unsigned char)model[i] < 0x20 || (unsigned char)model[i] > 0x7E)
            model[i] = '?';

    *native_sectors = sectors;
    *lss   = ss;
    *lba48 = l48;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  ATA (PIO): регистры                                                */
/* ------------------------------------------------------------------ */

#define A_DATA  0
#define A_ERR   1
#define A_SECC  2
#define A_LBA0  3
#define A_LBA1  4
#define A_LBA2  5
#define A_DRV   6
#define A_CMD   7

#define SR_BSY 0x80
#define SR_DRQ 0x08
#define SR_ERR 0x01
#define SR_DF  0x20

static void bd_ata_delay(uint16_t io)
{
    inb(io + A_CMD); inb(io + A_CMD); inb(io + A_CMD); inb(io + A_CMD);
}

/*
 * Ждать BSY=0.  -1: таймаут/нет устройства.
 * Таймаут по времени (~10 с): HDD после standby раскручивается до 8-10 с,
 * а счёт по числу inb() зависит от скорости порта на конкретном чипсете.
 */
static int bd_ata_wait_bsy(uint16_t io)
{
    unsigned int t = 10000;                 /* x ~1 мс */
    while (t--) {
        for (int k = 0; k < 20; k++) {      /* ~20 опросов на мс */
            unsigned char s = inb(io + A_CMD);
            /* 0xFF -- ���� � �������, 0x00 -- ������ ����; 0x50 (DRDY) �����. */
            if (s == 0xFF || s == 0x00) return -1;
            if (!(s & SR_BSY)) return 0;
            pcix_udelay(50);
        }
    }
    return -1;
}

static int bd_ata_wait_drq(uint16_t io)
{
    /* r14-diskfix: 0x50 -- ���������� ������ �� ������ ���������� �� BSY
     * (DRDY, DRQ ��� �� ������).  ������� ���� ������ ��� ������ ������
     * � ���� ������, ������� �� �������/���������� ������ IDENTIFY �
     * ������ ������ ��������� �������������. */
    unsigned int t = 10000;
    while (t--) {
        for (int k = 0; k < 20; k++) {
            unsigned char s = inb(io + A_CMD);
            if (s == 0xFF) return -1;
            if (s & (SR_ERR | SR_DF)) return -1;
            if (!(s & SR_BSY) && (s & SR_DRQ)) return 0;
            pcix_udelay(50);
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  ATA: IDENTIFY + регистрация                                        */
/* ------------------------------------------------------------------ */

static int g_ata_chan_name = 0;   /* сквозной номер канала для имени */

static void ata_note(int chan, int slave, uint16_t io, const char *why, unsigned st)
{
    debug_puts("[ATA] ch"); debug_putnum((uint64_t)chan);
    debug_puts(slave ? " slave" : " master");
    debug_puts(" io="); pcix_puthexw(io, 4);
    debug_puts(" st="); pcix_puthexw(st, 2);
    debug_puts(": "); debug_puts(why); debug_putc('\n');
}

static void bd_ata_identify(int chan_no, int slave, uint16_t io, uint16_t ctrl)
{
    /* r14-diskfix: IDENTIFY ��� LBA-���� (0xA0) �� ����� ������������
     * � USB-������ ���������� LBA28-��������� � ������� ����� �������.
     * � 0xE0 ���� �������� � ������ LBA (��� Lefebvre/OSDev). */
    outb(io + A_DRV, 0xE0 | (slave ? 0x10 : 0x00));
    bd_ata_delay(io);
    outb(ctrl, 0x02);               /* nIEN */
    bd_ata_delay(io);

    {
        unsigned st0 = inb(io + A_CMD);
        if (st0 == 0xFF) {          /* плавающая шина */
            ata_note(chan_no, slave, io, "no device (floating bus)", st0);
            return;
        }
    }

    outb(io + A_SECC, 0);
    outb(io + A_LBA0, 0);
    outb(io + A_LBA1, 0);
    outb(io + A_LBA2, 0);
    outb(io + A_CMD, 0xEC);         /* IDENTIFY DEVICE */

    bd_ata_delay(io);
    unsigned char st = inb(io + A_CMD);
    if (st == 0x00 || st == 0xFF) {
        ata_note(chan_no, slave, io, "no device (no response to IDENTIFY)", st);
        return;
    }

    if (bd_ata_wait_bsy(io) < 0) {
        ata_note(chan_no, slave, io, "BSY never cleared", inb(io + A_CMD));
        return;
    }

    {
        unsigned char mid  = inb(io + A_LBA1);
        unsigned char high = inb(io + A_LBA2);
        if (mid == 0x14 && high == 0xEB) { ata_note(chan_no, slave, io, "ATAPI (optical) -- skipped", st); return; }
        if (mid == 0x3C && high == 0xC3) { ata_note(chan_no, slave, io, "SEMB signature -- skipped", st); return; }
        if (mid == 0x69 && high == 0x96) { ata_note(chan_no, slave, io, "port multiplier signature -- skipped", st); return; }
    }

    if (bd_ata_wait_drq(io) < 0) {
        ata_note(chan_no, slave, io, "IDENTIFY: no DRQ (error)", inb(io + A_CMD));
        return;
    }

    u16int id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(io + A_DATA);
    bd_ata_delay(io);

    uint64_t sectors; uint32_t lss; int lba48; char model[41];
    if (!blkdev_ata_parse_identify(id, &sectors, &lss, &lba48, model)) {
        ata_note(chan_no, slave, io, "IDENTIFY data rejected (ATAPI / zero capacity)", st);
        return;
    }

    blkdev_t d;
    memset((u8int *)&d, 0, sizeof(d));
    d.type = BLKDEV_ATA;
    d.native_sectors = sectors;
    d.lss = lss;
    d.io = io; d.ctrl = ctrl; d.is_slave = slave; d.lba48 = lba48;
    name_num(d.name, "ata", chan_no);
    {
        int n = 0; while (d.name[n]) n++;
        d.name[n++] = slave ? 's' : 'm'; d.name[n] = 0;
    }
    for (int i = 0; i < 40 && model[i]; i++) d.model[i] = model[i];
    if (blkdev_add(&d) < 0)
        debug_puts("[ATA] device table full -- disk not registered\n");
}

/* ------------------------------------------------------------------ */
/*  ATA: чтение/запись (посекторно)                                    */
/* ------------------------------------------------------------------ */

static int bd_ata_sector_rw(const blkdev_t *d, uint64_t lba, u8int *buf, int write)
{
    if (!d->lba48 && lba > 0x0FFFFFFFull)
        return 0;

    if (d->lba48)
        outb(d->io + A_DRV, 0x40 | (d->is_slave ? 0x10 : 0x00));
    else
        outb(d->io + A_DRV, 0xE0 | (d->is_slave ? 0x10 : 0x00) |
                            (u8int)((lba >> 24) & 0x0F));
    bd_ata_delay(d->io);

    if (bd_ata_wait_bsy(d->io) < 0)
        return 0;

    if (d->lba48) {
        outb(d->io + A_SECC, 0);
        outb(d->io + A_LBA0, (u8int)(lba >> 24));
        outb(d->io + A_LBA1, (u8int)(lba >> 32));
        outb(d->io + A_LBA2, (u8int)(lba >> 40));
        outb(d->io + A_SECC, 1);
        outb(d->io + A_LBA0, (u8int)(lba));
        outb(d->io + A_LBA1, (u8int)(lba >> 8));
        outb(d->io + A_LBA2, (u8int)(lba >> 16));
        outb(d->io + A_CMD, write ? 0x34 : 0x24);
    } else {
        outb(d->io + A_SECC, 1);
        outb(d->io + A_LBA0, (u8int)(lba));
        outb(d->io + A_LBA1, (u8int)(lba >> 8));
        outb(d->io + A_LBA2, (u8int)(lba >> 16));
        /* LBA28: ������� 4 ���� LBA -- � ������� ������� �������� drive/head.
         * ������ ��� ���������� ������, � �� ���� 128 ��� ��������
         * ��� ������ ����� (����� ������� ��������). */
        outb(d->io + A_DRV, 0xE0 | (d->is_slave ? 0x10 : 0x00) |
                            (u8int)((lba >> 24) & 0x0F));
        outb(d->io + A_CMD, write ? 0x30 : 0x20);
    }

    bd_ata_delay(d->io);
    if (bd_ata_wait_bsy(d->io) < 0) return 0;
    if (bd_ata_wait_drq(d->io) < 0) return 0;

    uint32_t words = d->lss / 2u;

    if (write) {
        u16int *data = (u16int *)buf;
        for (uint32_t i = 0; i < words; i++)
            outw(d->io + A_DATA, data[i]);
        if (bd_ata_wait_bsy(d->io) < 0)
            return 0;
    } else {
        for (uint32_t i = 0; i < words; i++) {
            u16int w = inw(d->io + A_DATA);
            buf[i * 2]     = (u8int)(w & 0xFF);
            buf[i * 2 + 1] = (u8int)(w >> 8);
        }
    }

    bd_ata_delay(d->io);
    {
        unsigned char s = inb(d->io + A_CMD);
        if (s & (SR_ERR | SR_DF))
            return 0;
    }
    return 1;
}

static int bd_ata_rw(const blkdev_t *d, uint64_t lba, uint32_t count,
                  void *buf, int write)
{
    u8int *p = (u8int *)buf;

    if (!d->lba48 && (lba > 0x0FFFFFFFull || lba + count > 0x10000000ull))
        return 0;

    for (uint32_t i = 0; i < count; i++) {
        if (!bd_ata_sector_rw(d, lba + i, p + (uint64_t)i * d->lss, write))
            return 0;
    }
    return 1;
}

static int bd_ata_flush(const blkdev_t *d)
{
    if (bd_ata_wait_bsy(d->io) < 0)
        return 0;
    outb(d->io + A_DRV, (d->lba48 ? 0x40 : 0xE0) | (d->is_slave ? 0x10 : 0x00));
    outb(d->io + A_CMD, d->lba48 ? 0xEA : 0xE7);   /* FLUSH CACHE (EXT) */
    bd_ata_delay(d->io);
    /* Сброс кэша HDD может занимать секунды. */
    unsigned int t = 30000000u;
    while (t--) {
        unsigned char s = inb(d->io + A_CMD);
        if (s == 0xFF) return 0;
        if (!(s & SR_BSY))
            return (s & (SR_ERR | SR_DF)) ? 0 : 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Сбор контроллеров по PCI                                           */
/* ------------------------------------------------------------------ */

#define MAX_CTRL 8
#define MAX_CHAN 16

typedef struct { uint16_t io, ctrl; } ide_chan_t;

static pcix_dev_t g_ahci[MAX_CTRL];  static int g_ahci_n;
static pcix_dev_t g_nvme[MAX_CTRL];  static int g_nvme_n;
static ide_chan_t g_chan[MAX_CHAN];  static int g_chan_n;
static int        g_ide_ctrl_n;

static void add_channel(uint16_t io, uint16_t ctrl)
{
    if (!io || !ctrl) return;
    for (int i = 0; i < g_chan_n; i++)
        if (g_chan[i].io == io) return;
    if (g_chan_n < MAX_CHAN) {
        g_chan[g_chan_n].io = io;
        g_chan[g_chan_n].ctrl = ctrl;
        g_chan_n++;
    }
}

static void pci_log(const char *what, const pcix_dev_t *d)
{
    debug_puts("[BLKDEV] PCI: "); debug_puts(what); debug_puts(" at ");
    debug_putnum(d->bus); debug_putc(':');
    debug_putnum(d->dev); debug_putc('.');
    debug_putnum(d->fn);
    debug_puts(" ven="); pcix_puthex(d->vendor);
    debug_puts(" dev="); pcix_puthex(d->device);
    debug_putc('\n');
}

static int g_pci_total = 0;

/*
 * Похож ли 0x0104-контроллер на AHCI HBA: BAR5 назначен, не I/O,
 * и после включения MEM-space регистры CAP/GHC читаются осмысленно.
 * Проверка нужна, чтобы не отдать настоящему RAID-контроллеру
 * (без AHCI) регистры чужого формата.
 */
static int is_ahci_hba(const pcix_dev_t *d)
{
    u64int bar = 0;
    int is_io = 0;

    if (!pcix_bar(d, 5, &bar, &is_io, 0) || is_io || !bar)
        return 0;

    pcix_enable(d, PCIX_EN_MEM);

    {
        volatile u32int *hba = (volatile u32int *)pcix_map_mmio(bar, 0x100);
        u32int cap, ghc;

        if (!hba)
            return 0;

        cap = hba[0];
        ghc = hba[1];

        if (cap == 0xFFFFFFFFu || cap == 0)
            return 0;
        if (ghc == 0xFFFFFFFFu)
            return 0;

        /* CAP.NP (биты 4:0): 31 = все порты, для 0x0104-RAID неверно. */
        if ((cap & 0x1Fu) == 0x1Fu)
            return 0;
    }

    return 1;
}

static int storage_visit(const pcix_dev_t *d, void *ctx)
{
    (void)ctx;

    /* Полный список PCI -- нужен, чтобы по логу понять топологию. */
    g_pci_total++;
    debug_puts("[PCI] ");
    pcix_puthexw(d->bus, 2); debug_putc(':');
    pcix_puthexw(d->dev, 2); debug_putc('.');
    pcix_puthexw(d->fn, 1);  debug_putc(' ');
    pcix_puthexw(d->vendor, 4); debug_putc(':');
    pcix_puthexw(d->device, 4);
    debug_puts(" class ");
    pcix_puthexw(d->base_class, 2); debug_putc('.');
    pcix_puthexw(d->sub_class, 2);  debug_putc('.');
    pcix_puthexw(d->prog_if, 2);
    if (d->base_class == 0x06 && d->sub_class == 0x04 && d->hdr_type == 1) {
        debug_puts(" bridge -> bus ");
        pcix_puthexw((pcix_read32(d, 0x18) >> 8) & 0xFF, 2);
    }
    debug_putc('\n');

    if (d->base_class != 0x01)
        return 0;

    if (d->sub_class == 0x06 ||
        (d->sub_class == 0x04 && is_ahci_hba(d))) {
        /*
         * 0x0106 -- SATA в режиме AHCI.
         *
         * r14-vboxfix: 0x0104 (RAID) у части контроллеров -- тоже HBA
         * с ABAR в BAR5: Intel PCH, VirtualBox ICH9 (8086:2829). Раньше
         * 0x0104 брали только у Intel, поэтому SATA-контроллер
         * VirtualBox пропускался целиком и диск не находился. Теперь
         * решает наличие рабочего ABAR, а не вендор.
         */
        if (g_ahci_n < MAX_CTRL) {
            pci_log(d->sub_class == 0x06 ? "SATA/AHCI controller"
                                         : "RAID-mode SATA (AHCI HBA)", d);
            g_ahci[g_ahci_n++] = *d;
        }
    } else if (d->sub_class == 0x08 && d->prog_if == 0x02) {
        if (g_nvme_n < MAX_CTRL) {
            pci_log("NVMe controller", d);
            g_nvme[g_nvme_n++] = *d;
        }
    } else if (d->sub_class == 0x01) {
        pci_log("IDE controller", d);
        g_ide_ctrl_n++;

        pcix_enable(d, PCIX_EN_IO | PCIX_EN_BM);

        u64int b0 = 0, b1 = 0, b2 = 0, b3 = 0;
        int io0, io1, io2, io3;

        /* Primary: native (prog_if bit 0) -- BAR0/BAR1, иначе legacy. */
        if ((d->prog_if & 0x01) &&
            pcix_bar(d, 0, &b0, &io0, 0) && io0 &&
            pcix_bar(d, 1, &b1, &io1, 0) && io1)
            add_channel((uint16_t)b0, (uint16_t)(b1 + 2));
        else
            add_channel(0x1F0, 0x3F6);

        /* Secondary: bit 2 -- BAR2/BAR3. */
        if ((d->prog_if & 0x04) &&
            pcix_bar(d, 2, &b2, &io2, 0) && io2 &&
            pcix_bar(d, 3, &b3, &io3, 0) && io3)
            add_channel((uint16_t)b2, (uint16_t)(b3 + 2));
        else
            add_channel(0x170, 0x376);

        /*
         * Часть чипсетов объявляет native-mode, но реально отвечает по
         * legacy-портам (или наоборот).  Лишний опрос безвреден: пустая
         * шина читается как 0xFF, дубликаты отсекаются по io-порту.
         */
        add_channel(0x1F0, 0x3F6);
        add_channel(0x170, 0x376);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Точка входа                                                        */
/* ------------------------------------------------------------------ */

int blkdev_init(void)
{
    if (g_inited)
        return g_dev_n;
    g_inited = 1;

    g_dev_n = 0;
    memset((u8int *)g_dev, 0, sizeof(g_dev));
    memset((u8int *)g_dirty, 0, sizeof(g_dirty));
    g_ahci_n = g_nvme_n = g_chan_n = g_ide_ctrl_n = 0;

    debug_puts("[BLKDEV] probing storage devices\n");

    int nfn = pcix_enum(storage_visit, 0);
    debug_puts("[BLKDEV] PCI functions seen: ");
    debug_putnum((uint64_t)nfn);
    debug_putc('\n');

    /* Совсем без PCI-контроллеров -- запасной опрос стандартных портов. */
    if (g_chan_n == 0 && g_ahci_n == 0 && g_nvme_n == 0) {
        debug_puts("[BLKDEV] no storage controller on PCI -- legacy IDE ports\n");
        add_channel(0x1F0, 0x3F6);
        add_channel(0x170, 0x376);
    }

    /* 1. IDE-каналы. */
    for (int c = 0; c < g_chan_n; c++) {
        bd_ata_identify(g_ata_chan_name, 0, g_chan[c].io, g_chan[c].ctrl);
        bd_ata_identify(g_ata_chan_name, 1, g_chan[c].io, g_chan[c].ctrl);
        g_ata_chan_name++;
    }

    /* 2. AHCI. */
    for (int i = 0; i < g_ahci_n; i++)
        ahci_init_controller(&g_ahci[i]);

    /* 3. NVMe. */
    for (int i = 0; i < g_nvme_n; i++)
        nvme_init_controller(&g_nvme[i]);

    debug_puts("[BLKDEV] done, devices: ");
    debug_putnum((uint64_t)g_dev_n);
    debug_putc('\n');

    if (g_dev_n == 0 && (g_ahci_n || g_nvme_n || g_ide_ctrl_n))
        debug_puts("[BLKDEV] WARNING: storage controllers present but NO disk "
                   "was initialised -- see [AHCI]/[NVME]/[ATA] lines above\n");
    if (g_ahci_n == 0 && g_nvme_n == 0 && g_ide_ctrl_n == 0)
        debug_puts("[BLKDEV] WARNING: no SATA/NVMe/IDE controller found on PCI\n");

    return g_dev_n;
}

/* ------------------------------------------------------------------ */
/*  Диспетчер: адресация 512-байтными секторами                        */
/* ------------------------------------------------------------------ */

static int native_rw(const blkdev_t *d, uint64_t nlba, uint32_t ncount,
                     void *buf, int write)
{
    switch (d->type) {
        case BLKDEV_ATA:  return bd_ata_rw(d, nlba, ncount, buf, write);
        case BLKDEV_AHCI: return ahci_rw(d->port, nlba, ncount, buf, write);
        case BLKDEV_NVME: return nvme_rw(d->port, nlba, ncount, buf, write);
        default:          return 0;
    }
}

static u8int g_rmw[4096];

static int bd_rw(int idx, uint64_t lba, uint32_t count, void *buf, int write)
{
    if (idx < 0 || idx >= g_dev_n || !buf || count == 0)
        return 0;

    const blkdev_t *d = &g_dev[idx];

    if (lba + count < lba || lba + count > d->sectors)
        return 0;

    if (write)
        g_dirty[idx] = 1;

    if (d->lss == 512)
        return native_rw(d, lba, count, buf, write);

    /* Эмуляция 512-байтных секторов поверх 1K/2K/4K. */
    uint32_t ratio = d->lss / 512u;
    u8int   *p = (u8int *)buf;

    while (count) {
        uint64_t nlba = lba / ratio;
        uint32_t off  = (uint32_t)(lba % ratio);

        if (off == 0 && count >= ratio) {
            uint32_t nrun = count / ratio;
            if (!native_rw(d, nlba, nrun, p, write))
                return 0;
            uint32_t used = nrun * ratio;
            p += (uint64_t)used * 512u; lba += used; count -= used;
        } else {
            uint32_t n = ratio - off;
            if (n > count) n = count;

            if (!native_rw(d, nlba, 1, g_rmw, 0))
                return 0;
            if (write) {
                memcpy(g_rmw + off * 512u, p, n * 512u);
                if (!native_rw(d, nlba, 1, g_rmw, 1))
                    return 0;
            } else {
                memcpy(p, g_rmw + off * 512u, n * 512u);
            }
            p += (uint64_t)n * 512u; lba += n; count -= n;
        }
    }
    return 1;
}

static int g_io_err_logged = 0;

static void io_err(const char *op, int idx, uint64_t lba, uint32_t count)
{
    if (g_io_err_logged++ >= 16)
        return;
    debug_puts("[BLKDEV] I/O ERROR ");
    debug_puts(op);
    debug_putc(' ');
    if (idx >= 0 && idx < g_dev_n) debug_puts(g_dev[idx].name);
    debug_puts(" lba=");
    debug_putnum(lba);
    debug_puts(" count=");
    debug_putnum(count);
    debug_putc('\n');
}

int blkdev_read(int idx, uint64_t lba, uint32_t count, void *buf)
{
    int ok = bd_rw(idx, lba, count, buf, 0);
    if (!ok) io_err("read", idx, lba, count);
    return ok;
}

int blkdev_write(int idx, uint64_t lba, uint32_t count, const void *buf)
{
    int ok = bd_rw(idx, lba, count, (void *)buf, 1);
    if (!ok) io_err("write", idx, lba, count);
    return ok;
}

int blkdev_flush(int idx)
{
    if (idx < 0 || idx >= g_dev_n)
        return 0;
    if (!g_dirty[idx])
        return 1;

    const blkdev_t *d = &g_dev[idx];
    int ok = 0;
    switch (d->type) {
        case BLKDEV_ATA:  ok = bd_ata_flush(d);          break;
        case BLKDEV_AHCI: ok = ahci_flush(d->port);   break;
        case BLKDEV_NVME: ok = nvme_flush(d->port);   break;
        default: break;
    }
    if (ok)
        g_dirty[idx] = 0;
    else
        debug_puts("[BLKDEV] flush FAILED\n");
    return ok;
}

/* ------------------------------------------------------------------ */
/*  Краткий отчёт для экрана                                           */
/* ------------------------------------------------------------------ */

static void rep_num(char *dst, int *pos, uint64_t v)
{
    char t[24]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) dst[(*pos)++] = t[--k];
}

static void rep_str(char *dst, int *pos, const char *s)
{
    while (*s && *pos < 110) dst[(*pos)++] = *s++;
}

void blkdev_report(void (*emit)(const char *line))
{
    char ln[128];
    int p = 0;

    rep_str(ln, &p, "PCI: ");
    rep_num(ln, &p, (uint64_t)g_pci_total);
    rep_str(ln, &p, " fn; AHCI ");  rep_num(ln, &p, (uint64_t)g_ahci_n);
    rep_str(ln, &p, ", NVMe ");     rep_num(ln, &p, (uint64_t)g_nvme_n);
    rep_str(ln, &p, ", IDE ");      rep_num(ln, &p, (uint64_t)g_ide_ctrl_n);
    rep_str(ln, &p, "; disks ");    rep_num(ln, &p, (uint64_t)g_dev_n);
    ln[p] = 0; emit(ln);

    for (int i = 0; i < g_dev_n; i++) {
        p = 0;
        rep_str(ln, &p, g_dev[i].name);
        rep_str(ln, &p, " ");
        rep_num(ln, &p, g_dev[i].sectors / 2048u);
        rep_str(ln, &p, " MiB, sector ");
        rep_num(ln, &p, g_dev[i].lss);
        if (g_dev[i].model[0]) { rep_str(ln, &p, " "); rep_str(ln, &p, g_dev[i].model); }
        ln[p] = 0; emit(ln);
    }
}
