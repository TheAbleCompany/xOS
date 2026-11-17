#!/bin/bash
# make_int80list.sh — адреса всех int $0x80 в ELF (для ldsim)
#
#   usage: make_int80list.sh <elf> <out.txt>
#
# ldsim (tools/ldsim.c) патчит эти места на ud2 и эмулирует syscall
# ядра ArtyomX64 на хосте.
set -e
ELF="$1"; OUT="$2"
objdump -d "$ELF" | awk '
  $2=="cd" && $3=="80" { sub(":.*$", "", $1); print $1 }
' | tr -d ':' > "$OUT"
echo "int80 sites: $(wc -l < "$OUT")"
