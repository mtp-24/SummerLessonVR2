#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How far 1:1 inset captures (agent.capture=3xx) of a moving window are from a still one:
    ghost.py <ref.png> <capture.png>... [--box x0,y0,x1,y1 ...]
Each capture is aligned to the reference by the shift (whole pixels, within +-40) that matches best
over its overlap, then the RMS luma difference left is given over the whole overlap and over each
box (given in the reference's pixels): the ghosting and blur the motion added (the reference's own
TAA noise is the floor: a second still capture against the first)."""
import struct, sys, zlib


def png(p):
    b = open(p, "rb").read(); i = 8; idat = b""; w = h = 0
    while i < len(b):
        n, = struct.unpack(">I", b[i:i + 4]); t = b[i + 4:i + 8]; d = b[i + 8:i + 8 + n]
        if t == b"IHDR": w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT": idat += d
        i += 12 + n
    raw = zlib.decompress(idat)
    return [[0.299 * raw[y * (w * 3 + 1) + 1 + 3 * x] + 0.587 * raw[y * (w * 3 + 1) + 2 + 3 * x] +
             0.114 * raw[y * (w * 3 + 1) + 3 + 3 * x] for x in range(w)] for y in range(h)], w, h


def rms(a, b, w, h, dx, dy, box=None, step=1):
    s = n = 0
    x0, y0, x1, y1 = box or (0, 0, w, h)
    for y in range(max(y0, -dy, 0), min(y1, h - dy, h), step):
        ra, rb = a[y], b[y + dy]
        for x in range(max(x0, -dx, 0), min(x1, w - dx, w), step):
            d = ra[x] - rb[x + dx]; s += d * d; n += 1
    return (s / n) ** 0.5 if n > 500 else 1e9


def best_shift(ref, c, w, h):
    """the whole-pixel shift (within +-40) that matches c to ref best over their overlap"""
    _, bx, by = min(((rms(ref, c, w, h, dx, dy, step=3), dx, dy) for dy in range(-40, 41, 2) for dx in range(-40, 41, 2)))
    _, dx, dy = min(((rms(ref, c, w, h, dx, dy, step=1), dx, dy) for dy in range(by - 2, by + 3) for dx in range(bx - 2, bx + 3)))
    return dx, dy


def main():
    args = sys.argv[1:]
    boxes = []
    if "--box" in args:
        k = args.index("--box"); boxes = [tuple(int(v) for v in s.split(",")) for s in args[k + 1:]]; args = args[:k]
    ref, w, h = png(args[0])
    for p in args[1:]:
        c, _, _ = png(p)
        dx, dy = best_shift(ref, c, w, h)
        r = rms(ref, c, w, h, dx, dy)
        out = "%s: shift %+d,%+d  rms %.2f" % (p.split("/")[-1], dx, dy, r)
        for bb in boxes:
            out += "  box%s %.2f" % (bb, rms(ref, c, w, h, dx, dy, bb))
        print(out)


if __name__ == "__main__":
    main()
