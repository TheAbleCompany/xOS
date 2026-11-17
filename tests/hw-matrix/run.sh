#!/bin/bash
# usage: run.sh iso disk machine(pc|q35) bus(ide|ahci|nvme|usb) [extra qemu args] ; prints serial log
ISO=$1; DISK=$2; M=$3; BUS=$4; shift 4
case $BUS in
 ide)  DEV="-drive file=$DISK,format=raw,if=ide,index=0" ;;
 ahci) DEV="-device ich9-ahci,id=ahci -drive file=$DISK,format=raw,if=none,id=d0 -device ide-hd,drive=d0,bus=ahci.0" ;;
 nvme) DEV="-drive file=$DISK,format=raw,if=none,id=n0 -device nvme,drive=n0,serial=nv0" ;;
 none) DEV="" ;;
esac
timeout ${TMO:-25} qemu-system-x86_64 -M $M -m 512M -cdrom $ISO -boot order=d -display none -serial file:/tmp/serial.log -no-reboot $DEV "$@" >/dev/null 2>&1
grep -aE "BLKDEV|AHCI|NVME|AUTOMOUNT|\[FS\]|PROBE|GPT|MBR" /tmp/serial.log
