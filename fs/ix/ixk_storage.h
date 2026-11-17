// ixk_storage.h -- слой накопителей для ix-FS драйверов (FAT/NTFS/EXT).
//
// Раньше только legacy ATA PIO primary master (0x1F0), из-за чего "disk0"
// на современном железе (AHCI-only, второй канал, slave) был недоступен.
// Теперь бэкенды: legacy ATA (4 устройства: 2 канала x master/slave)
// и AHCI/SATA (polling, drivers/ahci.c).  Порядок: primary master,
// primary slave, secondary master, secondary slave, затем AHCI-порты.
//
// Использование:
//   ixk_storage_init();
//   for (int d = 0; d < ixk_storage_dev_count(); d++) {
//       ixk_storage_set_dev(d);
//       ... ixk_storage_read() работает с активным устройством ...
//   }

#ifndef IXK_STORAGE_H
#define IXK_STORAGE_H

#include <stdint.h>

#define IXK_KIND_ATA  0
#define IXK_KIND_AHCI 1

typedef struct {
    uint8_t  kind;      /* IXK_KIND_*                      */
    uint8_t  dev;       /* индекс в бэкенде                */
    uint64_t sectors;   /* размер в секторах               */
    char     label[16]; /* "ata0", "ahci3" -- для логов    */
} ixk_storage_dev_t;

/* Обсмотреть все бэкенды (идемпотентно). Возвращает число найденных. */
int      ixk_storage_init(void);

/* Нет устройств на машине (NVMe-only и т.п.). */
int      ixk_storage_dev_present(void);

int      ixk_storage_dev_count(void);
const ixk_storage_dev_t *ixk_storage_dev_info(int i);
/* Выбрать активное устройство (все ixk_storage_read идут в него). */
int      ixk_storage_set_dev(int i);
int      ixk_storage_cur_dev(void);

/* Читает count секторов по 512 байт с активного устройства, начиная
 * с абсолютного LBA, в buf. 1 = успех, 0 = ошибка/нет устройства. */
int      ixk_storage_read(uint64_t lba, uint32_t count, void *buf);

#endif
