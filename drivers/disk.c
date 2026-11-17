#include "../lib/common.h"
#include "../drivers/disk.h"
#include "../drivers/monitor.h"  // для вывода отладочной информации

/*
 * =====================================================================
 *  ПОРТАТИВНЫЙ ATA PIO ДРАЙВЕР (r12-bootfix, r13-storage: мультиканал)
 * =====================================================================
 *
 *  Что было не так в старой версии (почему ОС зависала на других ПК):
 *
 *  1. ata_wait_bsy() -- БЕСКОНЕЧНЫЙ цикл `while (inb(STATUS) & BSY);`.
 *     На машинах, где legacy-IDE канал 0x1F0 отсутствует (SATA в режиме
 *     AHCI без Compatibility-режима, чистые NVMe-системы, свежие
 *     чипсеты), чтение порта статуса возвращает 0xFF ("floating bus").
 *     0xFF & ATA_SR_BSY == 0x80 --> драйвер крутился в цикле вечно.
 *     Ядро застывало сразу после "Initializing ATA disk" -- на экране
 *     оставалось консольное окно отладки.  На ASUS P553M контроллер
 *     работал в legacy/IDE-режиме, поэтому там всё выглядело рабочим.
 *
 *  2. ata_wait_drq() -- тоже вечный цикл при статусе без ERR и без DRQ.
 *
 *  3. После IDENTIFY контроллер мог поднять IRQ14 ещё до ремапа PIC:
 *     BIOS маппит IRQ14 на вектор 0x0E -- это Page Fault в IDT ядра.
 *     Теперь nIEN (бит 1 регистра управления 0x3F6) отключает прерывания
 *     канала на всё время работы polling-драйвера.
 *
 *  Теперь: все ожидания ограничены таймаутами, шина детектируется
 *  (0xFF/0x00), при отсутствии диска всё безопасно пропускается --
 *  загрузка продолжается, disk0 просто не монтируется.
 *
 *  r13-storage:
 *  - поддерживаются ОБА legacy-канала (0x1F0 primary, 0x170 secondary)
 *    с master и slave: до 4 накопителей, не только 0x1F0/master;
 *  - LBA48 (READ/WRITE SECTORS EXT, 0x24/0x34) для обращения к секторам
 *    выше 128 ГиБ, если диск его поддерживает (IDENTIFY word 83 bit 10,
 *    размер по words 100..103);
 *  - добавлен API ata_dev_present/ata_dev_sectors/ata_read_dev/
 *    ata_write_dev для слоя ixk_storage (fs/ix). Старый API сохранён:
 *    hd_read_sector() = ata_read_dev(0, ..., 1, ...);
 *    ata_drive_present() = ata_dev_present(0).
 * =====================================================================
 */

#define ATA_DEV_MAX 4

/* Каналы legacy ATA.  RMK: на чисто-AHCI машинах эти порты могут отдавать
 * 0xFF -- каналы тогда просто не поднимаются. */
typedef struct {
    u16int base;      /* основной блок регистров (0x1F0 / 0x170) */
    u16int ctrl;      /* блок управления  (0x3F6 / 0x376)        */
    u8int  ready;     /* 1 = каналу ключали nIEN                 */
} ata_chan_t;

static ata_chan_t chans[2] = {
    { 0x1F0, 0x3F6, 0 },
    { 0x170, 0x376, 0 },
};

/* Есть ли реально диск: [chan][drv 0=master 1=slave]. */
static int ata_present_tbl[2][2];

/* Геометрия по IDENTIFY. */
static u64int ata_sectors_tbl[2][2];   /* всего секторов (LBA48 или LBA28) */
static int    ata_lba48_tbl[2][2];

/* Старый флаг "диск найден на 0x1F0 master" -- для совместимости. */
#define ata_present (ata_present_tbl[0][0])

int ata_drive_present(void)
{
    return ata_dev_present(0);
}

int ata_dev_present(int dev)
{
    if (dev < 0 || dev >= ATA_DEV_MAX)
        return 0;
    return ata_present_tbl[dev >> 1][dev & 1];
}

