#!/usr/bin/env python3
# 4Kn диск: GPT в 4096-байтных логических секторах + FAT32(bps=4096), ext4, NTFS(bps=4096)
import struct, zlib, subprocess, sys, uuid, os
out=sys.argv[1]; SS=4096; SIZE=256*1024*1024; N=SIZE//SS
def run(*a): subprocess.check_call(a, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
PS=10240  # 40 MiB в секторах по 4K
starts=[256,256+PS+256,256+2*(PS+256),256+3*(PS+256)]
imgs=[('f.img',['mkfs.vfat','-F','32','-S','4096','-n','FAT4K']),
      ('e.img',['mkfs.ext2','-q','-b','4096','-d','src']),
      ('n.img',['mkntfs','-F','-Q','-q','-s','4096','-c','4096']),
      ('x.img',['mkfs.ext4','-q','-b','4096','-d','src'])]
disk=open(out,'wb'); disk.truncate(SIZE)
for i,(img,cmd) in enumerate(imgs):
    open(img,'wb').truncate(PS*SS)
    run(*(cmd+[img]))
    if img=='f.img': run('mcopy','-i',img,'src/HELLO.TXT','::HELLO.TXT')
    disk.seek(starts[i]*SS); disk.write(open(img,'rb').read())
EBD=uuid.UUID('EBD0A0A2-B9E5-4433-87C0-68B6B72699C7').bytes_le
LIN=uuid.UUID('0FC63DAF-8483-4772-8E79-3D69D8477DE4').bytes_le
types=[EBD,LIN,EBD,LIN]
ents=b''
for i,s in enumerate(starts):
    e=types[i]+uuid.uuid4().bytes_le+struct.pack('<QQQ',s,s+PS-1,0)+('P%d'%(i+1)).encode('utf-16le').ljust(72,b'\0')
    ents+=e
ents=ents.ljust(128*128,b'\0'); ecrc=zlib.crc32(ents)&0xffffffff
def hdr(my,alt,ent_lba):
    h=struct.pack('<8sIIIIQQQQ16sQIII',b'EFI PART',0x10000,92,0,0,my,alt,6,N-6,uuid.uuid4().bytes_le,ent_lba,128,128,ecrc)
    c=zlib.crc32(h)&0xffffffff
    return h[:16]+struct.pack('<I',c)+h[20:]
disk.seek(0); mbr=bytearray(512); mbr[446+4]=0xEE; mbr[446+8:446+16]=struct.pack('<II',1,min(N-1,0xFFFFFFFF)); mbr[510:512]=b'\x55\xAA'; disk.write(mbr)
disk.seek(1*SS); disk.write(hdr(1,N-1,2)); disk.seek(2*SS); disk.write(ents)
disk.seek((N-1-4)*SS); disk.write(ents); disk.seek((N-1)*SS); disk.write(hdr(N-1,1,N-1-4))
disk.close()
if len(sys.argv)>2 and sys.argv[2]=='corrupt-primary':
    with open(out,'r+b') as f: f.seek(1*SS+24); f.write(b'\xff'*8)   # ломаем MyLBA/CRC основного заголовка
