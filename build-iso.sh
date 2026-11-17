#!/bin/sh
# build-iso.sh -- stage isodir/ and produce artyom-test.iso with Limine.
#
# Host has no xorriso on PATH except the copy shipped next to the cross
# toolchain in D:/toolchain/bin, and limine.exe lives in limine-binary/.
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

XORRISO=${XORRISO:-/d/toolchain/bin/xorrisofs.exe}
LIMINE=limine-binary/limine-tool-windows-x86/limine.exe
LIMINE_BIN=limine-binary
MAKE=${MAKE:-mingw32-make}
PYTHON=${PYTHON:-py -3}

[ -x "$XORRISO" ] || { echo "missing $XORRISO" >&2; exit 1; }
[ -f "$LIMINE"   ] || { echo "missing $LIMINE" >&2; exit 1; }

if [ ! -f main.elf ]; then
    echo "main.elf not built -- run: make" >&2
    exit 1
fi

if [ ! -f apps/initrd.img ] || [ -n "$(find apps -maxdepth 1 -name '*.c' -newer apps/initrd.img -print -quit)" ]; then
    $MAKE -C apps initrd
fi

rm -rf isodir
mkdir -p isodir/boot/limine isodir/EFI/BOOT

cp main.elf         isodir/boot/main.elf
cp apps/initrd.img  isodir/boot/initrd.img
cp boot/limine/limine.conf isodir/boot/limine/limine.conf
cp "$LIMINE_BIN/limine-bios.sys"     isodir/boot/limine/
cp "$LIMINE_BIN/limine-bios-cd.bin"  isodir/boot/limine/
cp "$LIMINE_BIN/limine-uefi-cd.bin"  isodir/boot/limine/
cp "$LIMINE_BIN/BOOTX64.EFI"         isodir/EFI/BOOT/

"$XORRISO" -R -r -J -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    isodir -o artyom-test.iso

"$LIMINE" bios-install artyom-test.iso

echo "built artyom-test.iso ($(wc -c < artyom-test.iso) bytes)"
