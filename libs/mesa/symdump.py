#!/usr/bin/env python3
import struct
import sys
import os

STT_FUNC = 2
STT_OBJECT = 1
STT_NOTYPE = 0
STB_GLOBAL = 1
STB_WEAK = 2


def elf_symbols(data):
    if data[:4] != b"\x7fELF":
        return None
    is64 = data[4] == 2
    if not is64:
        return None
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    if e_shoff == 0:
        return None
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsize = \
            struct.unpack_from("<IIQQQQIIQQ", data, off)
        secs.append((name, typ, flags, addr, offset, size, link, info, align, entsize))

    syms = []
    for (name, typ, flags, addr, offset, size, link, info, align, entsize) in secs:
        if typ != 2:
            continue
        strtab = secs[link]
        stroff = strtab[4]
        count = size // (entsize or 24)
        for j in range(count):
            o = offset + j * (entsize or 24)
            st_name, st_info, st_other, st_shndx, st_value, st_size = \
                struct.unpack_from("<IBBHQQ", data, o)
            if st_name == 0:
                continue
            end = data.index(b"\0", stroff + st_name)
            nm = data[stroff + st_name:end].decode("utf-8", "replace")
            bind = st_info >> 4
            styp = st_info & 0xF
            syms.append((nm, bind, styp, st_shndx, st_value, st_size))
    return syms


def ar_members(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"!<arch>\n":
        yield (os.path.basename(path), data)
        return
    pos = 8
    long_names = b""
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[0:16].decode("ascii", "replace").strip()
        size = int(hdr[48:58].decode("ascii", "replace").strip() or 0)
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size
        if (pos & 1) and pos < len(data):
            pos += 1
        if name == "//":
            long_names = body
            continue
        if name in ("/", "/SYM64/", "__.SYMDEF"):
            continue
        if name.startswith("/") and name[1:].isdigit():
            off = int(name[1:])
            end = long_names.index(b"/\n", off)
            name = long_names[off:end].decode("utf-8", "replace")
        yield (name.rstrip("/"), body)


def main(argv):
    mode = argv[1]
    defined = set()
    undefined = {}
    for path in argv[2:]:
        for mname, body in ar_members(path):
            syms = elf_symbols(body)
            if syms is None:
                continue
            for (nm, bind, styp, shndx, val, size) in syms:
                if bind not in (STB_GLOBAL, STB_WEAK):
                    continue
                if shndx == 0:
                    undefined.setdefault(nm, set()).add(mname)
                else:
                    defined.add(nm)

    if mode == "-u":
        out = sorted(n for n in undefined if n not in defined)
        for n in out:
            print(n)
        print("total undefined: %d" % len(out), file=sys.stderr)
    elif mode == "-d":
        for n in sorted(defined):
            print(n)
    elif mode == "-all":
        print("DEFINED: %d" % len(defined))
        miss = sorted(n for n in undefined if n not in defined)
        print("UNDEFINED (external): %d" % len(miss))
        for n in miss:
            print(n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