u64int ata_dev_sectors(int dev)
{
    if (dev < 0 || dev >= ATA_DEV_MAX)
        return 0;
    return ata_sectors_tbl[dev >> 1][dev & 1];
}

/* 400 нс задержка -- 4 чтения регистра статуса (стандарт ATA). */
static void ata_delay_chan(ata_chan_t *c)
{
    inb(c->base + 7);
    inb(c->base + 7);
    inb(c->base + 7);
    inb(c->base + 7);
}

/*
 * Ждать сброса BSY на заданном канале.  Возвращает 0 при успехе, -1 при
 * таймауте или если шина "висит в воздухе" (0xFF) / мертва (0x00).
 */
static int chan_wait_bsy(ata_chan_t *c)
{
    /*
     * Таймаут: 300 тыс. чтений статуса.  На реальном железе это
     * порядка 0.2-0.5 с -- с запасом больше, чем нужно исправному PIO
     * (миллисекунды), но отсутствующий/зависший диск больше не
     * задерживает загрузку на десятки секунд.
     */
    unsigned int timeout = 300000;

    while (timeout--) {
        unsigned char status = inb(c->base + 7);

        /* Floating bus (нет канала/устройства) или мёртвый порт. */
        if (status == 0xFF || status == 0x00)
            return -1;

        if (!(status & ATA_SR_BSY))
            return 0;

        /* Раз в 400 нс даём контроллеру передышку. */
        if ((timeout & 0x3FF) == 0)
            ata_delay_chan(c);
    }

    return -1;
}

static int chan_wait_drq(ata_chan_t *c)
{
    unsigned int timeout = 300000;

    while (timeout--) {
        unsigned char status = inb(c->base + 7);

        if (status == 0xFF || status == 0x00)
            return -1;

        if (status & ATA_SR_ERR)
            return -1;

        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ))
            return 0;

        if ((timeout & 0x3FF) == 0)
            ata_delay_chan(c);
    }

    return -1;
}

/* Совместимость со старыми вызовами (первичный канал master). */
int ata_wait_bsy(void) { return chan_wait_bsy(&chans[0]); }
int ata_wait_drq(void) { return chan_wait_drq(&chans[0]); }

void hd_busy(void)
{
    if (ata_wait_bsy() < 0)
        debug_puts("HDD busy timeout!\n");
}

/*
 * IDENTIFY DEVICE на (канал, устройство).
 * Возвращает 1, если найден полноценный ATA-диск (не ATAPI/SATA-комп).
 * Заполняет ata_present_tbl / ata_sectors_tbl / ata_lba48_tbl.
 */
