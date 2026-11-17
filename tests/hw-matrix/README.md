QEMU-стенд (qemu-system-x86, dosfstools, mtools, ntfs-3g, exfatprogs, gdisk, fdisk, e2fsprogs, xorriso):
  rebuild.sh               -- сборка ядра: sub-make ВСЕХ каталогов, потом линковка
  mkiso.sh / mkiso_rd.sh   -- ISO без initrd / с initrd (нужен для storage.log)
  mkdisk.sh, mkebr.sh, mk4k.py -- тестовые диски MBR/GPT, EBR, 4Kn GPT
  run.sh <iso> <img> <pc|q35> <ide|ahci|nvme|none>;  chk.sh <метка> -- сводка по serial-логу
  host-fs/                 -- хост-стенды драйверов ФС (листинг и чтение файла с контрольной суммой)
Самотест VFS: добавьте `#define FS_TREE_SELFTEST 1` первой строкой в fs/ix/ix_automount.c и
kernel/main.c, соберите, загрузите диск с деревом (FAT32/ext2/NTFS) и сравните `[TREE]`-строки
serial-лога с манифестом: treecmp.py.  Пути в скриптах рассчитаны на /home/claude/t.
