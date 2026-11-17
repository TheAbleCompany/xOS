#!/bin/bash
# usage: mkiso.sh <kernel_dir> <out.iso>
set -e
K=$1; OUT=$2; R=$(mktemp -d)
mkdir -p $R/boot/limine $R/EFI/BOOT
cp $K/main.elf $R/boot/
cp $K/boot/limine/limine.conf $R/boot/limine/
# убираем module_path: initrd нет
cp /home/claude/t/initrd.img $R/boot/initrd.img
cp $K/limine-binary/{limine-bios.sys,limine-bios-cd.bin,limine-uefi-cd.bin} $R/boot/limine/
cp $K/limine-binary/BOOTX64.EFI $R/EFI/BOOT/
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label $R -o $OUT 2>&1 | tail -1
$K/limine-binary/limine-tool bios-install $OUT 2>&1 | tail -1
