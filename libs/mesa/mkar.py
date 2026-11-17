#!/usr/bin/env python3
import os
import struct
import sys

import symdump

ARMAG = b"!<arch>\n"
STB_GLOBAL = 1
STB_WEAK = 2


def read_members(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[:8] != ARMAG:
        raise SystemExit("%s is not an ar archive" % path)

    pos = 8
    out = []
    long_names = b""

    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[0:16].decode("ascii", "replace").strip()
        try:
            size = int(hdr[48:58].decode("ascii", "replace").strip())
        except ValueError:
            break
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
            real = long_names[off:end].decode("utf-8", "replace")
        else:
            real = name.rstrip("/")

        out.append((real, body))

    return out


def _member_header(name_field, size):
    hdr = bytearray()
    hdr += name_field.ljust(16)
    hdr += b"0".ljust(12)
    hdr += b"0".ljust(6)
    hdr += b"0".ljust(6)
    hdr += b"644".ljust(8)
    hdr += str(size).encode("ascii").ljust(10)
    hdr += b"`\n"
    return bytes(hdr)


def _member_symbols(body):
    syms = symdump.elf_symbols(body)
    if not syms:
        return []
    out = []
    for (nm, bind, styp, shndx, val, size) in syms:
        if bind not in (STB_GLOBAL, STB_WEAK):
            continue
        if shndx == 0:
            continue
        if styp == 3:
            continue
        out.append(nm)
    return out


def write_members(path, members):
    long_names = bytearray()
    encoded = []

    for name, body in members:
        raw = name.encode("utf-8")
        if len(raw) > 15 or name != name.rstrip():
            off = len(long_names)
            long_names += raw + b"/\n"
            encoded.append((("/%d" % off).encode("ascii"), body, raw))
        else:
            encoded.append(((name + "/").encode("ascii"), body, raw))

    sym_entries = []  # (member index, symbol name), one entry per symbol
    for idx, (name_field, body, raw) in enumerate(encoded):
        for nm in _member_symbols(body):
            sym_entries.append((idx, nm))

    sym_names = [nm for _, nm in sym_entries]

    index_body = bytearray()
    index_body += struct.pack(">I", len(sym_names))

    names_bytes = bytearray()
    for nm in sym_names:
        names_bytes += nm.encode("utf-8") + b"\0"

    slots = 4 + 4 * len(sym_names)

    long_names_blob = b""
    if long_names:
        long_names_blob = _member_header(b"//", len(long_names)) + bytes(long_names)
        if len(long_names_blob) & 1:
            long_names_blob += b"\n"

    index_off = 8
    index_size = slots + len(names_bytes)
    index_total = 60 + index_size + (index_size & 1)

    # GNU ar places the symbol index ("/") first, then the long-name
    # table ("//"), then members. bfd only detects the armap when it is
    # the first member, so keep that order.
    long_names_off = index_off + index_total
    member_off = long_names_off + len(long_names_blob)

    offsets = [0] * len(encoded)
    for idx, (name_field, body, raw) in enumerate(encoded):
        offsets[idx] = member_off
        member_off += 60 + len(body) + (len(body) & 1)

    for member_idx, _ in sym_entries:
        index_body += struct.pack(">I", offsets[member_idx])
    index_body += names_bytes

    buf = bytearray(ARMAG)
    buf += _member_header(b"/", len(index_body))
    buf += index_body
    if len(index_body) & 1:
        buf += b"\n"
    buf += long_names_blob

    for idx, (name_field, body, raw) in enumerate(encoded):
        buf += _member_header(name_field, len(body))
        buf += body
        if len(body) & 1:
            buf += b"\n"

    with open(path, "wb") as f:
        f.write(buf)

    return len(sym_names)


def main(argv):
    if len(argv) < 3:
        print("usage: mkar.py <out.a> <obj>...")
        print("       mkar.py -dir <objroot> <out.a>")
        print("       mkar.py -extract <in.a> <outdir>")
        return 2

    if argv[1] == "-dir":
        root = argv[2]
        dest = argv[3]
        skip = set()
        rest = argv[4:]
        if len(rest) >= 2 and rest[0] == "--skip":
            skip = set(os.path.normpath(p) for p in rest[1].split(","))
        objs = []
        for dirpath, dirnames, filenames in os.walk(root):
            rel = os.path.normpath(os.path.relpath(dirpath, root))
            dirnames[:] = [d for d in dirnames
                           if os.path.normpath(os.path.join(rel, d)) not in skip]
            if rel in skip:
                continue
            for fn in filenames:
                if fn.endswith(".o"):
                    objs.append(os.path.join(dirpath, fn))
        objs.sort()
        members = []
        used = {}
        for obj in objs:
            with open(obj, "rb") as f:
                body = f.read()
            name = obj.replace(root, "").replace("\\", "/").lstrip("/").replace("/", "_")
            if name in used:
                used[name] += 1
                name = "%d_%s" % (used[name], name)
            else:
                used[name] = 1
            members.append((name, body))
        nsyms = write_members(dest, members)
        print("wrote %s (%d members, %d syms, %d bytes)" %
              (dest, len(members), nsyms, os.path.getsize(dest)))
        return 0

    if argv[1] == "-list":
        members = read_members(argv[2])
        print("%d members, %d bytes" % (len(members), os.path.getsize(argv[2])))
        return 0

    if argv[1] == "-extract":
        members = read_members(argv[2])
        outdir = argv[3]
        os.makedirs(outdir, exist_ok=True)
        for name, body in members:
            dest = os.path.join(outdir, os.path.basename(name))
            with open(dest, "wb") as f:
                f.write(body)
        print("extracted %d members" % len(members))
        return 0

    out = argv[1]
    members = []
    seen = {}

    for obj in argv[2:]:
        with open(obj, "rb") as f:
            body = f.read()
        name = os.path.basename(obj)
        if name in seen:
            seen[name] += 1
            name = "%d_%s" % (seen[name], name)
        else:
            seen[name] = 1
        members.append((name, body))

    nsyms = write_members(out, members)
    print("wrote %s (%d members, %d syms, %d bytes)" %
          (out, len(members), nsyms, os.path.getsize(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
