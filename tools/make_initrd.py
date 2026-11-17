#!/usr/bin/env python3
"""Сборщик initrd.img в формате ядра ArtyomX64 (fs/initrd.h).

Формат:
    u32  nfiles
    96 * 76-байтные записи (magic, type, app_type, name[64], pad, offset, length)
    данные файлов подряд, offset считается от начала образа.

Использование:
    make_initrd.py host_file initrd_name app_type [host_file initrd_name app_type ...]
"""

import os
import struct
import sys

MAX_FILES = 96
HEADER_MAGIC = 0xBF
FS_FILE = 0x01

APP_TYPES = {
    "none": 0,
    "console": 1,
    "gui": 2,
}

HEADER_SIZE = 76
NAME_SIZE = 64


def usage(prog):
    sys.stderr.write(
        "usage: %s host name type ...\n"
        "types: none, console, gui\n" % prog
    )


def build(entries, out_path):
    data_offset = 4 + HEADER_SIZE * MAX_FILES

    headers = bytearray(HEADER_SIZE * MAX_FILES)
    payload = []

    for index, (host_path, name, app_type) in enumerate(entries):
        encoded = name.encode("utf-8")
        if not encoded:
            raise ValueError("empty initrd name for %s" % host_path)
        if len(encoded) >= NAME_SIZE:
            raise ValueError("initrd name too long: %s" % name)

        with open(host_path, "rb") as stream:
            blob = stream.read()

        base = index * HEADER_SIZE
        headers[base] = HEADER_MAGIC
        headers[base + 1] = FS_FILE
        headers[base + 2] = app_type
        headers[base + 3:base + 3 + len(encoded)] = encoded
        struct.pack_into("<II", headers, base + 68, data_offset, len(blob))

        payload.append(blob)
        data_offset += len(blob)

    with open(out_path, "wb") as stream:
        stream.write(struct.pack("<I", len(entries)))
        stream.write(headers)
        for blob in payload:
            stream.write(blob)

    return data_offset


def main(argv):
    args = argv[1:]
    if not args or len(args) % 3 != 0:
        usage(argv[0])
        return 1

    entries = []
    for offset in range(0, len(args), 3):
        host_path, name, app_name = args[offset:offset + 3]

        if app_name not in APP_TYPES:
            sys.stderr.write("error: unknown app type '%s'\n" % app_name)
            return 1

        if not os.path.isfile(host_path):
            sys.stderr.write("error: file not found: %s\n" % host_path)
            return 1

        entries.append((host_path, name, APP_TYPES[app_name]))

    if len(entries) > MAX_FILES:
        sys.stderr.write("error: too many files (max %d)\n" % MAX_FILES)
        return 1

    total = build(entries, "initrd.img")

    for host_path, name, app_type in entries:
        sys.stderr.write(
            "initrd: %-24s <- %s (%d bytes)\n"
            % (name, host_path, os.path.getsize(host_path))
        )
    sys.stderr.write("initrd.img created: %d bytes\n" % total)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
