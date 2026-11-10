#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A GCN gfx7 (the PS4's GPU) disassembler: LLVM assembles gfx7 but no longer disassembles it.

    gcn7dis.py <code.bin> [offset] [length]   -> the instructions, one a line

The opcode tables come from LLVM itself (no number typed by hand): LLVM's gfx8 disassembler is
swept over each encoding's opcode field for the mnemonics (with an operand sample each), each
sample is assembled again for gfx7, and its gfx7 opcode read back from the encoding; cached in
tools/gcn7dis.json next to this file (made again when missing). The operands are printed from the
fields (registers, inline constants, literals) as the Sea Islands ISA lays them out.
"""
import json, os, re, struct, subprocess, sys

MC = os.path.join(os.environ.get("LLVM") or "/opt/homebrew/opt/llvm/bin", "llvm-mc")
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gcn7dis.json")


def mc(cpu, lines, disasm=False):
    """llvm-mc over many lines at once: each input line's output line (None where it failed)"""
    args = [MC, "-triple=amdgcn", "-mcpu=" + cpu, "-show-encoding"] + (["--disassemble"] if disasm else [])
    out = []
    # (one call per line keeps errors from shifting the outputs; batched by chunks of markers)
    text = "\n".join(lines) + "\n"
    r = subprocess.run(args, input=text, capture_output=True, text=True)
    return r.stdout, r.stderr


def vi_words(cls, op):
    """a gfx8 encoding of class cls with opcode op and harmless operands"""
    if cls == "SOP2": return [0x80000000 | op << 23 | 2 << 16 | 1 << 8 | 0]
    if cls == "SOPK": return [0xb0000000 | op << 23 | 2 << 16 | 0x10]
    if cls == "SOP1": return [0xbe800000 | 2 << 16 | op << 8 | 4]
    if cls == "SOPC": return [0xbf000000 | op << 16 | 1 << 8 | 0]
    if cls == "SOPP": return [0xbf800000 | op << 16 | 0]
    if cls == "VOP1": return [0x7e000000 | 2 << 17 | op << 9 | 0x101]
    if cls == "VOP2": return [op << 25 | 2 << 17 | 3 << 9 | 0x101]
    if cls == "VOPC": return [0x7c000000 | op << 17 | 3 << 9 | 0x101]
    if cls == "VOP3": return [0xd0000000 | op << 16 | 2, 0x101 | 0x102 << 9 | 0x103 << 18]
    if cls == "VINTRP": return [0xd4000000 | 2 << 18 | (op & 3) << 16 | 1 << 10 | 0]
    if cls == "DS": return [0xd8000000 | op << 17 | 0x10, 1 | 2 << 8 | 3 << 16 | 4 << 24]
    if cls == "MUBUF": return [0xe0000000 | op << 18 | 0x10, 1 | 2 << 8 | 1 << 16 | 0x80 << 24]
    if cls == "MTBUF": return [0xe8000000 | op << 15 | 0x10, 1 | 2 << 8 | 1 << 16 | 0x80 << 24]
    if cls == "MIMG": return [0xf0000000 | op << 18 | 0xf << 8, 4 | 0 << 8 | 2 << 16 | 4 << 21]
    if cls == "SMEM": return [0xc0000000 | op << 18 | 2 << 6 | 1 << 17, 0x10]
    raise ValueError(cls)


# each class: the gfx8 opcode range swept, and how gfx7 encodes the opcode (a field of word 0)
CLASSES = {
    "SOP2": (128, lambda w: (w >> 23) & 0x7f), "SOPK": (32, lambda w: (w >> 23) & 0x1f),
    "SOP1": (256, lambda w: (w >> 8) & 0xff), "SOPC": (128, lambda w: (w >> 16) & 0x7f),
    "SOPP": (128, lambda w: (w >> 16) & 0x7f), "VOP1": (256, lambda w: (w >> 9) & 0xff),
    "VOP2": (64, lambda w: (w >> 25) & 0x3f), "VOPC": (256, lambda w: (w >> 17) & 0xff),
    "VOP3": (1024, lambda w: (w >> 17) & 0x1ff), "VINTRP": (4, lambda w: (w >> 16) & 3),
    "DS": (256, lambda w: (w >> 18) & 0xff), "MUBUF": (128, lambda w: (w >> 18) & 0x7f),
    "MTBUF": (16, lambda w: (w >> 16) & 7), "MIMG": (128, lambda w: (w >> 18) & 0x7f),
    "SMEM": (256, lambda w: (w >> 22) & 0x1f),
}


def gfx7_class(w):
    if w >> 23 == 0x17d: return "SOP1"
    if w >> 23 == 0x17e: return "SOPC"
    if w >> 23 == 0x17f: return "SOPP"
    if w >> 28 == 0xb: return "SOPK"
    if w >> 30 == 2: return "SOP2"
    if w >> 27 == 0x18: return "SMEM"  # (SMRD)
    if w >> 25 == 0x3f: return "VOP1"
    if w >> 25 == 0x3e: return "VOPC"
    if w >> 31 == 0: return "VOP2"
    k = w >> 26
    return {0x32: "VINTRP", 0x34: "VOP3", 0x36: "DS", 0x38: "MUBUF", 0x3a: "MTBUF", 0x3c: "MIMG",
            0x3e: "EXP"}.get(k)


