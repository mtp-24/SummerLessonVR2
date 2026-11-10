#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The engine's TAA pixel shaders from an agent log (agent.taadump=1, agent/insetpp.c):

    taadump.py <agent.log> <out-prefix>  -> <out-prefix>-<N>.bin (the RHI shader's block as the engine holds it),
                                            <out-prefix>-<N>.s (its GCN code disassembled, gfx7)

Each "taadump N <line> <base64>" line is a piece of shader N's block. Its GCN code starts at the
`s_mov_b32 s107, <imm>` every one of these shaders begins with (+0xa4 in Hikari's blocks, +0xa0 in
Chisato's and Allison's: their blocks list one resource fewer before it) and ends at the last
s_endpgm before the shader binary's info block (the 7 bytes "OrbShdr"); the stage registers
(PGM_LO, PGM_HI, RSRC1, RSRC2, ...) sit at +0x3c. Disassembled with tools/gcn7dis.py (LLVM's
tables; LLVM itself no longer disassembles gfx7). The code's FNV-1a and the registers are printed
for agent/taa_patch.h (tools/gen_taa_patch.py)."""
import base64, json, os, re, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gcn7dis


def fnv(b):
    h = 0x811c9dc5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xffffffff
    return h


def main(log, prefix):
    parts, block = {}, {}
    for line in open(log, errors="replace"):
        b = re.search(r"taadump (\d+) begin \d+ bytes \((the RHI shader's block|the resource's)", line)
        if b:  # (the stage registers are at +0x3c of the RHI block only, not in the bytecode)
            block[int(b.group(1))] = b.group(2).startswith("the RHI")
        m = re.search(r"taadump (\d+) (\d+) ([A-Za-z0-9+/=]+)\s*$", line)
        if m:
            parts.setdefault(int(m.group(1)), {})[int(m.group(2))] = base64.b64decode(m.group(3))
    tables = json.load(open(gcn7dis.CACHE)) if os.path.exists(gcn7dis.CACHE) else gcn7dis.build_tables()
    for s, p in sorted(parts.items()):
        data = b"".join(p[i] for i in sorted(p))
        missing = [i for i in range(max(p) + 1) if i not in p]
        open("%s-%d.bin" % (prefix, s), "wb").write(data)
        print("shader %d: %d bytes%s" % (s, len(data), (", lines missing: %s" % missing[:10]) if missing else ""))
        start = data.find(b"\xff\x03\xeb\xbe")  # s_mov_b32 s107, <literal>
        info = data.find(b"OrbShdr", start)
        if start < 0:
            print("  no s_mov_b32 s107 start: not disassembled")
            continue
        if info < start:  # (an older dump without the info block: the code up to the last s_endpgm)
            info = len(data)
        end = data.rfind(b"\x00\x00\x81\xbf", start, info)  # the last s_endpgm before the info block
        if end < 0:
            print("  no s_endpgm before the info block")
            continue
        code = data[start:end + 4]
        if not block.get(s, True):
            print("  the code at +0x%x, %d bytes, FNV-1a 0x%08x (the bytecode: no stage registers)" % (start, len(code), fnv(code)))
        else:
            regs = struct.unpack_from("<12I", data, 0x3c)
            print("  the code at +0x%x, %d bytes, FNV-1a 0x%08x; PGM 0x%x 0x%x RSRC1 0x%08x (VGPRs %d, SGPRs %d) RSRC2 0x%x, "
              "colour %u, inputs 0x%x" % (start, len(code), fnv(code), regs[0], regs[1], regs[2], ((regs[2] & 0x3f) + 1) * 4,
                                          (((regs[2] >> 6) & 0xf) + 1) * 8, regs[3], regs[5], regs[6]))
        out = "%s-%d.s" % (prefix, s)
        open(out, "w").write("\n".join(gcn7dis.dis(code, start, tables)) + "\n")
        print("  disassembled -> %s" % out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
