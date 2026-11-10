# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared by match.py, codemap.py and fdiff.py: a PS4 eboot's code read with capstone.

The eboots these were written for (Summer Lesson's three) map their code at VA 0 from file
offset 0x4000 and their data likewise (VA = file offset - 0x4000); text_size() reads the code
segment's size from the program headers. Needs capstone (pip install capstone).
"""
import re
import struct

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

OFF = 0x4000
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True


def load(path):
    return open(path, "rb").read()


def text_size(b):
    """The executable PT_LOAD's file size."""
    phoff, = struct.unpack_from("<Q", b, 0x20)
    phn, = struct.unpack_from("<H", b, 0x38)
    for i in range(phn):
        t, fl, o, va, pa, fs, ms, al = struct.unpack_from("<IIQQQQQQ", b, phoff + i * 56)
        if t == 1 and fl & 1:
            assert o - va == OFF, "code not at file offset VA + 0x4000"
            return fs
    raise ValueError("no executable segment")


def insn_at(b, va):
    for i in md.disasm(b[va + OFF:va + OFF + 16], va):
        return i
    return None


def is_branch(i):
    return i.group(1) or i.group(2) or i.group(7)  # jump, call, relative branch


def rip_target(b, va):
    """The address an instruction reaches: its rip-relative operand, or its rel32 branch."""
    i = insn_at(b, va)
    if not i:
        return None
    for op in i.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    if i.imm_size == 4 and is_branch(i):
        return i.operands[0].imm
    return None


def find_start(b, p, k):
    """The instruction whose displacement (or rel32) is at p and that ends at p + 4 + k; the
    longest decode first (a shorter one can decode the same bytes without their prefix)."""
    for s in range(11, 0, -1):
        i = insn_at(b, p - s)
        if i and i.address + i.size == p + 4 + k and (
                i.disp_offset == s or (i.imm_offset == s and i.imm_size == 4)):
            return i
    return None


def signature(b, va, n=10):
    """A regex for n instructions from va, rip-relative displacements and rel32 branches masked
    (they change when code or data moves); stops after a ret, jmp or ud2. (pattern, length)."""
    pat = b""
    cur = va
    for _ in range(n):
        i = insn_at(b, cur)
        if not i:
            break
        raw = b[cur + OFF:cur + OFF + i.size]
        mask = [False] * i.size
        for op in i.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and i.disp_size:
                for j in range(i.disp_offset, i.disp_offset + i.disp_size):
                    mask[j] = True
        if is_branch(i) and i.imm_size == 4:
            for j in range(i.imm_offset, i.imm_offset + 4):
                mask[j] = True
        pat += b"".join(b"." if m else re.escape(bytes([x])) for x, m in zip(raw, mask))
        cur += i.size
        if i.mnemonic in ("ret", "jmp", "ud2"):
            break
    return pat, cur - va
