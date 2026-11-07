#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opcode table for rdna2gcn.c: each instruction it handles, with its gfx1013 and gfx7 opcode
numbers, read back from llvm-mc encodings (so no number is typed by hand). Source counts and data
widths are read from the same assembly templates.

Usage: tools/gen_rdna2gcn_tables.py > payload/rdna2gcn_tables.h
"""
import os
import re
import subprocess
import sys

MC = os.environ.get("LLVM_MC", "llvm-mc")


def words(cpu, text):
    args = [MC, "-triple=amdgcn", "-mcpu=" + cpu, "-show-encoding"]
    if cpu.startswith("gfx10"):
        args.append("-mattr=+wavefrontsize64")
    r = subprocess.run(args, input=text + "\n", capture_output=True, text=True)
    m = re.search(r"encoding: \[(.*)\]", r.stdout)
    if not m:
        raise SystemExit("%s: %r: %s" % (cpu, text, r.stderr.strip()))
    b = [int(x, 16) for x in m.group(1).split(",")]
    return [b[i] | b[i + 1] << 8 | b[i + 2] << 16 | b[i + 3] << 24 for i in range(0, len(b), 4)]


FIELD = {
    "SOP1": (lambda w: (w[0] >> 8) & 0xff, lambda w: (w[0] >> 8) & 0xff),
    "SOP2": (lambda w: (w[0] >> 23) & 0x7f, lambda w: (w[0] >> 23) & 0x7f),
    "SOPC": (lambda w: (w[0] >> 16) & 0x7f, lambda w: (w[0] >> 16) & 0x7f),
    "SOPK": (lambda w: (w[0] >> 23) & 0x1f, lambda w: (w[0] >> 23) & 0x1f),
    "SOPP": (lambda w: (w[0] >> 16) & 0x7f, lambda w: (w[0] >> 16) & 0x7f),
    "SMEM": (lambda w: (w[0] >> 18) & 0xff, lambda w: (w[0] >> 22) & 0x1f),
    "VOP1": (lambda w: (w[0] >> 9) & 0xff, lambda w: (w[0] >> 9) & 0xff),
    "VOP2": (lambda w: (w[0] >> 25) & 0x3f, lambda w: (w[0] >> 25) & 0x3f),
    "VOPC": (lambda w: (w[0] >> 17) & 0xff, lambda w: (w[0] >> 17) & 0xff),
    "VOP3": (lambda w: (w[0] >> 16) & 0x3ff, lambda w: (w[0] >> 17) & 0x1ff),
    "MIMG": (lambda w: (w[0] >> 18) & 0xff, lambda w: (w[0] >> 18) & 0x7f),
    "MUBUF": (lambda w: (w[0] >> 18) & 0xff, lambda w: (w[0] >> 18) & 0x7f),
    "DS": (lambda w: (w[0] >> 18) & 0xff, lambda w: (w[0] >> 18) & 0xff),
}

# row flags (see rdna2gcn.c)
F_CMPX = 1    # v_cmpx: RDNA writes EXEC only, gfx7 also an SGPR pair
F_NC = 2      # RDNA add/sub without carry-out; the gfx7 op writes one (VOP3b)
F_CARRY = 4   # writes a carry-out on both (VCC in e32, an SGPR pair in VOP3b)
F_COND = 8    # last source is an SGPR pair (cndmask condition, carry-in)
F_SDST = 16   # destination is an SGPR
F_DSREAD = 32 # DS op that returns data

rows = []    # (fmt, name, g10, g7, nsrc, flags, width)
defs = []    # extra (prefix, fmt, name, value) defines


def operands(text):
    return [o.strip() for o in re.split(r",(?![^\[]*\])", text.split(None, 1)[1])] if " " in text else []


def regwidth(o):
    m = re.match(r"[sv]\[(\d+):(\d+)\]", o)
    return int(m.group(2)) - int(m.group(1)) + 1 if m else 1


def add(fmt, name, t10, t7=None, flags=0):
    t7 = t7 or t10
    f10 = FIELD[fmt][0]
    f7 = FIELD[fmt][1]
    ops = operands(re.sub(r" (dmask|dim|unorm|idxen|offset).*", "", t7))
    ndst = 2 if flags & (F_NC | F_CARRY) else 1  # the gfx7 templates name every destination
    nsrc = max(len(ops) - ndst, 0)
    width = 0
    if fmt in ("SMEM", "MUBUF") or (fmt == "DS" and flags & F_DSREAD):
        width = regwidth(ops[0])
    elif fmt == "DS":
        width = regwidth(ops[1])
    rows.append((fmt, name, f10(words("gfx1013", t10)), f7(words("gfx700", t7)), nsrc, flags,
                 width))


def add10(fmt, name, t10):
    """RDNA-only instruction, rewritten by rdna2gcn.c: only its RDNA number is needed."""
    defs.append(("G10", fmt, name, FIELD[fmt][0](words("gfx1013", t10))))


def add7(fmt, name, t7):
    """gfx7 instruction emitted by rdna2gcn.c with no RDNA counterpart in the table."""
    defs.append(("G7", fmt, name, FIELD[fmt][1](words("gfx700", t7))))


def vop1(name, flags=0):
    add("VOP1", name, name + "_e32 v0, v1", flags=flags)
    add("VOP3", name, name + "_e64 v0, v1", flags=flags)


def vop2(name, g7=None, flags=0):
    add("VOP2", name, name + "_e32 v0, v1, v2", (g7 or name) + "_e32 v0, v1, v2", flags=flags)
    add("VOP3", name, name + "_e64 v0, v1, v2", (g7 or name) + "_e64 v0, v1, v2", flags=flags)


def vopc(name):
    if name.startswith("v_cmpx_"):
        add("VOPC", name, name + "_e32 v1, v2", name + "_e32 vcc, v1, v2", F_CMPX)
        add("VOP3", name, name + "_e64 v1, v2", name + "_e64 s[0:1], v1, v2", F_CMPX)
    else:
        add("VOPC", name, name + "_e32 vcc, v1, v2")
        add("VOP3", name, name + "_e64 s[0:1], v1, v2")


# ---- scalar ----
for n in ("s_mov_b32", "s_abs_i32", "s_not_b32", "s_bcnt1_i32_b32", "s_flbit_i32"):
    add("SOP1", n, n + " s0, s1")
for n in ("s_mov_b64", "s_not_b64", "s_and_saveexec_b64"):
    add("SOP1", n, n + " s[0:1], s[2:3]")
add("SOP1", "s_andn1_saveexec_b64", "s_andn1_saveexec_b64 s[0:1], s[2:3]", "s_mov_b64 s[0:1], s[2:3]")
# wave32 lane-mask ops: rewritten as their 64-bit forms (rdna2gcn.c, wave32 to wave64)
add10("SOP1", "s_and_saveexec_b32", "s_and_saveexec_b32 s0, s1")
add7("SOP1", "s_bcnt1_i32_b64", "s_bcnt1_i32_b64 s0, s[2:3]")
for n in ("s_add_i32", "s_add_u32", "s_sub_i32", "s_cselect_b32", "s_bfm_b32", "s_lshl_b32",
          "s_lshr_b32", "s_ashr_i32", "s_mul_i32", "s_min_i32", "s_min_u32", "s_xor_b32",
          "s_and_b32", "s_andn2_b32", "s_or_b32"):
    add("SOP2", n, n + " s0, s1, s2")
for n in ("s_and_b64", "s_cselect_b64", "s_nor_b64", "s_or_b64", "s_andn2_b64", "s_orn2_b64",
          "s_xor_b64"):
    add("SOP2", n, n + " s[0:1], s[2:3], s[4:5]")
add10("SOP2", "s_mul_hi_u32", "s_mul_hi_u32 s0, s1, s2")
for n in ("s_cmp_eq_u32", "s_cmp_lg_u32", "s_cmp_eq_i32", "s_cmp_lg_i32", "s_cmp_ge_u32", "s_cmp_gt_u32", "s_cmp_le_u32",
          "s_cmp_lt_u32", "s_cmp_ge_i32", "s_cmp_gt_i32", "s_cmp_le_i32", "s_cmp_lt_i32"):
    add("SOPC", n, n + " s0, s1")
for n in ("s_movk_i32", "s_addk_i32", "s_cmpk_lt_i32"):
    add("SOPK", n, n + " s0, 0x1")
for n in ("s_branch", "s_cbranch_scc0", "s_cbranch_scc1", "s_cbranch_vccz", "s_cbranch_vccnz",
          "s_cbranch_execz", "s_cbranch_execnz"):
    add("SOPP", n, n + " 1")
for n in ("s_endpgm", "s_barrier"):
    add("SOPP", n, n)
add("SOPP", "s_nop", "s_nop 1")
add("SOPP", "s_waitcnt", "s_waitcnt vmcnt(0)")
add("SOPP", "s_inst_prefetch", "s_inst_prefetch 0x3", "s_nop 0")
for n, reg in (("s_load_dword", "s4"), ("s_load_dwordx2", "s[4:5]"), ("s_load_dwordx4", "s[4:7]"),
               ("s_load_dwordx8", "s[4:11]"), ("s_load_dwordx16", "s[4:19]")):
    add("SMEM", n, "%s %s, s[0:1], 0x4" % (n, reg), "%s %s, s[0:1], 0x1" % (n, reg))
for n, reg in (("s_buffer_load_dword", "s4"), ("s_buffer_load_dwordx2", "s[4:5]")):
    add("SMEM", n, "%s %s, s[0:3], 0x4" % (n, reg), "%s %s, s[0:3], 0x1" % (n, reg))

# ---- vector ALU ----
for n in ("v_cvt_f32_i32", "v_cvt_f32_u32", "v_cvt_u32_f32", "v_exp_f32", "v_log_f32", "v_mov_b32",
          "v_rcp_f32", "v_rcp_iflag_f32", "v_rsq_f32", "v_sqrt_f32", "v_movrels_b32"):
    vop1(n)
add("VOP1", "v_readfirstlane_b32", "v_readfirstlane_b32 s0, v1", flags=F_SDST)
for n in ("v_add_f32", "v_sub_f32", "v_subrev_f32", "v_mul_f32", "v_mac_f32", "v_max_f32",
          "v_min_f32", "v_max_i32", "v_min_i32", "v_min_u32", "v_mul_u32_u24", "v_lshlrev_b32",
          "v_lshrrev_b32", "v_ashrrev_i32", "v_and_b32", "v_xor_b32", "v_or_b32"):
    vop2(n)
for n, g7 in (("v_add_nc_u32", "v_add_i32"), ("v_sub_nc_u32", "v_sub_i32"),
              ("v_subrev_nc_u32", "v_subrev_i32")):
    add("VOP2", n, n + "_e32 v0, v1, v2", g7 + "_e32 v0, vcc, v1, v2", F_NC)
    add("VOP3", n, n + "_e64 v0, v1, v2", g7 + "_e64 v0, s[0:1], v1, v2", F_NC)
add("VOP2", "v_add_co_ci_u32", "v_add_co_ci_u32_e32 v0, vcc, v1, v2, vcc",
    "v_addc_u32_e32 v0, vcc, v1, v2, vcc", F_CARRY | F_COND)
add("VOP3", "v_add_co_ci_u32", "v_add_co_ci_u32_e64 v0, s[0:1], v1, v2, s[2:3]",
    "v_addc_u32_e64 v0, s[0:1], v1, v2, s[2:3]", F_CARRY | F_COND)
add("VOP2", "v_cndmask_b32", "v_cndmask_b32_e32 v0, v1, v2, vcc", flags=F_COND)
add("VOP3", "v_cndmask_b32", "v_cndmask_b32_e64 v0, v1, v2, s[0:1]", flags=F_COND)
add("VOP2", "v_madak_f32", "v_madak_f32 v0, v1, v2, 0x1")
add("VOP2", "v_madmk_f32", "v_madmk_f32 v0, v1, 0x1, v2")
for n in ("v_mad_f32", "v_med3_f32", "v_bfe_u32", "v_mad_u32_u24"):
    add("VOP3", n, n + " v0, v1, v2, v3")
for n in ("v_mul_lo_u32", "v_mul_hi_u32", "v_mul_hi_i32"):
    add("VOP3", n, n + " v0, v1, v2")
add("VOP3", "v_mad_u64_u32", "v_mad_u64_u32 v[0:1], s[0:1], v1, v2, v[4:5]", flags=F_CARRY)
add10("VOP3", "v_readlane_b32", "v_readlane_b32 s0, v1, s2")
add7("VOP2", "v_readlane_b32", "v_readlane_b32 s0, v1, s2")
add10("VOP3", "v_lshl_add_u32", "v_lshl_add_u32 v0, v1, 3, v2")
add10("VOP3", "v_add3_u32", "v_add3_u32 v0, v1, v2, v3")
add10("VOP3", "v_add_lshl_u32", "v_add_lshl_u32 v0, v1, v2, v3")
add10("VOP3", "v_permlanex16_b32", "v_permlanex16_b32 v0, v1, s2, s3")
for t, conds in (("f32", ("f", "lt", "eq", "le", "gt", "lg", "ge", "o", "u", "nge", "nlg", "ngt",
                          "nle", "neq", "nlt", "tru")),
                 ("i32", ("f", "lt", "eq", "le", "gt", "ne", "ge", "t")),
                 ("u32", ("f", "lt", "eq", "le", "gt", "ne", "ge", "t"))):
    for c in conds:
        vopc("v_cmp_%s_%s" % (c, t))
        vopc("v_cmpx_%s_%s" % (c, t))

# ---- memory ----
add("MIMG", "image_sample_lz", "image_sample_lz v[0:3], v[4:5], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D",
    "image_sample_lz v[0:3], v[4:5], s[0:7], s[8:11] dmask:0xf")
add("MIMG", "image_store", "image_store v[0:3], v[4:5], s[0:7] dmask:0xf dim:SQ_RSRC_IMG_2D unorm",
    "image_store v[0:3], v[4:5], s[0:7] dmask:0xf unorm")
add("MUBUF", "buffer_load_dword", "buffer_load_dword v1, v0, s[8:11], 0 idxen")
for n, reg in (("buffer_store_dword", "v1"), ("buffer_store_dwordx2", "v[1:2]")):
    add("MUBUF", n, "%s %s, v0, s[8:11], 0 idxen" % (n, reg))
for n, t in (("ds_read_b32", "v1, v4"), ("ds_read2_b32", "v[0:1], v4 offset0:1 offset1:2"),
             ("ds_read2st64_b32", "v[0:1], v4 offset0:1 offset1:2"),
             ("ds_read2_b64", "v[0:3], v4 offset0:1 offset1:2"), ("ds_read_b128", "v[0:3], v4")):
    add("DS", n, "%s %s" % (n, t), flags=F_DSREAD)
add("DS", "ds_write_b32", "ds_write_b32 v4, v1")
add("DS", "ds_write_b64", "ds_write_b64 v4, v[0:1]")
for n in ("ds_write2_b32", "ds_write2st64_b32"):
    add("DS", n, n + " v4, v1, v2 offset0:1 offset1:2")
add7("DS", "ds_swizzle_b32", "ds_swizzle_b32 v0, v1 offset:0x401f")

FMTS = ["SOP1", "SOP2", "SOPC", "SOPK", "SOPP", "SMEM", "VOP1", "VOP2", "VOPC", "VOP3", "MIMG",
        "MUBUF", "DS"]
out = ["/* generated by tools/gen_rdna2gcn_tables.py from llvm-mc encodings - do not edit */",
       "#pragma once", "", "enum fmt { %s };" % ", ".join(FMTS), "",
       "#define F_CMPX %d\n#define F_NC %d\n#define F_CARRY %d\n#define F_COND %d\n"
       "#define F_SDST %d\n#define F_DSREAD %d" % (F_CMPX, F_NC, F_CARRY, F_COND, F_SDST, F_DSREAD),
       ""]
seen = set()
for (fmt, name, a, b, ns, fl, wd) in rows:
    for pre, val in (("G10", a), ("G7", b)):
        key = "%s_%s_%s" % (pre, fmt, name.upper())
        if key not in seen:
            seen.add(key)
            out.append("#define %s 0x%x" % (key, val))
for (pre, fmt, name, val) in defs:
    out.append("#define %s_%s_%s 0x%x" % (pre, fmt, name.upper(), val))
out.append("")
out.append("struct opmap { unsigned char fmt, nsrc, flags, width; unsigned short g10, g7; };")
out.append("static const struct opmap k_ops[] = {")
for (fmt, name, a, b, ns, fl, wd) in rows:
    out.append("  {%s, %d, %d, %d, 0x%x, 0x%x}, /* %s */" % (fmt, ns, fl, wd, a, b, name))
out.append("};")
out.append("")
out.append("#ifdef R2G_WANT_NAMES /* tools only: the mnemonic of each k_ops row and extra define */")
out.append("static const char *const k_op_names[] = {")
for (fmt, name, a, b, ns, fl, wd) in rows:
    out.append("  \"%s\"," % name)
out.append("};")
out.append("struct opextra { const char *pre; unsigned char fmt; unsigned short op; const char *name; };")
out.append("static const struct opextra k_op_extra[] = {")
for (pre, fmt, name, val) in defs:
    out.append("  {\"%s\", %s, 0x%x, \"%s\"}," % (pre, fmt, val, name))
out.append("};")
out.append("#endif")
print("\n".join(out))
