/*
 * ============================================================
 * drivers/blkdev.h -- унифицированный слой блочных устройств.
 * ============================================================
 *
 * Собирает воедино все найденные при загрузке накопители:
 *
 *   - legacy / native-mode IDE (PIO), master+slave на каждом
 *     канале, LBA28 и LBA48;
 *   - AHCI-контроллеры (SATA), любое их число и любое место
 *     в иерархии PCI;
 *   - NVMe-контроллеры (PCIe SSD), активные namespace-ы.
 *
 * Каждый найденный накопитель получает индекс 0..BLKDEV_MAX-1.
 *
 * ВАЖНО -- единицы адресации.  blkdev_read()/blkdev_write() и
 * blkdev_sectors() всегда оперируют 512-байтными секторами,
 * независимо от того, чем диск является на самом деле.  Диски с
 * логическим сектором 4096 (4Kn SATA/NVMe) слой эмулирует сам:
 * неполные сектора обслуживаются через read-modify-write.
 * Реальный размер сектора доступен через blkdev_lss() -- он нужен
 * только таблицам разделов (GPT хранит LBA в логических секторах
 * диска).
 *
 * Слой polling-овый (без IRQ).
 */

#ifndef BLKDEV_H
#define BLKDEV_H

#include "../lib/common.h"
#include "pcix.h"

#define BLKDEV_MAX 16

#define BLKDEV_NONE 0
#define BLKDEV_ATA  1   /* IDE (PIO), LBA28/LBA48            */
#define BLKDEV_AHCI 2   /* SATA через AHCI HBA               */
#define BLKDEV_NVME 3   /* NVMe namespace                    */

typedef struct {
    int      present;
    int      type;          /* BLKDEV_*                              */
    uint64_t sectors;       /* ёмкость в 512-байтных секторах        */
    uint32_t lss;           /* родной логический сектор: 512..4096   */
    uint64_t native_sectors;/* ёмкость в родных секторах             */
    char     name[20];      /* "ata0m", "sata0", "nvme0n1", ...      */
    char     model[41];     /* модель из IDENTIFY ("" если нет)      */

    /* ATA */
    uint16_t io;            /* база команд канала                    */
    uint16_t ctrl;          /* база управления                       */
    int      is_slave;      /* 1 = slave                             */
    int      lba48;         /* диск поддерживает LBA48               */

    /* AHCI: индекс в таблице портов ahci.c;
     * NVMe: индекс в таблице namespace-ов nvme.c */
    int      port;
} blkdev_t;

/*
 * PCI-обход + инициализация ATA/AHCI/NVMe.  Безопасно вызывать на
 * любой машине: отсутствие контроллеров оставляет список пустым.
 * Возвращает число дисков.
 */
int blkdev_init(void);

/* Число найденных накопителей. */
int blkdev_count(void);

/* Указатель на дескриптор (0 вне диапазона). */
const blkdev_t *blkdev_get(int idx);

/* Чтение/запись count 512-байтных секторов.  1 = успех. */
int blkdev_read(int idx, uint64_t lba, uint32_t count, void *buf);
int blkdev_write(int idx, uint64_t lba, uint32_t count, const void *buf);

/* Ёмкость диска в 512-байтных секторах (0 вне диапазона). */
uint64_t blkdev_sectors(int idx);

/* Родной логический сектор диска в байтах (512 вне диапазона). */
uint32_t blkdev_lss(int idx);

/*
 * Сброс кэша записи на носитель.  blkdev_write() только помечает диск
 * "грязным"; blkdev_flush() выдаёт FLUSH CACHE (ATA/AHCI) или Flush
 * (NVMe).  Вызывается ФС-слоем один раз на VFS-операцию записи, а не
 * на каждый сектор.  1 = успех (или сбрасывать нечего).
 */
int blkdev_flush(int idx);

/*
 * Разбор ATA IDENTIFY DEVICE (256 слов) -- общий для IDE и AHCI.
 * Возвращает 0, если устройство не годится (ATAPI, ёмкость 0 и т.п.).
 */
int blkdev_ata_parse_identify(const u16int *id, uint64_t *native_sectors,
                              uint32_t *lss, int *lba48, char model[41]);

/* Краткий отчёт (контроллеры + диски) -- по строке на emit(). */
void blkdev_report(void (*emit)(const char *line));

/* Добавить устройство (драйверы вызывают при регистрации).
 * Возвращает индекс или -1.  proto->name/model уже заполнены. */
int blkdev_add(const blkdev_t *proto);

/* ---------------- драйверы, реализованные отдельно ---------------- */

/* AHCI (drivers/ahci.c).  LBA/count -- в РОДНЫХ секторах порта. */
int  ahci_init_controller(const pcix_dev_t *d);
int  ahci_rw(int port_idx, uint64_t lba, uint32_t count, void *buf, int write);
int  ahci_flush(int port_idx);

/* NVMe (drivers/nvme.c).  LBA/count -- в РОДНЫХ секторах namespace. */
int  nvme_init_controller(const pcix_dev_t *d);
int  nvme_rw(int ns_idx, uint64_t lba, uint32_t count, void *buf, int write);
int  nvme_flush(int ns_idx);

#endif /* BLKDEV_H */
