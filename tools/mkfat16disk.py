#!/usr/bin/env python3
"""
mkfat16disk.py -- build a raw HDD image with an MBR and one FAT16 partition.

Used to exercise the kernel's disk path in QEMU (automount -> disk0, FAT
write/create/truncate) without depending on mtools/parted on the host.

    python mkfat16disk.py out.img 64 [file1:arcname1 ...]
"""

import os
import struct
import sys

SECTOR = 512
SPC = 4
RESERVED = 32
NUM_FATS = 2
ROOT_ENTRIES = 512
MEDIA = 0xF8
PART_START = 2048


def dirent(name, ext, attr, cluster, size):
    e = bytearray(32)
    e[0:8] = name.upper().ljust(8)[:8].encode("ascii", "replace")
    e[8:11] = ext.upper().ljust(3)[:3].encode("ascii", "replace")
    e[11] = attr
    struct.pack_into("<H", e, 24, 0x6000)
    struct.pack_into("<H", e, 26, cluster)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


def build(out_path, size_mb, files):
    total_sectors = size_mb * 1024 * 1024 // SECTOR
    part_sectors = total_sectors - PART_START

    root_dir_sectors = ROOT_ENTRIES * 32 // SECTOR
    fat_size = 1
    while True:
        data_sectors = part_sectors - RESERVED - NUM_FATS * fat_size - root_dir_sectors
        clusters = data_sectors // SPC
        need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if clusters <= 65524 and need <= fat_size:
            break
        fat_size = need
    first_data = RESERVED + NUM_FATS * fat_size + root_dir_sectors

    img = bytearray(total_sectors * SECTOR)
    base = PART_START * SECTOR

    fat = bytearray(fat_size * SECTOR)
    struct.pack_into("<HH", fat, 0, 0xFFF8, 0xFFFF)
    next_cluster = 2
    entries = []

    def alloc(nbytes):
        nonlocal next_cluster
        n = max(1, (nbytes + SPC * SECTOR - 1) // (SPC * SECTOR))
        first = next_cluster
        next_cluster += n
        return first, n

    for path, arc in files:
        with open(path, "rb") as f:
            content = f.read()
        first, n = alloc(len(content))
        for i in range(n):
            off = base + (first_data + (first - 2 + i) * SPC) * SECTOR
            chunk = content[i * SPC * SECTOR:(i + 1) * SPC * SECTOR]
            img[off:off + len(chunk)] = chunk
            struct.pack_into("<H", fat, (first + i) * 2, 0xFFFF if i == n - 1 else first + i + 1)
        name, _, ext = arc.partition(".")
        entries.append(dirent(name, ext, 0x20, first, len(content)))

    entries.append(dirent("SUBDIR", "", 0x10, 0, 0))

    for i in range(NUM_FATS):
        off = base + (RESERVED + i * fat_size) * SECTOR
        img[off:off + len(fat)] = fat

    root_off = base + (RESERVED + NUM_FATS * fat_size) * SECTOR
    for i, e in enumerate(entries[:ROOT_ENTRIES]):
        img[root_off + i * 32:root_off + (i + 1) * 32] = e

    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"MKFAT16 "
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = SPC
    struct.pack_into("<H", bs, 14, RESERVED)
    bs[16] = NUM_FATS
    struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
    struct.pack_into("<H", bs, 19, 0)
    bs[21] = MEDIA
    struct.pack_into("<H", bs, 22, fat_size)
    struct.pack_into("<H", bs, 24, 32)
    struct.pack_into("<H", bs, 26, 64)
    struct.pack_into("<H", bs, 28, 0)
    struct.pack_into("<I", bs, 32, part_sectors)
    bs[36] = 0x80
    bs[38] = 0x29
    struct.pack_into("<I", bs, 39, 0x12345678)
    bs[43:54] = b"AXTEST     "
    bs[54:62] = b"FAT16   "
    bs[510:512] = b"\x55\xAA"
    img[base:base + SECTOR] = bs

    mbr = bytearray(SECTOR)
    part = bytearray(16)
    part[0] = 0x80
    part[4] = 0x06
    struct.pack_into("<I", part, 8, PART_START)
    struct.pack_into("<I", part, 12, part_sectors)
    mbr[446:462] = part
    mbr[510:512] = b"\x55\xAA"
    img[0:SECTOR] = mbr

    with open(out_path, "wb") as f:
        f.write(bytes(img))

    print("wrote %s: %d MB, %d clusters used, fat %d sectors"
          % (out_path, size_mb, next_cluster - 2, fat_size))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    out = sys.argv[1]
    size = int(sys.argv[2])
    pairs = []
    for spec in sys.argv[3:]:
        src, _, arc = spec.partition(":")
        pairs.append((src, arc or os.path.basename(src)))
    build(out, size, pairs)