static int ata_identify(ata_chan_t *c, int drv)
{
    unsigned char status;
    u8int ident[512];

    /* Выбрать устройство и выключить прерывания канала. */
    outb(c->base + 6, drv ? 0xB0 : 0xA0);
    ata_delay_chan(c);
    outb(c->ctrl, 0x02);        /* nIEN = 1: IRQ14/IRQ15 выключены */
    c->ready = 1;
    ata_delay_chan(c);

    /* Шина есть вообще?  Нет контроллера -> чтения дают 0xFF. */
    status = inb(c->base + 7);
    if (status == 0xFF)
        return 0;

    /* Стандартная последовательность IDENTIFY DEVICE. */
    outb(c->base + 2, 0);   /* sector count */
    outb(c->base + 3, 0);   /* LBA low      */
    outb(c->base + 4, 0);   /* LBA mid      */
    outb(c->base + 5, 0);   /* LBA high     */

    outb(c->base + 7, 0xEC);
    ata_delay_chan(c);

    status = inb(c->base + 7);

    /* 0x00 -- устройства с таким адресом нет; 0xFF -- шина в воздухе. */
    if (status == 0x00 || status == 0xFF)
        return 0;

    if (chan_wait_bsy(c) < 0)
        return 0;

    /*
     * Не-ATA устройства сигнализируют кодом в LBA_MID/LBA_HIGH:
     *   0x14/0xEB -- ATAPI (CD-ROM)
     *   0x3C/0xC3 -- SATA
     *   0x69/0x96 -- SATAPI
     * Legacy-PIO драйвер умеет только обычный ATA.
     */
    {
        unsigned char mid  = inb(c->base + 4);
        unsigned char high = inb(c->base + 5);

        if (mid == 0x14 && high == 0xEB)
            return 0;      /* ATAPI (CD) -- пропускаем */
        if ((mid == 0x3C && high == 0xC3) ||
            (mid == 0x69 && high == 0x96))
            return 0;      /* SATA не в IDE-режиме -- пропускаем */
    }

    if (chan_wait_drq(c) < 0)
        return 0;

    /* Прочитать 512 байт идентификационных данных. */
    for (int i = 0; i < 256; i++) {
        u16int w = inw(c->base);
        ident[i * 2]     = (u8int)(w & 0xFF);
        ident[i * 2 + 1] = (u8int)(w >> 8);
    }

    ata_delay_chan(c);

    ata_present_tbl[c == &chans[1] ? 1 : 0][drv] = 1;

    /*
     * Число секторов: LBA28 в words 60/61, ext в words 100..103.
     * LBA48 доступен, если word 83 высвечивает бит 10 (при word 83 != 0)
     * и word 83 bit 15 == 0 (иначе vendor-специфика).
     */
    u32int lba28 = ((u32int)ident[120] | ((u32int)ident[121] << 8) |
                    ((u32int)ident[122] << 16) | ((u32int)ident[123] << 24));
    u16int w83   = (u16int)(ident[166] | ((u16int)ident[167] << 8));
    int    has48 = (w83 != 0 && w83 != 0xFFFF &&
                    (w83 & 0x0400) && !(w83 & 0x8000));
    u64int lba48 = 0;
    if (has48) {
        lba48 = (u64int)ident[200]
              | ((u64int)ident[201] << 8)
              | ((u64int)ident[202] << 16)
              | ((u64int)ident[203] << 24)
              | ((u64int)ident[204] << 32)
              | ((u64int)ident[205] << 40)
              | ((u64int)ident[206] << 48)
              | ((u64int)ident[207] << 56);
    }

    ata_lba48_tbl[c == &chans[1] ? 1 : 0][drv] = (has48 && lba48) ? 1 : 0;
    ata_sectors_tbl[c == &chans[1] ? 1 : 0][drv] =
        (has48 && lba48) ? lba48 : (u64int)lba28;

    return 1;
}

/*
 * Инициализация ОБОИХ legacy-каналов (master и slave, т.е. до 4 дисков).
 * Безопасно вызывать на ЛЮБОЙ машине: если legacy-канала или диска нет,
 * функция быстро вернётся с нулевым числом устройств и ядро продолжит
 * загрузку без disk0.  Повторные вызовы -- no-op (идемпотентно).
 */
static int ata_inited = 0;

void hd_initialize(void)
{
    if (ata_inited)
        return;
    ata_inited = 1;

    debug_puts("Initializing ATA drives...\n");

    int found = 0;
    for (int ci = 0; ci < 2; ci++) {
        ata_chan_t *c = &chans[ci];

        /* Нет шины вовсе (floating 0xFF) -- канал пропускаем целиком,
         * но nIEN всё равно выставим, чтобы не ловить случайные IRQ. */
        if (inb(c->base + 7) == 0xFF) {
            debug_puts("ATA:");
            debug_puts(ci ? " secondary" : " primary");
            debug_puts(" channel absent (floating bus)\n");
            continue;
        }

        for (int drv = 0; drv < 2; drv++) {
            int ok = ata_identify(c, drv);
            if (!ok)
                continue;
            found++;
            int di = ci * 2 + drv;
            debug_puts("ATA: device ");
            debug_putnum((u64int)di);
            debug_puts(" present, ");
            debug_putnum(ata_sectors_tbl[ci][drv]);
            debug_puts(" sectors");
            if (ata_lba48_tbl[ci][drv])
                debug_puts(" (LBA48)");
            debug_puts("\n");
        }
    }

    if (!found)
        debug_puts("ATA: no legacy PIO drives (AHCI-only/NVMe system?)\n");
}

