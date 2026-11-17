#!/bin/bash
# MBR: primary FAT32 + extended{ ext2, ext4, NTFS }
set -e
OUT=$1; rm -f $OUT; truncate -s 256M $OUT
sfdisk -q $OUT <<S
label: dos
start=2048, size=81920, type=c
start=84000, size=400000, type=5
start=86048, size=81920, type=83
start=172000, size=81920, type=83
start=258000, size=81920, type=7
S
python3 - $OUT <<'PY'
import sys,subprocess,json
out=sys.argv[1]
parts=json.loads(subprocess.check_output(['sfdisk','-J',out]))['partitiontable']['partitions']
m={'c':'p1.img','83':None,'7':'p3.img'}
imgs=['p1.img',None,'p2.img','p4.img','p3.img']
for p,img in zip(parts,imgs):
    print(p['node'],p['start'],p['size'],p['type'])
    if img:
        with open(out,'r+b') as f, open(img,'rb') as g: f.seek(p['start']*512); f.write(g.read())
PY
