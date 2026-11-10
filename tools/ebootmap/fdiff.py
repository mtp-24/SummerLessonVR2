#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""fdiff.py SRC DST SRC_VA DST_VA LENGTH: two stretches of code compared instruction by
instruction, the addresses that instructions reach left out (they move with the code). Lists
the first differences and how many there are. Start both at an instruction boundary."""
import sys

import ebootmap as em


def listing(b, va, ln):
    out = []
    for i in em.md.disasm(b[va + em.OFF:va + em.OFF + ln], va):
        ops = i.op_str
        if em.rip_target(b, i.address) is not None or em.is_branch(i):
            ops = ops.split("[")[0] + ("<ref>" if "[" in ops else "<target>")
        out.append((i.mnemonic, ops, i.address))
    return out


def main():
    S, D = em.load(sys.argv[1]), em.load(sys.argv[2])
    sa, da, ln = (int(x, 16) for x in sys.argv[3:6])
    A, B = listing(S, sa, ln), listing(D, da, ln)
    diff = 0
    for x, y in zip(A, B):
        if x[:2] != y[:2]:
            diff += 1
            if diff <= 15:
                print(f"  {x[2]:#x} {x[0]} {x[1]}  |  {y[2]:#x} {y[0]} {y[1]}")
    print(f"{sa:#x} / {da:#x}: {len(A)} and {len(B)} instructions, {diff} differ")


if __name__ == "__main__":
    main()