static ata_chan_t *ata_dev_chan(int dev)
{
    return &chans[dev >> 1];
}

static int ata_dev_drv(int dev)
{
    return dev & 1;
}

/*
 * Чтение одного сектора с указанного устройства.
 * Aдресация: LBA28 для секторов ниже 128 ГиБ; выше -- LBA48
 * (READ SECTORS EXT 0x24), если диск его поддерживает.
 * Возвращает 1 при успехе, 0 при ошибке/отсутствии диска.
 */
static int ata_read_one(ata_chan_t *c, int drv, u64int lba, u8int *buffer)
{
    /* Сначала выбрать устройство: после probe отсутствующего slave
     * канал остаётся переключён на него, и статус читается как 0x00
     * ("порта нет").  Раньше ожидание BSY шло до выбора диска --
     * из-за этого чтения с master падали. */
    outb(c->base + 6, (u8int)(0xA0 | (drv ? 0x10 : 0)));
    ata_delay_chan(c);

    if (chan_wait_bsy(c) < 0)
        return 0;

    if (!ata_lba48_tbl[c == &chans[1] ? 1 : 0][drv] || lba <= 0x0FFFFFFFull) {
        if (lba > 0x0FFFFFFFull)
            return 0;

        outb(c->base + 6, (u8int)(0xE0 | (drv ? 0x10 : 0) | ((lba >> 24) & 0x0F)));
        ata_delay_chan(c);
        outb(c->base + 2, 1);
        outb(c->base + 3, (u8int)(lba & 0xFF));
        outb(c->base + 4, (u8int)((lba >> 8) & 0xFF));
        outb(c->base + 5, (u8int)((lba >> 16) & 0xFF));
        outb(c->base + 7, 0x20);        /* READ SECTORS */
    } else {
        /* LBA48: пары "low/high" определяются двойной записью. */
        outb(c->base + 6, (u8int)(0xE0 | (drv ? 0x10 : 0)));
        ata_delay_chan(c);
        outb(c->base + 2, 0);
        outb(c->base + 2, 1);
        outb(c->base + 3, (u8int)((lba >> 0)  & 0xFF));
        outb(c->base + 3, (u8int)((lba >> 24) & 0xFF));
        outb(c->base + 4, (u8int)((lba >> 8)  & 0xFF));
        outb(c->base + 4, (u8int)((lba >> 32) & 0xFF));
        outb(c->base + 5, (u8int)((lba >> 16) & 0xFF));
        outb(c->base + 5, (u8int)((lba >> 40) & 0xFF));
        outb(c->base + 7, 0x24);        /* READ SECTORS EXT */
    }

    if (chan_wait_bsy(c) < 0)
        return 0;

    if (chan_wait_drq(c) < 0)
        return 0;

    for (int i = 0; i < 256; i++) {
        u16int data = inw(c->base);
        buffer[i * 2]     = (u8int)(data & 0xFF);
        buffer[i * 2 + 1] = (u8int)(data >> 8);
    }

    /* 400 нс перед следующей командой. */
    ata_delay_chan(c);

    return 1;
}

/*
 * Чтение нескольких секторов (по одному за команду) с устройства dev:
 *   dev 0 = primary master, 1 = primary slave,
 *   dev 2 = secondary master, 3 = secondary slave.
 */
int ata_read_dev(int dev, u64int lba, u32int count, u8int *buffer)
{
    if (dev < 0 || dev >= ATA_DEV_MAX || !ata_dev_present(dev) ||
        !buffer || count == 0)
        return 0;

    ata_chan_t *c = ata_dev_chan(dev);
    int drv = ata_dev_drv(dev);

    u64int end = lba + (u64int)count;
    if (end > ata_sectors_tbl[dev >> 1][dev & 1])
        return 0;                            /* за краем диска */

    u8int *dst = buffer;
    for (u32int i = 0; i < count; i++)
        if (!ata_read_one(c, drv, lba + i, dst + (u64int)i * 512))
            return 0;

    return 1;
}

