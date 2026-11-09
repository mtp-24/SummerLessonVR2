#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""libs.py EBOOT [ID...]: a PS4 eboot's import libraries (DT_SCE_IMPORT_LIB), by the id an
import's NID carries ("abc#BQ#8": library BQ); all of them with no id given."""
import struct
import sys

A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"


def libraries(b):
    phoff, = struct.unpack_from("<Q", b, 0x20)
    phn, = struct.unpack_from("<H", b, 0x38)
    dyn = dynlib = None
    for i in range(phn):
        t, fl, o, va, pa, fs, ms, al = struct.unpack_from("<IIQQQQQQ", b, phoff + i * 56)
        if t == 2:
            dyn = (o, fs)
        elif t == 0x61000000:
            dynlib = o
    tags = {}
    for k in range(0, dyn[1], 16):
        tag, val = struct.unpack_from("<QQ", b, dyn[0] + k)
        tags.setdefault(tag, []).append(val)
    strtab = dynlib + tags[0x61000035][0]
    name = lambda off: b[strtab + off:b.index(b"\0", strtab + off)].decode()
    return {v >> 48: name(v & 0xffffffff) for v in tags.get(0x61000015, [])}


def main():
    libs = libraries(open(sys.argv[1], "rb").read())
    if len(sys.argv) == 2:
        for i in sorted(libs):
            print(i, libs[i])
    for x in sys.argv[2:]:
        n = 0
        for c in x:
            n = n * 64 + A.index(c)
        print(x, n, libs.get(n))


if __name__ == "__main__":
    main()
