#!/usr/bin/env bash
#
# make_test_disk.sh -- создаёт тестовый диск (raw .img) с MBR-разметкой
# и одним FAT16/FAT32-разделом, наполненным файлами -- для проверки
# "disk0" (fs/ix/ix_disk_fat.c) в QEMU.
#
# Использование:
#   ./make_test_disk.sh [OUT_IMG] [SIZE_MB] [FILES_DIR] [FAT_BITS]
#
#   OUT_IMG    -- путь к создаваемому образу диска (по умолчанию testdisk.img)
#   SIZE_MB    -- размер диска в МиБ                (по умолчанию 64)
#   FILES_DIR  -- папка, содержимое которой будет скопировано в корень
#                 раздела (по умолчанию ./testdisk_files, создаётся
#                 с парой примеров, если такой папки ещё нет)
#   FAT_BITS   -- 16 или 32 (по умолчанию 16; для размеров < ~32 МиБ
#                 FAT32 создать не получится -- берите FAT16)
#
# Зависимости: parted, mkfs.vfat/mkfs.fat (dosfstools), mtools (mcopy/mmd), dd.
# В Debian/Ubuntu:  sudo apt-get install -y parted dosfstools mtools
#
# Результат: $OUT_IMG -- диск с MBR-таблицей (msdos) и одним ПЕРВИЧНЫМ
# FAT-разделом, который ix_disk_fat.c находит и монтирует как "disk0".
# Запуск в QEMU:
#   qemu-system-x86_64 -M pc -m 512M -cdrom main.iso -hda testdisk.img -boot d

set -euo pipefail

OUT_IMG="${1:-testdisk.img}"
SIZE_MB="${2:-64}"
FILES_DIR="${3:-testdisk_files}"
FAT_BITS="${4:-16}"

PART_IMG="$(mktemp --suffix=.img)"
trap 'rm -f "$PART_IMG"' EXIT

for tool in parted mkfs.vfat mcopy mmd dd; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Не найден '$tool'. Установите: sudo apt-get install -y parted dosfstools mtools" >&2
        exit 1
    }
done

if [ "$FAT_BITS" != "16" ] && [ "$FAT_BITS" != "32" ]; then
    echo "FAT_BITS должен быть 16 или 32 (получено: $FAT_BITS)" >&2
    exit 1
fi

# Пример содержимого, если пользователь не передал свою папку с файлами.
if [ ! -d "$FILES_DIR" ]; then
    echo "==> '$FILES_DIR' не найдена, создаю с примерами"
    mkdir -p "$FILES_DIR"
    cat > "$FILES_DIR/HELLO.TXT" <<'EOF'
Hello from disk0!
Этот файл лежит на реальном (эмулированном) ATA-диске,
смонтированном через fs/ix/ix_disk_fat.c.
EOF
    cat > "$FILES_DIR/README.MD" <<'EOF'
# ArtyomX64 disk0 test

Если вы видите этот файл в приложении Files во вкладке
"disk0 (real HDD)" -- интеграция FAT-драйвера и ATA PIO
работает от начала до конца.
EOF
fi

echo "==> Создаю диск $OUT_IMG (${SIZE_MB} МиБ)"
dd if=/dev/zero of="$OUT_IMG" bs=1M count="$SIZE_MB" status=none

echo "==> Размечаю MBR (msdos) с одним первичным разделом (1 МиБ .. конец)"
# ВАЖНО: таблица должна быть msdos (MBR), не gpt -- ix_disk_fat.c читает
# только классическую MBR-таблицу из 4 первичных разделов.
parted -s "$OUT_IMG" mklabel msdos mkpart primary fat"$FAT_BITS" 1MiB 100% >/dev/null

# Абсолютный LBA начала и размер раздела в секторах -- нужны, чтобы собрать
# отдельный FAT-образ ровно под этот раздел и потом вклеить его в диск.
read -r PART_LBA PART_SECTORS < <(
    parted -s -m "$OUT_IMG" unit s print \
        | awk -F: '/^1:/ { gsub("s","",$2); gsub("s","",$4); print $2, $4 }'
)
echo "==> Раздел: LBA=$PART_LBA, секторов=$PART_SECTORS"

echo "==> Форматирую раздел в FAT$FAT_BITS и копирую файлы"
truncate -s "$((PART_SECTORS * 512))" "$PART_IMG"
mkfs.vfat -F "$FAT_BITS" -n DISK0 "$PART_IMG" >/dev/null

copy_tree() {
    local src="$1" dst="$2"
    mmd -i "$PART_IMG" "$dst" 2>/dev/null || true
    for entry in "$src"/*; do
        [ -e "$entry" ] || continue
        local name; name="$(basename "$entry")"
        if [ -d "$entry" ]; then
            copy_tree "$entry" "$dst/$name"
        else
            mcopy -i "$PART_IMG" "$entry" "$dst/$name"
        fi
    done
}
copy_tree "$FILES_DIR" "::"

echo "==> Содержимое раздела:"
mdir -i "$PART_IMG" ::

echo "==> Вклеиваю FAT-раздел в диск по LBA $PART_LBA"
dd if="$PART_IMG" of="$OUT_IMG" bs=512 seek="$PART_LBA" conv=notrunc status=none

echo "==> Готово: $OUT_IMG"
echo "    qemu-system-x86_64 -M pc -m 512M -cdrom main.iso -hda $OUT_IMG -boot d -serial stdio"