/* Старый API: сектор с primary master. */
int hd_read_sector(u32int lba, u8int *buffer)
{
    if (!ata_dev_present(0))
        return 0;
    return ata_read_dev(0, (u64int)lba, 1, buffer);
}

/*
 * Запись одного сектора.  LBA28/LBA48 аналогично чтению.
 * Файловые системы пока read-only, функция используется напрямую
 * только отладкой.
 */
static int ata_write_one(ata_chan_t *c, int drv, u64int lba, const u8int *buffer)
{
    /* См. ata_read_one(): выбрать устройство до ожидания BSY. */
    outb(c->base + 6, (u8int)(0xA0 | (drv ? 0x10 : 0)));
    ata_delay_chan(c);

    if (chan_wait_bsy(c) < 0)
        return 0;

    if (!ata_lba48_tbl[c == &chans[1] ? 1 : 0][drv] || lba <= 0x0FFFFFFFull) {
        if (lba > 0x0FFFFFFFull)
            return 0;

        outb(c->base + 6, (u8int)(0xE0 | (drv ? 0x10 : 0) | ((lba >> 24) & 0x0F)));
        ata_delay_chan(c);
        outb(c->base + 2, 1);
        outb(c->base + 3, (u8int)(lba & 0xFF));
        outb(c->base + 4, (u8int)((lba >> 8) & 0xFF));
        outb(c->base + 5, (u8int)((lba >> 16) & 0xFF));
        outb(c->base + 7, 0x30);        /* WRITE SECTORS */
    } else {
        outb(c->base + 6, (u8int)(0xE0 | (drv ? 0x10 : 0)));
        ata_delay_chan(c);
        outb(c->base + 2, 0);
        outb(c->base + 2, 1);
        outb(c->base + 3, (u8int)((lba >> 0)  & 0xFF));
        outb(c->base + 3, (u8int)((lba >> 24) & 0xFF));
        outb(c->base + 4, (u8int)((lba >> 8)  & 0xFF));
        outb(c->base + 4, (u8int)((lba >> 32) & 0xFF));
        outb(c->base + 5, (u8int)((lba >> 16) & 0xFF));
        outb(c->base + 5, (u8int)((lba >> 40) & 0xFF));
        outb(c->base + 7, 0x34);        /* WRITE SECTORS EXT */
    }

    if (chan_wait_bsy(c) < 0)
        return 0;

    if (chan_wait_drq(c) < 0)
        return 0;

    const u16int *data = (const u16int *)buffer;
    for (int i = 0; i < 256; i++)
        outw(c->base, data[i]);

    /* Дождаться, пока контроллер выгрузит буфер. */
    if (chan_wait_bsy(c) < 0)
        return 0;

    ata_delay_chan(c);

    return 1;
}

int ata_write_dev(int dev, u64int lba, u32int count, u8int *buffer)
{
    if (dev < 0 || dev >= ATA_DEV_MAX || !ata_dev_present(dev) ||
        !buffer || count == 0)
        return 0;

    ata_chan_t *c = ata_dev_chan(dev);
    int drv = ata_dev_drv(dev);

    u64int end = lba + (u64int)count;
    if (end > ata_sectors_tbl[dev >> 1][dev & 1])
        return 0;

    for (u32int i = 0; i < count; i++)
        if (!ata_write_one(c, drv, lba + i, buffer + (u64int)i * 512))
            return 0;

    return 1;
}

/* Старый API: сектор на primary master. */
int hd_write_sector(u32int lba, u8int *buffer)
{
    if (!ata_dev_present(0))
        return 0;
    return ata_write_dev(0, (u64int)lba, 1, buffer);
}
