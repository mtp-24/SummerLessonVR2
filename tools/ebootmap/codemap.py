#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""codemap.py SRC DST VA...: where each code address of SRC is in DST, by a signature from it
(8, 12, 16, 24 then 32 instructions, until it is found exactly once). "same bytes" when the
first 16 bytes are identical too (a patch site's original bytes, for one)."""
import re
import sys

import ebootmap as em


def main():
    S, D = em.load(sys.argv[1]), em.load(sys.argv[2])
    Dt = D[em.OFF:em.OFF + em.text_size(D)]
    for a in sys.argv[3:]:
        va, res = int(a, 16), None
        for n in (8, 12, 16, 24, 32):
            pat, ln = em.signature(S, va, n)
            hits = [m.start() for m in re.finditer(pat, Dt, re.DOTALL)][:2]
            if len(hits) != 1 and hits:
                continue  # more than one: longer
            res = (hits[0] if hits else None, n, ln)
            break
        if not res:
            print(f"{va:#x} -> found more than once")
        elif res[0] is None:
            print(f"{va:#x} -> not found ({res[1]} instructions, {res[2]} bytes)")
        else:
            d = res[0]
            same = S[va + em.OFF:va + em.OFF + 16] == D[d + em.OFF:d + em.OFF + 16]
            print(f"{va:#x} -> {d:#x} ({res[1]} instructions, {res[2]} bytes){' same bytes' if same else ''}")


if __name__ == "__main__":
    main()
