#!/bin/bash
# usage: mkdisk.sh out.img mbr|gpt [sector_log=512]
set -e
OUT=$1; TBL=$2
SZ=256 # MiB
rm -f $OUT p*.img; truncate -s ${SZ}M $OUT
mkdir -p src/sub; echo "hello from fs" > src/HELLO.TXT; echo nested > src/sub/NEST.TXT
# part images: 40 MiB each
for i in 1 2 3 4 5; do truncate -s 40M p$i.img; done
mkfs.vfat -F 32 -n FAT32T p1.img >/dev/null
mcopy -i p1.img src/HELLO.TXT ::HELLO.TXT; mmd -i p1.img ::sub; mcopy -i p1.img src/sub/NEST.TXT ::sub/NEST.TXT
mkfs.ext2 -q -d src p2.img
mkntfs -F -Q -q p3.img >/dev/null 2>&1
mkfs.ext4 -q -d src p4.img
mkfs.exfat p5.img >/dev/null
# layout in sectors: start 2048, each 40MiB=81920 sectors
if [ "$TBL" = mbr ]; then
sfdisk -q $OUT <<S
label: dos
start=2048, size=81920, type=c
start=84000, size=81920, type=83
start=165920, size=81920, type=7
start=247840, size=81920, type=83
S
else
sgdisk -Z $OUT >/dev/null
sgdisk -n1:2048:+40M -t1:0700 -n2:0:+40M -t2:8300 -n3:0:+40M -t3:0700 -n4:0:+40M -t4:8300 -n5:0:+40M -t5:0700 $OUT >/dev/null
fi
# вклеить образы по реальным стартам
python3 - "$OUT" "$TBL" <<'PY'
import sys,subprocess,re
out=sys.argv[1]
j=subprocess.check_output(['sfdisk','-J',out]).decode()
import json
parts=json.loads(j)['partitiontable']['partitions']
for i,p in enumerate(parts):
    img=f'p{i+1}.img'
    with open(out,'r+b') as f, open(img,'rb') as g:
        f.seek(p['start']*512); f.write(g.read())
    print(p['node'],p['start'],p['size'])
PY