def build_tables():
    tables = {}
    for cls, (n, field) in CLASSES.items():
        lines, ops = [], []
        for op in range(n):
            ws = vi_words(cls, op)
            lines.append(" ".join("0x%02x" % b for w in ws for b in struct.pack("<I", w)))
            ops.append(op)
        out, _ = mc("gfx801", lines, disasm=True)
        samples = sorted({ln.split("//")[0].strip() for ln in out.splitlines()
                          if ln.strip() and not ln.strip().startswith(".") and "encoding" in ln})
        t = {}
        for s in samples:
            o, _ = mc("gfx701", [s])
            m = re.search(r"encoding: \[(.*?)\]", o)
            if not m:
                continue
            b = [int(x, 16) for x in m.group(1).split(",")]
            w0 = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24
            c7 = gfx7_class(w0)
            if c7 is None:
                continue
            op7 = CLASSES[c7][1](w0) if c7 in CLASSES else 0
            t.setdefault(c7, {})[str(op7)] = s.split()[0]
        for c7, d in t.items():
            tables.setdefault(c7, {}).update(d)
        print("  %s: %d mnemonics" % (cls, len(t.get(cls, {}))), file=sys.stderr)
    json.dump(tables, open(CACHE, "w"), indent=0, sort_keys=True)
    return tables


INLINE = {**{128 + i: str(i) for i in range(65)}, **{192 + i: str(-i) for i in range(1, 17)},
          240: "0.5", 241: "-0.5", 242: "1.0", 243: "-1.0", 244: "2.0", 245: "-2.0", 246: "4.0",
          247: "-4.0", 106: "vcc_lo", 107: "vcc_hi", 124: "m0", 126: "exec_lo", 127: "exec_hi",
          251: "vccz", 252: "execz", 253: "scc", 254: "lds_direct"}


def src(v, lit):
    if v < 104: return "s%d" % v
    if v >= 256: return "v%d" % (v - 256)
    if v == 255: return "0x%08x" % lit
    return INLINE.get(v, "?%d" % v)


