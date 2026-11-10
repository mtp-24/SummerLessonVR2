#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""match.py SRC DST TARGET_VA [MAXREFS]: where a static of SRC's (a variable at TARGET_VA) is in
DST, from the code that reaches it.

Every instruction of SRC reaching TARGET_VA (rip-relative; found by xref) gives a signature of
the 10 instructions from it (ebootmap.signature); one found exactly once in DST gives the
target's address there from its own instruction. Each reference votes; one address with every
vote is the answer. Build xref first (see xref.c).
"""
import collections
import os
import re
import subprocess
import sys

import ebootmap as em


def main():
    src, dst, tg = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    maxrefs = int(sys.argv[4]) if len(sys.argv) > 4 else 60
    S, D = em.load(src), em.load(dst)
    Dt = D[em.OFF:em.OFF + em.text_size(D)]
    xref = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xref")
    if not os.access(xref, os.X_OK):
        sys.exit(f"build xref first: cc -O2 -o {xref} {xref}.c")
    out = subprocess.run([xref, src, hex(em.OFF), hex(em.text_size(S)), hex(tg)],
                         capture_output=True, text=True, check=True).stdout
    votes, examples, tried = collections.Counter(), {}, 0
    for line in out.splitlines():
        _, p, k = line.split()
        i = em.find_start(S, int(p, 16), int(k))
        if not i or em.rip_target(S, i.address) != tg:
            continue
        pat, ln = em.signature(S, i.address)
        if ln < 24:
            continue  # too short to be told apart
        tried += 1
        hits = [m.start() for m in re.finditer(pat, Dt, re.DOTALL)][:2]
        if len(hits) == 1:
            t = em.rip_target(D, hits[0])
            votes[t] += 1
            examples.setdefault(t, (i.address, hits[0]))
        if tried >= maxrefs:
            break
    print(f"{tg:#x}: {tried} references tried, {sum(votes.values())} found once in {os.path.basename(dst)}:",
          ", ".join(f"{t:#x} x{n} (e.g. {examples[t][0]:#x} -> {examples[t][1]:#x})"
                    for t, n in votes.most_common(4)) or "none")


if __name__ == "__main__":
    main()
