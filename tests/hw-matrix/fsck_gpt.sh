#!/bin/bash
# usage: fsck_gpt.sh disk.img  (GPT-раскладка mkdisk.sh)
D=$1
chk(){ n=$1; s=$2; k=$3
  dd if=$D of=chk_$n.img bs=512 skip=$s count=81920 2>/dev/null
  if [ $k = fat ]; then echo "[$n FAT32]"; fsck.vfat -n chk_$n.img 2>&1 | tail -1; mdir -i chk_$n.img :: 2>&1 | grep -i wprobe | head -1
  else echo "[$n ext]"; e2fsck -fn chk_$n.img 2>&1 | tail -1; debugfs -R "cat wprobe.txt" chk_$n.img 2>/dev/null; echo; fi; }
chk p1 2048 fat; chk p2 83968 ext; chk p4 247808 ext