def dis(code, base=0, tables=None):
    words = struct.unpack_from("<%dI" % (len(code) // 4), code)
    i, out = 0, []
    while i < len(words):
        w = words[i]
        c = gfx7_class(w)
        nw = 2 if c in ("VOP3", "DS", "MUBUF", "MTBUF", "MIMG", "EXP") else 1
        w1 = words[i + 1] if nw == 2 and i + 1 < len(words) else 0
        lit = None
        def name(cls, op):
            return (tables.get(cls) or {}).get(str(op), "%s_op%d" % (cls.lower(), op))
        txt = "?? 0x%08x" % w
        litp = words[i + nw] if i + nw < len(words) else 0
        uses_lit = False
        if c == "SOP2":
            a, b = w & 0xff, (w >> 8) & 0xff
            uses_lit = 255 in (a, b)
            txt = "%s s%d, %s, %s" % (name(c, (w >> 23) & 0x7f), (w >> 16) & 0x7f, src(a, litp), src(b, litp))
        elif c == "SOPK":
            txt = "%s s%d, 0x%x" % (name(c, (w >> 23) & 0x1f), (w >> 16) & 0x7f, w & 0xffff)
        elif c == "SOP1":
            a = w & 0xff; uses_lit = a == 255
            txt = "%s s%d, %s" % (name(c, (w >> 8) & 0xff), (w >> 16) & 0x7f, src(a, litp))
        elif c == "SOPC":
            a, b = w & 0xff, (w >> 8) & 0xff; uses_lit = 255 in (a, b)
            txt = "%s %s, %s" % (name(c, (w >> 16) & 0x7f), src(a, litp), src(b, litp))
        elif c == "SOPP":
            op = (w >> 16) & 0x7f; s16 = w & 0xffff
            n = name(c, op)
            if "branch" in n:
                txt = "%s @%x" % (n, base + 4 * (i + 1 + (s16 - 0x10000 if s16 & 0x8000 else s16)))
            else:
                txt = "%s 0x%x" % (n, s16)
        elif c == "SMEM":
            op = (w >> 22) & 0x1f; imm = (w >> 8) & 1; off = w & 0xff
            uses_lit = not imm and off == 255
            txt = "%s s%d, s[%d:], %s" % (name(c, op), (w >> 15) & 0x7f, ((w >> 9) & 0x3f) * 2,
                                          "0x%x" % off if imm else src(off, litp))
        elif c in ("VOP1", "VOP2", "VOPC"):
            s0 = w & 0x1ff; uses_lit = s0 == 255
            if c == "VOP1":
                txt = "%s v%d, %s" % (name(c, (w >> 9) & 0xff), (w >> 17) & 0xff, src(s0, litp))
            elif c == "VOP2":
                n2 = name(c, (w >> 25) & 0x3f)
                txt = "%s v%d, %s, v%d" % (n2, (w >> 17) & 0xff, src(s0, litp), (w >> 9) & 0xff)
                if n2 in ("v_madmk_f32", "v_madak_f32"):  # (a constant dword always follows)
                    uses_lit = True
                    txt += ", 0x%08x" % litp
            else:
                txt = "%s vcc, %s, v%d" % (name(c, (w >> 17) & 0xff), src(s0, litp), (w >> 9) & 0xff)
        elif c == "VINTRP":
            txt = "%s v%d, v%d, attr%d.%s" % (name(c, (w >> 16) & 3), (w >> 18) & 0xff, w & 0xff,
                                             (w >> 10) & 0x3f, "xyzw"[(w >> 8) & 3])
        elif c == "VOP3":
            op = (w >> 17) & 0x1ff
            s = [w1 & 0x1ff, (w1 >> 9) & 0x1ff, (w1 >> 18) & 0x1ff]
            neg, ab = (w1 >> 29) & 7, (w >> 8) & 7
            ops = []
            for k in range(3):
                t = src(s[k], 0)
                if ab >> k & 1: t = "|%s|" % t
                if neg >> k & 1: t = "-" + t
                ops.append(t)
            mods = (" clamp" if w >> 11 & 1 else "") + (" omod%d" % ((w1 >> 27) & 3) if (w1 >> 27) & 3 else "")
            txt = "%s v%d, %s%s" % (name(c, op), w & 0xff, ", ".join(ops), mods)
        elif c == "DS":
            txt = "%s v%d, v%d, v%d, v%d off0 %d off1 %d" % (name(c, (w >> 18) & 0xff), (w1 >> 24) & 0xff,
                                                         w1 & 0xff, (w1 >> 8) & 0xff, (w1 >> 16) & 0xff, w & 0xff, (w >> 8) & 0xff)
        elif c in ("MUBUF", "MTBUF"):
            op = (w >> 18) & 0x7f if c == "MUBUF" else (w >> 16) & 7
            txt = "%s v%d, v%d, s[%d:], %s offset 0x%x%s%s" % (name(c, op), (w1 >> 8) & 0xff, w1 & 0xff,
                                                     ((w1 >> 16) & 0x1f) * 4, src(w1 >> 24, 0), w & 0xfff,
                                                     " idxen" if w >> 13 & 1 else "", " offen" if w >> 12 & 1 else "")
            if c == "MTBUF":
                txt += " dfmt %d nfmt %d" % ((w >> 19) & 0xf, (w >> 23) & 7)
        elif c == "MIMG":
            txt = "%s v%d, v%d, s[%d:], s[%d:] dmask 0x%x%s%s" % (name(c, (w >> 18) & 0x7f), (w1 >> 8) & 0xff,
                                                              w1 & 0xff, ((w1 >> 16) & 0x1f) * 4, ((w1 >> 21) & 0x1f) * 4,
                                                              (w >> 8) & 0xf, " unorm" if w >> 12 & 1 else "",
                                                              " glc" if w >> 13 & 1 else "")
        elif c == "EXP":
            tgt = (w >> 4) & 0x3f
            tn = "mrt%d" % tgt if tgt < 8 else "mrtz" if tgt == 8 else "null" if tgt == 9 else \
                "pos%d" % (tgt - 12) if 12 <= tgt < 16 else "param%d" % (tgt - 32) if tgt >= 32 else "t%d" % tgt
            txt = "exp %s en 0x%x v%d v%d v%d v%d%s%s%s" % (tn, w & 0xf, w1 & 0xff, (w1 >> 8) & 0xff, (w1 >> 16) & 0xff,
                                                    (w1 >> 24) & 0xff, " compr" if w >> 10 & 1 else "",
                                                    " done" if w >> 11 & 1 else "", " vm" if w >> 12 & 1 else "")
        if c in ("VOP3",) and 255 in [w1 & 0x1ff, (w1 >> 9) & 0x1ff, (w1 >> 18) & 0x1ff]:
            uses_lit = True  # (gfx7 VOP3 takes no literal; shown as such if it seems to)
        n = nw + (1 if uses_lit else 0)
        out.append("%5x: %-36s %s" % (base + 4 * i, " ".join("%08x" % x for x in words[i:i + n]), txt))
        i += n
        if c == "SOPP" and name(c, (w >> 16) & 0x7f) == "s_endpgm":
            out.append("       (s_endpgm)")
    return out


def main():
    tables = json.load(open(CACHE)) if os.path.exists(CACHE) else build_tables()
    data = open(sys.argv[1], "rb").read()
    off = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    ln = int(sys.argv[3], 0) if len(sys.argv) > 3 else len(data) - off
    print("\n".join(dis(data[off:off + ln], off, tables)))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main()
