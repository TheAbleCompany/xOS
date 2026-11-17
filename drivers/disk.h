#define OUT_P_B(val, port) outb(port, val)
#define IN_P_B(port) inb(port)

// Если вам нужны версии с переменными:
#define OUT_P_B_VAL(val, port) outb(port, val)
#define IN_P_B_VAL(port) inb(port)

#define HD_DATA     0x1f0       /* регистр данных */
#define HD_ERROR    0x1f1       /* регистр ошибок */
#define HD_NSECTOR  0x1f2       /* регистр счетчика секторов */
#define HD_SECTOR   0x1f3       /* регистр стартового сектора */
#define HD_LCYL     0x1f4       /* регистр младшего байта номера цилиндра */
#define HD_HCYL     0x1f5       /* регистр старшего байта номера цилиндра */
#define HD_CURRENT  0x1f6       /* 101dhhhh , d=устройство, hhhh=головка */
#define HD_STATUS   0x1f7       /* регистр состояния/команд */
#define HD_COMMAND  0x1f7       /* регистр команд */


#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_SECCOUNT   0x1F2
#define ATA_LBA_LOW    0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HIGH   0x1F5
#define ATA_DRIVE      0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7
#define ATA_CONTROL    0x3F6

#define ATA_SR_BSY     0x80
#define ATA_SR_DRDY    0x40
#define ATA_SR_DRQ     0x08
#define ATA_SR_ERR     0x01

#include "../lib/common.h"

/* r12-bootfix: портативный ATA-стек.
 * Все ожидания ограничены таймаутами; при отсутствии legacy-канала
 * (AHCI/NVMe-системы) драйвер безопасно сообщает "диска нет" и
 * загрузка продолжается.
 * r13-storage: до 4 накопителей (2 legacy-канала x master/slave),
 * LBA48-адресация выше 128 ГиБ, мульти-секторный API для слоя storage. */
void hd_initialize(void);
int  hd_read_sector(u32int lba, u8int *buffer);   /* 1 = успех */
int  hd_write_sector(u32int lba, u8int *buffer);  /* 1 = успех */
int  ata_wait_bsy(void);       /* 0 = BSY снят, -1 = таймаут / нет шины */
int  ata_wait_drq(void);       /* 0 = DRQ готов, -1 = ошибка / таймаут */
int  ata_drive_present(void);  /* 1 = диск найден на 0x1F0 master */

/* r13-storage: устройства legacy ATA,
 *   dev 0 = primary master, 1 = primary slave,
 *   dev 2 = secondary master, 3 = secondary slave. */
#define ATA_DEV_MAX 4

int     ata_dev_present(int dev);            /* 1 = есть диск         */
u64int  ata_dev_sectors(int dev);            /* число секторов        */
int     ata_read_dev(int dev, u64int lba, u32int count, u8int *buf);   /* 1 = успех */
int     ata_write_dev(int dev, u64int lba, u32int count, u8int *buf);  /* 1 = успех */
