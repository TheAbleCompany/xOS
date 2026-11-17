#!/bin/bash
ISO=$1; shift
timeout 25 qemu-system-x86_64 -M q35 -m 512M -cdrom $ISO -boot order=d -display none -serial file:/tmp/serial.log -no-reboot "$@" >/dev/null 2>&1
grep -aE "registered|probing|\[FS\]|disk[0-9]:|PROBE" /tmp/serial.log | cut -c1-110
