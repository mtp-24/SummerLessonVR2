#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The TV captures in an agent log (agent.capture=N, agent/mirror.c) as PNGs:

    capture.py <agent.log> <out-prefix>     -> <out-prefix>-<N>.png for each capture

The log's lines "capture N begin WxH bgr ...", "capture N <line> <base64>", "capture N end";
the pixels are B, G, R as the display buffer holds them.
"""
import base64, re, struct, sys, zlib


def png(path, w, h, rgb):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)


def main(log, prefix):
    caps = {}
    for line in open(log, errors="replace"):
        m = re.search(r"capture (\d+) begin (\d+)x(\d+)", line)
        if m:
            caps[int(m.group(1))] = {"w": int(m.group(2)), "h": int(m.group(3)), "lines": {}}
            continue
        m = re.search(r"capture (\d+) (\d+) ([A-Za-z0-9+/=]+)\s*$", line)
        if m and int(m.group(1)) in caps:
            caps[int(m.group(1))]["lines"][int(m.group(2))] = base64.b64decode(m.group(3))
    for n, c in sorted(caps.items()):
        data = b"".join(c["lines"][i] for i in sorted(c["lines"]))
        want = c["w"] * c["h"] * 3
        missing = want - len(data)
        if missing > 0:
            data += b"\x80" * missing  # grey where lines were lost
        bgr = data[:want]
        rgb = bytearray(want)
        rgb[0::3], rgb[1::3], rgb[2::3] = bgr[2::3], bgr[1::3], bgr[0::3]
        out = "%s-%d.png" % (prefix, n)
        png(out, c["w"], c["h"], bytes(rgb))
        print("%s: %dx%d%s" % (out, c["w"], c["h"],
                               ", %d bytes missing (grey)" % missing if missing > 0 else ""))


if __name__ == "__main__":
    main(*sys.argv[1:3])
