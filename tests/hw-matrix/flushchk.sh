#!/bin/bash
for t in "pc ide" "q35 ide" "q35 nvme"; do
  ./mkdisk.sh disk_mbr.img mbr >/dev/null 2>&1
  TMO=25 ./run.sh new.iso disk_mbr.img $t >/dev/null
  printf "%-10s flush-failures: %s  probes-ok: %s\n" "$t" "$(grep -ac 'flush FAILED' /tmp/serial.log)" "$(grep -ac 'write+readback OK' /tmp/serial.log)"
done
echo "-- fsck после NVMe-прогона"
dd if=disk_mbr.img of=c.img bs=512 skip=2048 count=81920 2>/dev/null; fsck.vfat -n c.img 2>&1 | tail -1
dd if=disk_mbr.img of=c.img bs=512 skip=84000 count=81920 2>/dev/null; e2fsck -fn c.img 2>&1 | tail -1
