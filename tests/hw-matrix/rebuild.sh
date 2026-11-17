#!/bin/bash
cd /home/claude/artyomos/ArtyomX64
for d in flanterm kernel drivers drivers/gpu fs lib mm multitask; do make -C $d 2>&1 | grep -E " error |Error " ; done
make main.elf 2>&1 | grep -E " error |undefined|Error "
ls -la main.elf | awk '{print "main.elf", $5}'
