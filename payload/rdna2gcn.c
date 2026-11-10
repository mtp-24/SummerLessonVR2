/* rdna2gcn: re-encode an RDNA (gfx1013, wave64) compute shader as GCN (gfx7) code.
 *
 * A PS4 (backward-compatible) process is a "gen1" GPU context, which decodes GCN encodings only.
 * libSceHmd2's reprojection shader and the wave64 networks in libSceVrHand's models are RDNA, so
 * the payload translates the console's own copies at run time. The instruction set is the one
 * those shaders use (see tools/gen_rdna2gcn_tables.py); anything else is refused rather than
 * guessed. Opcode numbers are almost all shared (RDNA kept the CI numbering); what changes:
 *  - branches are re-targeted (instruction sizes differ);
 *  - SMEM (byte offsets) -> SMRD (dword immediate, CI 32-bit literal, or an SGPR byte offset);
 *  - SDWA forms (DWORD selects only) -> VOP3 with the same abs/neg/clamp/omod;
 *  - gfx7 VOP3 takes no literal and every VALU op reads one scalar value (SGPR or literal) at
 *    most: the others go through scratch VGPRs first; 1/(2*pi) is no inline constant on gfx7;
 *  - v_cmpx on RDNA writes EXEC only; on gfx7 it also writes VCC (e32) or an SGPR pair (e64):
 *    it becomes e64 with a scratch SGPR pair, so VCC survives;
 *  - RDNA's carry-less v_add/sub/subrev_nc_u32 are gfx7 VOP3b ops whose carry goes to that pair;
 *    v_lshl_add_u32, v_add3_u32 and v_add_lshl_u32 are split in two; v_add_co_ci_u32 is
 *    v_addc_u32; v_readlane_b32 moves to its gfx7 VOP2 form;
 *  - s_mul_hi_u32 (none on gfx7) is v_mul_hi_u32 + v_readfirstlane_b32 with EXEC all ones;
 *  - DPP (full masks, bound_ctrl:1: quad_perm, row_xmask, row_ror:8) and v_permlanex16 (lane ^ 16,
 *    its selects checked) become ds_swizzle_b32; inactive source lanes read 0 as they do on RDNA;
 *  - DS keeps its encoding, and gfx7 LDS ops need M0 = -1, set at entry; MUBUF loses dlc and
 *    gets a gfx7 dword 3 in its V# first (the gfx10 one reads as ATC on gfx7);
 *  - s_andn1_saveexec_b64 gets a gfx7 equivalent; s_inst_prefetch goes; MIMG loses dim;
 *    s_waitcnt is re-packed; s_cbranch_vccz/vccnz first wait for SMEM and rewrite VCC (CI reads a
 *    stale VCCZ after an SMEM load returns);
 *  - "s_nop 4" after a VALU op that writes an SGPR or EXEC (gfx7 has fewer interlocks).
 * Wide LDS ops keep their width: gfx7 wants them naturally aligned, which the addresses in these
 * shaders are (scaled indices, 16-byte constant offsets).
 *
 * wave32 shaders (libSceVrHand's) run as wave64 with EXEC_HI = 0, their lane masks kept 32-bit;
 * VCC_HI, which they use as a scalar, moves to an SGPR of its own (gfx7's VOPC/carry ops write the
 * whole VCC). Their groups of more than 32 threads run one wave64 per wave32 (rdna2gcn_compute).
 *
 * For the Hmd2 shader the output is byte-identical to tools/rdna2gcn.py (the llvm-mc based
 * reference); every libSceVrHand shader's output decodes as clean gfx7 code (opcodes from the
 * llvm-mc table, branch targets, operand rules).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "rdna2gcn.h"

#include <string.h>

#include "rdna2gcn_tables.h"

typedef unsigned int u32;
typedef unsigned long long u64;

#define MAX_INSNS 8192
#ifndef R2G_DEBUG_MAX
#define R2G_DEBUG_MAX(i, v) ((void)0)
#endif
#define LIT 255
#define INV_2PI 0x3e22f983u
#define VCC 106
#define VCC_HI 107
#define M0 124
#define EXEC_LO 126
#define EXEC 126
#define EXEC_HI 127
#define ALL_ONES 193 /* inline -1 */
#define WAIT_LGKM0 0x7f /* gfx7 s_waitcnt lgkmcnt(0) */

struct insn {
  u32 off; /* dwords */
  u32 len;
  u32 w[3];
  int fmt, sdwa, dpp;
};

struct out {
  u32 *p;
  u32 n, cap;
  int err;
};

static void emit(struct out *o, u32 w) {
  if (o->p && o->n < o->cap)
    o->p[o->n] = w;
  else if (o->p)
    o->err = 1;
  o->n++;
}

static int is_sgpr_like(u32 code) { return code < 128; } /* SGPRs, vcc, m0, exec */

static int is_branch(u32 op) {
  return op == G10_SOPP_S_BRANCH || op == G10_SOPP_S_CBRANCH_SCC0 || op == G10_SOPP_S_CBRANCH_SCC1 ||
         op == G10_SOPP_S_CBRANCH_VCCZ || op == G10_SOPP_S_CBRANCH_VCCNZ ||
         op == G10_SOPP_S_CBRANCH_EXECZ || op == G10_SOPP_S_CBRANCH_EXECNZ;
}

static int decode(const u32 *in, u32 n, struct insn *ins, u32 *count, const char **why) {
  u32 i = 0, k = 0;
  while (i < n) {
    if (k >= MAX_INSNS)
      return *why = "too many instructions", -1;
    struct insn *x = &ins[k++];
    u32 w = in[i];
    memset(x, 0, sizeof *x);
    x->off = i;
    x->w[0] = w;
    x->len = 1;
    if ((w >> 23) == 0x17d) {
      x->fmt = SOP1;
      if ((w & 0xff) == LIT)
        x->len = 2;
    } else if ((w >> 23) == 0x17e) {
      x->fmt = SOPC;
      if ((w & 0xff) == LIT || ((w >> 8) & 0xff) == LIT)
        x->len = 2;
    } else if ((w >> 23) == 0x17f) {
      x->fmt = SOPP;
    } else if ((w >> 28) == 0xb) {
      x->fmt = SOPK;
    } else if ((w >> 30) == 2) {
      x->fmt = SOP2;
      if ((w & 0xff) == LIT || ((w >> 8) & 0xff) == LIT)
        x->len = 2;
    } else if ((w >> 26) == 0x3d) {
      x->fmt = SMEM;
      x->len = 2;
    } else if ((w >> 26) == 0x35) {
      x->fmt = VOP3;
      x->len = 2;
    } else if ((w >> 26) == 0x3c) {
      x->fmt = MIMG;
      x->len = 2 + ((w >> 1) & 3); /* NSA: one more dword per four extra address VGPRs */
      if (x->len > 3)
        return *why = "MIMG NSA with more than 4 extra addresses", -1;
    } else if ((w >> 26) == 0x38) {
      x->fmt = MUBUF;
      x->len = 2;
    } else if ((w >> 26) == 0x36) {
      x->fmt = DS;
      x->len = 2;
    } else if ((w >> 25) == 0x3f || (w >> 25) == 0x3e || (w >> 31) == 0) {
      x->fmt = (w >> 25) == 0x3f ? VOP1 : (w >> 25) == 0x3e ? VOPC : VOP2;
      u32 src0 = w & 0x1ff;
      u32 op = x->fmt == VOP2 ? (w >> 25) & 0x3f : 0;
      if (src0 == 249) {
        x->sdwa = 1;
        x->len = 2;
      } else if (src0 == 250) {
        x->dpp = 1;
        x->len = 2;
      } else if (src0 == 233 || src0 == 234) {
        return *why = "DPP8 is not supported", -1;
      } else if (src0 == LIT || (x->fmt == VOP2 && (op == G10_VOP2_V_MADAK_F32 ||
                                                   op == G10_VOP2_V_MADMK_F32))) {
        x->len = 2;
      }
    } else {
      return *why = "unknown encoding", -1;
    }
    if (x->fmt == VOP3 && i + 1 < n) { /* (a header as the last dword: truncated, below) */
      u32 w1 = in[i + 1];
      if ((w1 & 0x1ff) == LIT || ((w1 >> 9) & 0x1ff) == LIT || ((w1 >> 18) & 0x1ff) == LIT)
        x->len = 3;
    }
    if (i + x->len > n)
      return *why = "truncated instruction", -1;
    for (u32 j = 1; j < x->len; j++)
      x->w[j] = in[i + j];
    i += x->len;
    if (x->fmt == SOPP && ((w >> 16) & 0x7f) == G10_SOPP_S_ENDPGM)
      break;
  }
  *count = k;
  return 0;
}

static const struct opmap *find(int fmt, u32 g10) {
  for (unsigned i = 0; i < sizeof k_ops / sizeof k_ops[0]; i++)
    if (k_ops[i].fmt == fmt && k_ops[i].g10 == g10)
      return &k_ops[i];
  return 0;
}

static const struct opmap *map_op(int fmt, u32 g10, const char **why) {
  const struct opmap *m = find(fmt, g10);
  if (!m)
    *why = "instruction not in the table";
  return m;
}

/* ---- gfx7 encoders ---- */
static void e_sop1(struct out *o, u32 op, u32 sdst, u32 s0) {
  emit(o, (0x17du << 23) | (sdst << 16) | (op << 8) | s0);
}

static void e_sopp(struct out *o, u32 op, u32 imm) { emit(o, (0x17fu << 23) | (op << 16) | imm); }

static void e_vop1(struct out *o, u32 op, u32 vdst, u32 src0, u32 lit) {
  emit(o, (0x3fu << 25) | ((vdst & 0xff) << 17) | (op << 9) | src0);
  if (src0 == LIT)
    emit(o, lit);
}

/* VOP3a (vdst may be an SGPR code for VOPC) */
static void e_vop3a(struct out *o, u32 op, u32 vdst, u32 abs, u32 clamp, u32 s0, u32 s1, u32 s2,
                    u32 omod, u32 neg) {
  emit(o, (0x34u << 26) | (op << 17) | (clamp << 11) | (abs << 8) | (vdst & 0xff));
  emit(o, (neg << 29) | (omod << 27) | (s2 << 18) | (s1 << 9) | s0);
}

/* VOP3b: vector dst + SGPR-pair dst (carry) */
static void e_vop3b(struct out *o, u32 op, u32 vdst, u32 sdst, u32 s0, u32 s1, u32 s2, u32 omod,
                    u32 neg) {
  emit(o, (0x34u << 26) | (op << 17) | (sdst << 8) | (vdst & 0xff));
  emit(o, (neg << 29) | (omod << 27) | (s2 << 18) | (s1 << 9) | s0);
}

static void e_nop4(struct out *o) { emit(o, 0xbf800004u); }

static void e_vop2(struct out *o, u32 op, u32 vdst, u32 src0, u32 vsrc1, u32 lit) {
  emit(o, (op << 25) | ((vdst & 0xff) << 17) | ((vsrc1 & 0xff) << 9) | src0);
  if (src0 == LIT)
    emit(o, lit);
}

static u32 int_src(u32 v) { return v <= 64 ? 128 + v : LIT; } /* inline integer or literal */

struct ctx {
  u32 sv[3];  /* scratch VGPR codes (256 + n) */
  u32 ss, ss2; /* scratch SGPR pairs (codes) */
  int m0;      /* the shader touches LDS (or swizzles): M0 = -1 at entry */
  int wave32;  /* a wave32 shader run as wave64 with only lanes 0..31 ever on (EXEC_HI = 0) */
  u32 vhi;     /* a wave32 shader uses VCC_HI as a free scalar temp; it moves here (gfx7's
                * v_cmp_e32 and carry ops would clobber the real VCC_HI) */
  int fix_vsharp; /* s_buffer_load V#s get a gfx7 dword 3 (compute shaders with a header) */
  /* split wave32 group: hardware thread x (0 .. 2 * threads) -> the wave32 thread's x/y/z */
  int split;
  u32 tidig, log2x, log2xy, maskx, masky;
  const unsigned char *tgt; /* instruction index -> some branch lands there */
};

/* One VALU op in VOP3 form under gfx7 rules: a literal never stays in VOP3, and only one
 * distinct scalar (SGPR/literal) is read; the cndmask condition counts as that scalar. */
static void valu3(struct out *o, const struct ctx *c, u32 op7, u32 vdst, u32 abs, u32 clamp,
                  u32 omod, u32 neg, u32 nsrc, u32 *src, const u32 *lit, int cndmask, int vop3b,
                  u32 sdst) {
  u32 k = 0;
  int used = 0;
  u32 used_code = 0;
  for (u32 i = 0; i < nsrc; i++) {
    u32 s = src[i];
    int literal = s == LIT || s == 248;
    if (!literal && !is_sgpr_like(s))
      continue;
    if (cndmask && i == nsrc - 1)
      continue; /* the condition pair */
    if (literal || (used && used_code != s)) {
      e_vop1(o, G7_VOP1_V_MOV_B32, c->sv[k] - 256, literal ? LIT : s, s == 248 ? INV_2PI : lit[i]);
      src[i] = c->sv[k++];
      continue;
    }
    used = 1;
    used_code = s;
  }
  if (cndmask && nsrc == 3) {
    u32 cond = src[2];
    for (u32 i = 0; i < 2; i++)
      if (is_sgpr_like(src[i]) && src[i] != cond) {
        e_vop1(o, G7_VOP1_V_MOV_B32, c->sv[k] - 256, src[i], 0);
        src[i] = c->sv[k++];
      }
  }
  u32 s0 = nsrc > 0 ? src[0] : 0, s1 = nsrc > 1 ? src[1] : 0, s2 = nsrc > 2 ? src[2] : 0;
  if (vop3b)
    e_vop3b(o, op7, vdst, sdst, s0, s1, s2, omod, neg);
  else
    e_vop3a(o, op7, vdst, abs, clamp, s0, s1, s2, omod, neg);
}

/* vdst = src (VGPR code) as read from lane f(lane) by ds_swizzle_b32 pattern pat. RDNA's DPP and
 * permlane (bound_ctrl, no fetch-inactive) read 0 from an inactive lane; ds_swizzle reads the
 * lane's register, so a copy with those lanes zeroed is swizzled. Uses sv[2] and ss2. */
static void swizzle(struct out *o, const struct ctx *c, u32 vdst, u32 src, u32 pat) {
  u32 t = c->sv[2];
  e_sop1(o, G7_SOP1_S_MOV_B64, c->ss2, EXEC);
  e_sop1(o, G7_SOP1_S_MOV_B64, EXEC, ALL_ONES);
  e_vop3a(o, G7_VOP3_V_CNDMASK_B32, t - 256, 0, 0, 128, src, c->ss2, 0, 0); /* ss2 ? src : 0 */
  e_sop1(o, G7_SOP1_S_MOV_B64, EXEC, c->ss2);
  emit(o, (0x36u << 26) | (G7_DS_DS_SWIZZLE_B32 << 18) | pat);
  emit(o, ((vdst & 0xff) << 24) | ((t - 256) & 0xff));
  e_sopp(o, G7_SOPP_S_WAITCNT, WAIT_LGKM0);
}

/* A wave32 group of more than 32 threads runs as twice as many hardware threads, one wave64 per
 * wave32 with only its low 32 lanes on: hardware thread x = 64w + l (l < 32) is wave32 thread
 * 32w + l, whose ids the shader gets in v0/v1/v2 as on RDNA (groups of powers of two). */
static void split_ids(struct out *o, const struct ctx *c) {
  u32 a = c->sv[0] - 256, b = c->sv[1] - 256;
  e_vop2(o, G7_VOP2_V_AND_B32, a, 128 + 31, 0, 0);           /* a = l */
  e_vop2(o, G7_VOP2_V_LSHRREV_B32, b, 128 + 6, 0, 0);        /* b = w */
  e_vop2(o, G7_VOP2_V_LSHLREV_B32, b, 128 + 5, b, 0);        /* b = 32w */
  e_vop2(o, G7_VOP2_V_OR_B32, a, 256 + b, a, 0);             /* a = 32w + l */
  if (c->tidig >= 2)
    e_vop2(o, G7_VOP2_V_LSHRREV_B32, 2, int_src(c->log2xy), a, c->log2xy);
  if (c->tidig >= 1) {
    e_vop2(o, G7_VOP2_V_LSHRREV_B32, 1, int_src(c->log2x), a, c->log2x);
    e_vop2(o, G7_VOP2_V_AND_B32, 1, int_src(c->masky), 1, c->masky);
  }
  e_vop2(o, G7_VOP2_V_AND_B32, 0, int_src(c->maskx), a, c->maskx);
}

/* A wave32 lane mask is one SGPR; gfx7 VALU ops write a pair. A mask going anywhere but VCC
 * (whose high half is free here: c->vhi holds the shader's VCC_HI) is written through the
 * scratch pair and its low half copied to the SGPR, so the next SGPR keeps its value. */
static u32 mask_dst(const struct ctx *c, u32 d) { return c->wave32 && d != VCC ? c->ss : d; }
static void mask_copy(struct out *o, const struct ctx *c, u32 d) {
  if (c->wave32 && d != VCC)
    e_sop1(o, G7_SOP1_S_MOV_B32, d, c->ss);
}

/* may x write SGPR r? (unknown -> yes) */
static int writes_sgpr(const struct insn *x, u32 r) {
  u32 w = x->w[0], d;
  const struct opmap *m;
  switch (x->fmt) {
  case SOP1:
  case SOP2:
  case SOPK:
    d = (w >> 16) & 0x7f;
    return r == d || r == d + 1;
  case SMEM:
    d = (w >> 6) & 0x7f;
    return r >= d && r < d + 16;
  case SOPC:
  case SOPP:
  case MIMG:
  case MUBUF:
  case DS:
    return 0;
  case VOP1:
    m = find(VOP1, (w >> 9) & 0xff);
    return !m || ((m->flags & F_SDST) && r == ((w >> 17) & 0xff));
  case VOP2:
    m = find(VOP2, (w >> 25) & 0x3f);
    return !m || ((m->flags & F_CARRY) && (r == VCC || r == VCC + 1));
  case VOPC:
    d = x->sdwa && ((x->w[1] >> 15) & 1) ? (x->w[1] >> 8) & 0x7f : VCC;
    return r == d || r == d + 1;
  case VOP3: {
    u32 op = (w >> 16) & 0x3ff;
    if (op < 0x100 || op == G10_VOP3_V_READLANE_B32)
      return r == (w & 0xff) || r == (w & 0xff) + 1;
    if (op == G10_VOP3_V_LSHL_ADD_U32 || op == G10_VOP3_V_ADD3_U32 ||
        op == G10_VOP3_V_ADD_LSHL_U32 || op == G10_VOP3_V_PERMLANEX16_B32)
      return 0;
    m = find(VOP3, op);
    if (m && (m->flags & F_CARRY))
      return r == ((w >> 8) & 0x7f) || r == ((w >> 8) & 0x7f) + 1;
    return !m;
  }
  }
  return 1;
}

/* SGPR r holds val when instruction i runs: an s_mov_b32 r, val before it on the only path */
static int sgpr_holds(const struct insn *ins, u32 i, const unsigned char *tgt, u32 r, u32 val) {
  while (i > 0) {
    if (tgt[i])
      return 0; /* a branch can land here with anything in r */
    const struct insn *x = &ins[--i];
    if (writes_sgpr(x, r))
      return x->fmt == SOP1 && ((x->w[0] >> 8) & 0xff) == G10_SOP1_S_MOV_B32 &&
             ((x->w[0] >> 16) & 0x7f) == r && (x->w[0] & 0xff) == LIT && x->w[1] == val;
  }
  return 0;
}

static int xlate_dpp(struct out *o, const struct ctx *c, const struct insn *x,
                     const struct opmap *m, const char **why) {
  u32 w = x->w[0], d = x->w[1];
  u32 ctrl = (d >> 8) & 0x1ff, pat;
  /* row/bank masks 0xf, bound_ctrl:1, no fetch-inactive, no neg/abs */
  if ((d >> 24) != 0xff || ((d >> 18) & 0x3f) != 2)
    return *why = "DPP form other than full masks and bound_ctrl", -1;
  if (ctrl < 0x100)
    pat = 0x8000 | ctrl; /* quad_perm: the same selector layout */
  else if (ctrl >= 0x160 && ctrl <= 0x16f)
    pat = 0x1f | ((ctrl & 0xf) << 10); /* row_xmask:n = lane ^ n */
  else if (ctrl == 0x128)
    pat = 0x1f | (8u << 10); /* row_ror:8 = lane ^ 8 */
  else
    return *why = "DPP control other than quad_perm, row_xmask, row_ror:8", -1;
  u32 vdst = (w >> 17) & 0xff, src0 = 256 + (d & 0xff);
  if (x->fmt == VOP1) {
    if (m->g7 != G7_VOP1_V_MOV_B32)
      return *why = "DPP on a VOP1 op other than v_mov_b32", -1;
    swizzle(o, c, vdst, src0, pat);
    return 0;
  }
  u32 op = (w >> 25) & 0x3f;
  if (x->fmt != VOP2 || (m->flags & (F_NC | F_CARRY | F_COND)) || op == G10_VOP2_V_MADAK_F32 ||
      op == G10_VOP2_V_MADMK_F32)
    return *why = "DPP on this op", -1;
  swizzle(o, c, c->sv[2] - 256, src0, pat);
  emit(o, (m->g7 << 25) | (vdst << 17) | (((w >> 9) & 0xff) << 9) | c->sv[2]);
  return 0;
}

/* Translate one instruction. map[] holds output offsets of source instructions (for branches;
 * filled by the caller between passes). */
static int xlate(struct out *o, const struct ctx *c, const struct insn *x, const u32 *map,
                 const struct insn *ins, u32 nins, const char **why) {
  u32 w = x->w[0];
  u32 op7;
  const struct opmap *m;
  if (x == ins && c->m0)
    e_sop1(o, G7_SOP1_S_MOV_B32, M0, ALL_ONES);
  if (x == ins && c->wave32)
    e_sop1(o, G7_SOP1_S_MOV_B32, EXEC_HI, 128); /* lanes 32..63 stay off: one wave of <=32 */
  if (x == ins && c->split)
    split_ids(o, c);
  switch (x->fmt) {
  case SOP1: {
    u32 op = (w >> 8) & 0xff;
    if (c->wave32 && op == G10_SOP1_S_AND_SAVEEXEC_B32) {
      /* no gfx7 b32 form, and the operands are single SGPRs (widening to b64 would touch a
       * neighbour): D = EXEC_LO; EXEC_LO = S0 & EXEC_LO; SCC = EXEC_LO != 0. EXEC_HI stays 0. */
      u32 d = (w >> 16) & 0x7f, s0 = w & 0xff;
      u32 t = c->ss2; /* scratch (c->vhi holds the shader's VCC_HI) */
      e_sop1(o, G7_SOP1_S_MOV_B32, t, EXEC_LO);
      emit(o, (2u << 30) | (G7_SOP2_S_AND_B32 << 23) | ((u32)EXEC_LO << 16) | (s0 << 8) | EXEC_LO);
      e_sop1(o, G7_SOP1_S_MOV_B32, d, t);
      return 0;
    }
    if (op == G10_SOP1_S_ANDN1_SAVEEXEC_B64) {
      /* D = EXEC; EXEC = ~S0 & EXEC; SCC = EXEC != 0 */
      u32 d = (w >> 16) & 0x7f, s0 = w & 0xff;
      e_sop1(o, G7_SOP1_S_MOV_B64, c->ss2, EXEC);
      emit(o, (2u << 30) | (G7_SOP2_S_ANDN2_B64 << 23) | ((u32)EXEC << 16) | (s0 << 8) | EXEC);
      e_sop1(o, G7_SOP1_S_MOV_B64, d, c->ss2);
      return 0;
    }
    if (!(m = map_op(SOP1, op, why)))
      return -1;
    emit(o, (w & ~(0xffu << 8)) | (m->g7 << 8));
    if (x->len > 1)
      emit(o, x->w[1]);
    break;
  }
  case SOP2: {
    u32 op = (w >> 23) & 0x7f;
    if (op == G10_SOP2_S_MUL_HI_U32) {
      /* no SALU form on gfx7: one lane of v_mul_hi_u32 (all lanes on, so lane 0 is) */
      u32 src[2] = {w & 0xff, (w >> 8) & 0xff}, lit[2] = {x->w[1], x->w[1]};
      e_sop1(o, G7_SOP1_S_MOV_B64, c->ss2, EXEC);
      e_sop1(o, G7_SOP1_S_MOV_B64, EXEC, ALL_ONES);
      valu3(o, c, G7_VOP3_V_MUL_HI_U32, c->sv[2] - 256, 0, 0, 0, 0, 2, src, lit, 0, 0, 0);
      e_vop1(o, G7_VOP1_V_READFIRSTLANE_B32, (w >> 16) & 0x7f, c->sv[2], 0);
      e_sop1(o, G7_SOP1_S_MOV_B64, EXEC, c->ss2);
      e_nop4(o);
      return 0;
    }
    if (!(m = map_op(SOP2, op, why)))
      return -1;
    emit(o, (w & ~(0x7fu << 23)) | (m->g7 << 23));
    if (x->len > 1)
      emit(o, x->w[1]);
    break;
  }
  case SOPC:
    if (!(m = map_op(SOPC, (w >> 16) & 0x7f, why)))
      return -1;
    emit(o, (w & ~(0x7fu << 16)) | (m->g7 << 16));
    if (x->len > 1)
      emit(o, x->w[1]);
    break;
  case SOPK:
    if (!(m = map_op(SOPK, (w >> 23) & 0x1f, why)))
      return -1;
    emit(o, (w & ~(0x1fu << 23)) | (m->g7 << 23));
    break;
  case SOPP: {
    u32 op = (w >> 16) & 0x7f, imm = w & 0xffff;
    if (op == G10_SOPP_S_INST_PREFETCH)
      return 0;
    if (!(m = map_op(SOPP, op, why)))
      return -1;
    op7 = m->g7;
    if (op == G10_SOPP_S_WAITCNT) {
      u32 vm = (imm & 0xf) | (((imm >> 14) & 3) << 4), ex = (imm >> 4) & 7, lg = (imm >> 8) & 0x3f;
      if (vm > 15)
        vm = 15;
      if (lg > 15)
        lg = 15;
      imm = vm | (ex << 4) | (lg << 8);
    } else if (is_branch(op)) {
      int simm = (int)(short)imm;
      u32 tgt = x->off + 1 + (u32)simm;
      u32 j = 0;
      while (j < nins && ins[j].off != tgt)
        j++;
      if (j == nins)
        return *why = "branch into an instruction", -1;
      if (op == G10_SOPP_S_CBRANCH_VCCZ || op == G10_SOPP_S_CBRANCH_VCCNZ) {
        e_sopp(o, G7_SOPP_S_WAITCNT, WAIT_LGKM0);
        e_sop1(o, G7_SOP1_S_MOV_B64, VCC, VCC);
      }
      if (map) {
        int rel = (int)map[j] - (int)(o->n + 1);
        if (rel < -32768 || rel > 32767)
          return *why = "branch out of range", -1;
        imm = (u32)rel & 0xffff;
      }
    }
    e_sopp(o, op7, imm);
    break;
  }
  case SMEM: {
    u32 w1 = x->w[1];
    u32 op = (w >> 18) & 0xff, sbase = w & 0x3f, sdata = (w >> 6) & 0x7f;
    u32 off = w1 & 0x1fffff, soff = (w1 >> 25) & 0x7f;
    if (!(m = map_op(SMEM, op, why)))
      return -1;
    u32 d3 = sbase * 2 + 3; /* a scalar buffer load's V#: dword 3 as MUBUF's */
    int vsharp = c->fix_vsharp && op >= G10_SMEM_S_BUFFER_LOAD_DWORD;
    if (vsharp) {
      e_sop1(o, G7_SOP1_S_MOV_B32, c->ss, d3); /* c->ss2 may carry the offset below */
      e_sop1(o, G7_SOP1_S_MOV_B32, d3, LIT);
      emit(o, 0x00027facu);
    }
    u32 head = (0x18u << 27) | (m->g7 << 22) | (sdata << 15) | (sbase << 9);
    if (soff != 125 && off) {
      /* SGPR + immediate byte offset: gfx7 SMRD has one or the other. Add them in a scratch
       * SGPR; s_add_u32 writes SCC, which is saved and put back around it. */
      u32 sa = c->ss2, sb = c->ss2 + 1;
      emit(o, (2u << 30) | (G7_SOP2_S_CSELECT_B32 << 23) | (sa << 16) | (128u << 8) | ALL_ONES);
      if (off <= 64) {
        emit(o, (2u << 30) | (G7_SOP2_S_ADD_U32 << 23) | (sb << 16) | ((128 + off) << 8) | soff);
      } else {
        emit(o, (2u << 30) | (G7_SOP2_S_ADD_U32 << 23) | (sb << 16) | (LIT << 8) | soff);
        emit(o, off);
      }
      emit(o, (0x17eu << 23) | (G7_SOPC_S_CMP_LG_U32 << 16) | (128u << 8) | sa);
      emit(o, head | sb);
    } else if (soff != 125) { /* SGPR byte offset */
      emit(o, head | soff);
    } else if (off & 3) {
      return *why = "unaligned SMEM offset", -1;
    } else if (off / 4 <= 255) {
      emit(o, head | (1u << 8) | (off / 4));
    } else {
      emit(o, head | 0xff);
      emit(o, off / 4);
    }
    if (vsharp && !(sdata <= d3 && d3 < sdata + m->width))
      e_sop1(o, G7_SOP1_S_MOV_B32, d3, c->ss); /* back, unless the load replaces it */
    if (c->wave32 && sdata <= VCC_HI && VCC_HI < sdata + m->width) {
      /* a wide load into VCC: its high dword is the scalar that lives in c->vhi here */
      e_sopp(o, G7_SOPP_S_WAITCNT, WAIT_LGKM0);
      e_sop1(o, G7_SOP1_S_MOV_B32, c->vhi, VCC_HI);
    }
    break;
  }
  case MIMG: {
    u32 dim = (w >> 3) & 7;
    if (dim > 1)
      return *why = "MIMG dimension other than 1D/2D", -1;
    u32 op = (w >> 18) & 0x7f;
    if (!(m = map_op(MIMG, op, why)))
      return -1;
    u32 w1 = x->w[1];
    if (x->len == 3) {
      /* NSA (gfx10 addresses in any VGPRs): gfx7 needs them consecutive, so copy them to the
       * scratch VGPRs first. 1D/2D: one address per dimension, as below. */
      for (u32 k = 0; k <= dim; k++) {
        u32 a = k ? (x->w[2] >> (8 * (k - 1))) & 0xff : w1 & 0xff;
        e_vop1(o, G7_VOP1_V_MOV_B32, c->sv[k] - 256, 256 + a, 0);
      }
      w1 = (w1 & ~0xffu) | (c->sv[0] - 256);
    }
    u32 g7 = w & ~((7u << 3) | (3u << 1) | (1u << 7) | (0x7fu << 18));
    emit(o, g7 | (m->g7 << 18));
    emit(o, w1);
    break;
  }
  case MUBUF: {
    /* same fields; RDNA's dlc bit is gfx7's addr64 */
    if (((w >> 16) & 1) || ((x->w[1] >> 23) & 1))
      return *why = "MUBUF lds/tfe", -1;
    if (!(m = map_op(MUBUF, (w >> 18) & 0xff, why)))
      return -1;
    /* The V#'s dword 3 is laid out differently on gfx10 (FORMAT/RESOURCE_LEVEL/OOB_SELECT);
     * read as gfx7 its raw buffers have ATC set. These are untyped dword accesses, so dword 3
     * only has to say "32-bit float, xyzw, no ATC": written before every use (the V# may be
     * reloaded), with s_mov so SCC is untouched. Its s_load has been waited for already. */
    /* The shader's own dword 3 is put back after (it may read the SGPR again). */
    u32 d3 = ((x->w[1] >> 16) & 0x1f) * 4 + 3;
    e_sop1(o, G7_SOP1_S_MOV_B32, c->ss2, d3);
    e_sop1(o, G7_SOP1_S_MOV_B32, d3, LIT);
    emit(o, 0x00027facu);
    emit(o, (w & ~((1u << 15) | (0xffu << 18))) | (m->g7 << 18));
    emit(o, x->w[1]);
    e_sop1(o, G7_SOP1_S_MOV_B32, d3, c->ss2);
    break;
  }
  case DS:
    if ((w >> 17) & 1)
      return *why = "GDS", -1;
    if (!(m = map_op(DS, (w >> 18) & 0xff, why)))
      return -1;
    emit(o, (w & ~(0xffu << 18)) | (m->g7 << 18));
    emit(o, x->w[1]);
    break;
  case VOP1:
  case VOP2:
  case VOPC: {
    int fmt = x->fmt;
    u32 op = fmt == VOP1 ? (w >> 9) & 0xff : fmt == VOP2 ? (w >> 25) & 0x3f : (w >> 17) & 0xff;
    u32 vdst = (w >> 17) & 0xff, vsrc1 = 256 + ((w >> 9) & 0xff), src0 = w & 0x1ff;
    if (!(m = map_op(fmt, op, why)))
      return -1;
    op7 = m->g7;
    if (x->dpp)
      return xlate_dpp(o, c, x, m, why);
    u32 op3 = fmt == VOP1 ? 0x180 + op7 : fmt == VOP2 ? 0x100 + op7 : op7;
    int cmpx = (m->flags & F_CMPX) != 0, nc = fmt == VOP2 && (m->flags & F_NC);
    if (x->sdwa) {
      u32 s = x->w[1];
      if (((s >> 8) & 7) != 6 && fmt != VOPC)
        return *why = "SDWA dst_sel other than DWORD", -1;
      if (((s >> 16) & 7) != 6 || (fmt != VOP1 && ((s >> 24) & 7) != 6))
        return *why = "SDWA src_sel other than DWORD", -1;
      if (fmt != VOPC && ((s >> 11) & 3) != 0)
        return *why = "SDWA dst_unused other than UNUSED_PAD", -1;
      if (m->flags & F_CARRY)
        return *why = "SDWA carry op", -1;
      u32 a0 = (s >> 21) & 1, n0 = (s >> 20) & 1, a1 = (s >> 29) & 1, n1 = (s >> 28) & 1;
      u32 s0 = (s & 0xff) + (((s >> 23) & 1) ? 0 : 256);
      u32 s1 = ((s >> 31) & 1) ? ((w >> 9) & 0xff) : vsrc1;
      u32 clamp = fmt == VOPC ? 0 : (s >> 13) & 1, omod = fmt == VOPC ? 0 : (s >> 14) & 3;
      u32 src[3] = {s0, s1, VCC}, lit[3] = {0, 0, 0};
      if (fmt == VOP1) {
        valu3(o, c, op3, vdst, a0, clamp, omod, n0, 1, src, lit, 0, 0, 0);
      } else if (fmt == VOP2) {
        int cnd = (m->flags & F_COND) != 0;
        valu3(o, c, op3, vdst, a0 | (a1 << 1), clamp, omod, n0 | (n1 << 1), cnd ? 3 : 2, src,
              lit, cnd, nc, c->ss);
        if (nc)
          e_nop4(o);
      } else {
        u32 sdst = ((s >> 15) & 1) ? (s >> 8) & 0x7f : VCC;
        valu3(o, c, op3, cmpx ? c->ss : mask_dst(c, sdst), a0 | (a1 << 1), 0, 0, n0 | (n1 << 1), 2,
              src, lit, 0, 0, 0);
        e_nop4(o);
        if (!cmpx)
          mask_copy(o, c, sdst);
      }
      break;
    }
    u32 lit = x->len > 1 ? x->w[1] : 0;
    if (nc) {
      u32 src[2] = {src0, vsrc1}, l[2] = {lit, 0};
      valu3(o, c, op3, vdst, 0, 0, 0, 0, 2, src, l, 0, 1, c->ss);
      e_nop4(o);
      break;
    }
    if (cmpx) {
      u32 src[2] = {src0, vsrc1}, l[2] = {lit, 0};
      valu3(o, c, op3, c->ss, 0, 0, 0, 0, 2, src, l, 0, 0, 0);
      e_nop4(o);
      break;
    }
    if (fmt == VOP2 && (op == G10_VOP2_V_MADAK_F32 || op == G10_VOP2_V_MADMK_F32) &&
        (is_sgpr_like(src0) || src0 == LIT || src0 == 248)) {
      /* the K literal already is the one scalar read */
      e_vop1(o, G7_VOP1_V_MOV_B32, c->sv[0] - 256, src0 == 248 ? LIT : src0,
             src0 == 248 ? INV_2PI : 0);
      src0 = c->sv[0];
    }
    u32 head;
    if (fmt == VOP1)
      head = (0x3fu << 25) | (vdst << 17) | (op7 << 9);
    else if (fmt == VOP2)
      head = (op7 << 25) | (vdst << 17) | ((vsrc1 - 256) << 9);
    else
      head = (0x3eu << 25) | (op7 << 17) | ((vsrc1 - 256) << 9);
    if (src0 == 248) {
      emit(o, head | LIT);
      emit(o, INV_2PI);
    } else {
      emit(o, head | src0);
      if (x->len > 1)
        emit(o, lit);
    }
    if (fmt == VOPC || (m->flags & (F_CARRY | F_SDST)))
      e_nop4(o);
    break;
  }
  case VOP3: {
    u32 w1 = x->w[1];
    u32 op = (w >> 16) & 0x3ff, vdst = w & 0xff, abs = (w >> 8) & 7, clamp = (w >> 15) & 1;
    u32 opsel = (w >> 11) & 0xf, omod = (w1 >> 27) & 3, neg = (w1 >> 29) & 7;
    u32 src[3] = {w1 & 0x1ff, (w1 >> 9) & 0x1ff, (w1 >> 18) & 0x1ff};
    u32 lit[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++)
      if (src[i] == LIT)
        lit[i] = x->w[2];
    if (op == G10_VOP3_V_PERMLANEX16_B32) {
      /* op_sel:[0,1] = bound_ctrl, no fetch-inactive; selects 0x76543210/0xfedcba98: lane ^ 16 */
      u32 i = (u32)(x - ins);
      if (opsel != 2 || abs || clamp || omod || neg || src[0] < 256)
        return *why = "v_permlanex16 form other than bound_ctrl", -1;
      if (src[1] != LIT || x->w[2] != 0x76543210u || !is_sgpr_like(src[2]) ||
          !sgpr_holds(ins, i, c->tgt, src[2], 0xfedcba98u))
        return *why = "v_permlanex16 lane selects other than lane ^ 16", -1;
      swizzle(o, c, vdst, src[0], 0x1f | (16u << 10));
      break;
    }
    m = find(VOP3, op);
    if (m && (m->flags & F_CARRY)) {
      /* RDNA VOP3b: bits 8-14 are the carry-out SGPR pair */
      if (clamp || omod || neg)
        return *why = "VOP3b modifiers", -1;
      u32 sd = (w >> 8) & 0x7f;
      valu3(o, c, m->g7, vdst, 0, 0, 0, 0, m->nsrc, src, lit, (m->flags & F_COND) != 0, 1,
            mask_dst(c, sd));
      e_nop4(o);
      mask_copy(o, c, sd);
      break;
    }
    if (opsel)
      return *why = "VOP3 opsel", -1;
    if (op == G10_VOP3_V_LSHL_ADD_U32) {
      /* d = (a << s) + c -> t = a << s; d = t + c (carry to the scratch pair) */
      u32 t = c->sv[2];
      u32 a[2] = {src[1], src[0]}, la[2] = {lit[1], lit[0]};
      valu3(o, c, 0x100 + G7_VOP2_V_LSHLREV_B32, t - 256, 0, 0, 0, 0, 2, a, la, 0, 0, 0);
      u32 b[2] = {t, src[2]}, lb[2] = {0, lit[2]};
      valu3(o, c, 0x100 + G7_VOP2_V_ADD_NC_U32, vdst, 0, 0, 0, 0, 2, b, lb, 0, 1, c->ss);
      e_nop4(o);
      break;
    }
    if (op == G10_VOP3_V_ADD3_U32 || op == G10_VOP3_V_ADD_LSHL_U32) {
      /* t = a + b; d = t + c, or d = t << c */
      u32 t = c->sv[2];
      if (abs || clamp || omod || neg)
        return *why = "VOP3 modifiers on an integer op", -1;
      u32 a[2] = {src[0], src[1]}, la[2] = {lit[0], lit[1]};
      valu3(o, c, 0x100 + G7_VOP2_V_ADD_NC_U32, t - 256, 0, 0, 0, 0, 2, a, la, 0, 1, c->ss);
      if (op == G10_VOP3_V_ADD3_U32) {
        u32 b[2] = {t, src[2]}, lb[2] = {0, lit[2]};
        valu3(o, c, 0x100 + G7_VOP2_V_ADD_NC_U32, vdst, 0, 0, 0, 0, 2, b, lb, 0, 1, c->ss);
      } else {
        u32 b[2] = {src[2], t}, lb[2] = {lit[2], 0};
        valu3(o, c, 0x100 + G7_VOP2_V_LSHLREV_B32, vdst, 0, 0, 0, 0, 2, b, lb, 0, 0, 0);
      }
      e_nop4(o);
      break;
    }
    if (op == G10_VOP3_V_READLANE_B32) {
      /* gfx7 VOP2: the lane select (SGPR or inline constant) sits in the vsrc1 field */
      if (src[0] < 256 || src[1] >= 256 || src[1] == LIT || src[1] == 248 || abs || clamp ||
          omod || neg)
        return *why = "v_readlane form", -1;
      emit(o, (G7_VOP2_V_READLANE_B32 << 25) | ((vdst & 0x7f) << 17) | (src[1] << 9) | src[0]);
      e_nop4(o);
      break;
    }
    if (!m)
      return *why = "instruction not in the table", -1;
    op7 = m->g7;
    if (m->flags & F_NC) {
      if (abs || clamp || omod || neg)
        return *why = "VOP3 modifiers on an integer op", -1;
      valu3(o, c, op7, vdst, 0, 0, 0, 0, 2, src, lit, 0, 1, c->ss);
      e_nop4(o);
      break;
    }
    u32 mask = op7 < 0x100 && !(m->flags & F_CMPX); /* a compare writing an SGPR (pair) */
    valu3(o, c, op7, (m->flags & F_CMPX) ? c->ss : mask ? mask_dst(c, vdst) : vdst, abs, clamp, omod,
          neg, m->nsrc, src, lit, (m->flags & F_COND) != 0, 0, 0);
    if (op7 < 0x100)
      e_nop4(o);
    if (mask)
      mask_copy(o, c, vdst);
    break;
  }
  default:
    return *why = "unhandled format", -1;
  }
  return 0;
}

/* A wave32 shader: it drives EXEC_LO/VCC_LO as 32-bit masks (s_*_saveexec_b32, s_mov_b32 exec_lo,
 * ...) and uses VCC_HI as a free scalar. A wave64 shader never writes EXEC_LO/VCC_HI as a 32-bit
 * SGPR. Detecting it from the code (the header is not passed here) keeps one entry point. */
static int is_wave32(const struct insn *ins, u32 n) {
  for (u32 i = 0; i < n; i++) {
    const struct insn *x = &ins[i];
    u32 w = x->w[0];
    if (x->fmt == SOP1) {
      u32 op = (w >> 8) & 0xff, d = (w >> 16) & 0x7f;
      if (op == G10_SOP1_S_AND_SAVEEXEC_B32)
        return 1;
      /* a 32-bit SOP1 (s_mov_b32/s_not_b32) writing EXEC_LO; a wave64 b64 write has a b64 opcode */
      if (d == EXEC_LO && (op == G10_SOP1_S_MOV_B32 || op == G10_SOP1_S_NOT_B32))
        return 1;
    } else if (x->fmt == SOP2) {
      u32 op = (w >> 23) & 0x7f, d = (w >> 16) & 0x7f;
      if (d == EXEC_LO && (op == G10_SOP2_S_AND_B32 || op == G10_SOP2_S_ANDN2_B32 ||
                           op == G10_SOP2_S_OR_B32 || op == G10_SOP2_S_CSELECT_B32))
        return 1;
    }
  }
  return 0;
}

/* Replace VCC_HI by the reserved scalar in every operand field of one decoded instruction. Mask
 * ops (EXEC_LO/VCC_LO, saveexec) never name VCC_HI, so a blanket field replace is safe. */
static void remap_vcc_hi(struct insn *x, u32 vhi) {
  u32 *w = x->w;
  if (x->sdwa) {
    /* SDWA: an SGPR src0 (S0) or src1 (S1, in vsrc1) and a VOPC's SGPR destination (SD) */
    if (((w[1] >> 23) & 1) && (w[1] & 0xff) == VCC_HI) w[1] = (w[1] & ~0xffu) | vhi;
    if (x->fmt != VOP1 && ((w[1] >> 31) & 1) && ((w[0] >> 9) & 0xff) == VCC_HI)
      w[0] = (w[0] & ~(0xffu << 9)) | (vhi << 9);
    if (x->fmt == VOPC && ((w[1] >> 15) & 1) && ((w[1] >> 8) & 0x7f) == VCC_HI)
      w[1] = (w[1] & ~(0x7fu << 8)) | (vhi << 8);
    return;
  }
  switch (x->fmt) {
  case SOP1:
    if (((w[0] >> 16) & 0x7f) == VCC_HI) w[0] = (w[0] & ~(0x7fu << 16)) | (vhi << 16);
    if ((w[0] & 0xff) == VCC_HI) w[0] = (w[0] & ~0xffu) | vhi;
    break;
  case SOP2:
    if (((w[0] >> 16) & 0x7f) == VCC_HI) w[0] = (w[0] & ~(0x7fu << 16)) | (vhi << 16);
    if (((w[0] >> 8) & 0xff) == VCC_HI) w[0] = (w[0] & ~(0xffu << 8)) | (vhi << 8);
    if ((w[0] & 0xff) == VCC_HI) w[0] = (w[0] & ~0xffu) | vhi;
    break;
  case SOPC:
    if (((w[0] >> 8) & 0xff) == VCC_HI) w[0] = (w[0] & ~(0xffu << 8)) | (vhi << 8);
    if ((w[0] & 0xff) == VCC_HI) w[0] = (w[0] & ~0xffu) | vhi;
    break;
  case SOPK:
    if (((w[0] >> 16) & 0x7f) == VCC_HI) w[0] = (w[0] & ~(0x7fu << 16)) | (vhi << 16);
    break;
  case SMEM:
    if (((w[0] >> 6) & 0x7f) == VCC_HI) w[0] = (w[0] & ~(0x7fu << 6)) | (vhi << 6);
    if (((w[1] >> 25) & 0x7f) == VCC_HI) w[1] = (w[1] & ~(0x7fu << 25)) | (vhi << 25);
    break;
  case VOP1: {
    const struct opmap *m = find(VOP1, (w[0] >> 9) & 0xff);
    if ((w[0] & 0x1ff) == VCC_HI) w[0] = (w[0] & ~0x1ffu) | vhi; /* src0 */
    if (m && (m->flags & F_SDST) && ((w[0] >> 17) & 0xff) == VCC_HI) /* v_readfirstlane dst */
      w[0] = (w[0] & ~(0xffu << 17)) | (vhi << 17);
    break;
  }
  case VOP2:
  case VOPC:
    if ((w[0] & 0x1ff) == VCC_HI) w[0] = (w[0] & ~0x1ffu) | vhi; /* src0 */
    break;
  case VOP3: {
    for (int k = 0; k < 3; k++) {
      u32 sh = 9 * k;
      if (((w[1] >> sh) & 0x1ff) == VCC_HI) w[1] = (w[1] & ~(0x1ffu << sh)) | (vhi << sh);
    }
    u32 op = (w[0] >> 16) & 0x3ff;
    if (op < 0x100 || op == G10_VOP3_V_READLANE_B32) { /* dst is an SGPR */
      if ((w[0] & 0xff) == VCC_HI) w[0] = (w[0] & ~0xffu) | vhi;
    }
    break;
  }
  }
}

static u32 max_regs(const struct insn *ins, u32 n, u32 *max_s) {
  /* registers named by the code: VGPR fields and SGPR fields of every format used */
  u32 mv = 0, ms = 0;
#define V(r) do { if ((r) >= 256 && (r) - 256 > mv) { mv = (r) - 256; R2G_DEBUG_MAX(i, mv); } } while (0)
#define VR(r) do { if ((r) > mv) { mv = (r); R2G_DEBUG_MAX(i, mv); } } while (0)
#define S(r) do { if ((r) < 104 && (r) > ms) ms = (r); } while (0)
  for (u32 i = 0; i < n; i++) {
    const struct insn *x = &ins[i];
    const struct opmap *m;
    u32 w = x->w[0];
    switch (x->fmt) {
    case SOP1: S((w >> 16) & 0x7f); S(w & 0xff); break;
    case SOP2: S((w >> 16) & 0x7f); S(w & 0xff); S((w >> 8) & 0xff); break;
    case SOPC: S(w & 0xff); S((w >> 8) & 0xff); break;
    case SOPK: S((w >> 16) & 0x7f); break;
    case SMEM: {
      u32 op = (w >> 18) & 0xff;
      m = find(SMEM, op);
      S(((w >> 6) & 0x7f) + (m ? m->width : 1) - 1);
      S((w & 0x3f) * 2 + (op >= G10_SMEM_S_BUFFER_LOAD_DWORD ? 3 : 1)); /* buffer loads: a V# */
      S((x->w[1] >> 25) & 0x7f);
      break;
    }
    case MIMG: {
      u32 dm = (w >> 8) & 0xf, nd = 0;
      for (int b = 0; b < 4; b++)
        nd += (dm >> b) & 1;
      if (x->len == 3) /* NSA: each address VGPR on its own */
        for (u32 k = 0; k <= ((w >> 3) & 7); k++)
          VR(k ? (x->w[2] >> (8 * (k - 1))) & 0xff : x->w[1] & 0xff);
      else
        VR((x->w[1] & 0xff) + ((w >> 3) & 7)); /* 1D: one address VGPR, 2D: two */
      VR(((x->w[1] >> 8) & 0xff) + (nd ? nd : 1) - 1);
      S((((x->w[1] >> 16) & 0x1f) * 4) + 7);
      S((((x->w[1] >> 21) & 0x1f) * 4) + 3);
      break;
    }
    case MUBUF: {
      u32 w1 = x->w[1];
      m = find(MUBUF, (w >> 18) & 0xff);
      VR((w1 & 0xff) + (((w >> 12) & 3) == 3)); /* idxen + offen: two address VGPRs */
      VR(((w1 >> 8) & 0xff) + (m ? m->width : 1) - 1);
      S(((w1 >> 16) & 0x1f) * 4 + 3);
      S(w1 >> 24);
      break;
    }
    case DS: {
      u32 w1 = x->w[1];
      m = find(DS, (w >> 18) & 0xff);
      u32 wd = m ? m->width : 4;
      VR(w1 & 0xff);
      if (!m || (m->flags & F_DSREAD))
        VR((w1 >> 24) + wd - 1);
      if (!m || !(m->flags & F_DSREAD)) {
        VR(((w1 >> 8) & 0xff) + wd - 1);
        VR(((w1 >> 16) & 0xff) + wd - 1);
      }
      break;
    }
    case VOP1: case VOP2: case VOPC:
      m = x->fmt == VOP1 ? find(VOP1, (w >> 9) & 0xff) : 0;
      if (m && (m->flags & F_SDST))
        S((w >> 17) & 0xff);
      else if (x->fmt != VOPC)
        VR((w >> 17) & 0xff);
      if (x->fmt != VOP1 && !(x->sdwa && (x->w[1] >> 31)))
        VR((w >> 9) & 0xff);
      if (x->sdwa && x->fmt != VOP1 && (x->w[1] >> 31))
        S((w >> 9) & 0xff);
      if (x->sdwa) {
        u32 s = x->w[1];
        if ((s >> 23) & 1) S(s & 0xff); else VR(s & 0xff);
        if (x->fmt == VOPC && ((s >> 15) & 1)) S(((s >> 8) & 0x7f) + 1);
      } else if (x->dpp) {
        VR(x->w[1] & 0xff);
      } else {
        V(w & 0x1ff);
        S(w & 0x1ff);
      }
      break;
    case VOP3: {
      u32 w1 = x->w[1], op = (w >> 16) & 0x3ff;
      if (op < 0x100) { /* compare: an SGPR pair */
        S((w & 0xff) + 1);
      } else if (op == G10_VOP3_V_READLANE_B32) {
        S(w & 0xff);
      } else {
        VR(w & 0xff);
        m = find(VOP3, op);
        if (m && (m->flags & F_CARRY))
          S(((w >> 8) & 0x7f) + 1);
      }
      for (int k = 0; k < 3; k++) {
        u32 r = (w1 >> (9 * k)) & 0x1ff;
        V(r);
        S(r);
      }
      break;
    }
    }
  }
#undef V
#undef VR
#undef S
  *max_s = ms;
  return mv;
}

static u32 log2_of(u32 v) {
  u32 k = 0;
  while ((1u << k) < v)
    k++;
  return k;
}

int rdna2gcn_compute(const u32 *in, u32 in_dwords, u32 *out, u32 out_cap,
                     const struct rdna2gcn_cs *cs, struct rdna2gcn_info *info) {
  static struct insn ins[MAX_INSNS];
  static u32 map[MAX_INSNS];
  static unsigned char tgt[MAX_INSNS + 1];
  u32 n = 0;
  const char *why = 0;
  memset(info, 0, sizeof *info);
  if (decode(in, in_dwords, ins, &n, &why)) {
    info->error = why;
    return -1;
  }
  if (!n) {
    info->error = "no instructions";
    return -1;
  }
  struct ctx c;
  memset(&c, 0, sizeof c);
  c.wave32 = cs ? cs->wave32 != 0 : is_wave32(ins, n);
  c.fix_vsharp = cs != 0;
  if (cs && c.wave32) {
    u32 x = cs->threads[0] & 0xffff, y = cs->threads[1] & 0xffff, z = cs->threads[2] & 0xffff;
    x = x ? x : 1, y = y ? y : 1, z = z ? z : 1;
    u32 all = x * y * z;
    if (all > 32) {
      if (all % 32 || all > 512 || (x & (x - 1)) || (y & (y - 1)) || cs->tidig > 2) {
        info->error = "wave32 group of this shape";
        return -1;
      }
      c.split = 1;
      c.tidig = cs->tidig;
      c.log2x = log2_of(x), c.log2xy = log2_of(x * y);
      c.maskx = x - 1, c.masky = y - 1;
      info->hw_threads = 2 * all;
    }
  }
  info->wave32 = (u32)c.wave32;
  u32 ms = 0, mv = max_regs(ins, n, &ms);
  for (int i = 0; i < 3; i++)
    c.sv[i] = 256 + mv + 1 + i;
  u32 sp = ms + 1 + ((ms + 1) & 1);
  c.ss = sp;
  c.ss2 = sp + 2;
  /* one more scalar for the relocated VCC_HI (kept even, after the two scratch pairs) */
  c.vhi = c.wave32 ? c.ss2 + 2 : 0;
  if (c.wave32)
    for (u32 i = 0; i < n; i++)
      remap_vcc_hi(&ins[i], c.vhi);
  c.tgt = tgt;
  c.m0 = 0;
  memset(tgt, 0, sizeof tgt);
  for (u32 i = 0; i < n; i++) {
    const struct insn *x = &ins[i];
    u32 w = x->w[0];
    if (x->fmt == DS || x->dpp ||
        (x->fmt == VOP3 && ((w >> 16) & 0x3ff) == G10_VOP3_V_PERMLANEX16_B32))
      c.m0 = 1;
    if (x->fmt == SOPP && is_branch((w >> 16) & 0x7f)) {
      u32 t = x->off + 1 + (u32)(int)(short)(w & 0xffff);
      for (u32 j = 0; j < n; j++)
        if (ins[j].off == t)
          tgt[j] = 1;
    }
  }
  /* LDS/swizzle ops need M0 = -1. The shader may still set M0 itself (e.g. an indexed
   * v_movrels waterfall), as long as no LDS/swizzle op follows that write with M0 != -1. Walk
   * forwards: an M0 write to a non -1 value is a hazard only if a later DS/swizzle op reads it. */
  if (c.m0) {
    int m0_dirty = 0;
    u32 bad = 0;
    for (u32 i = 0; i < n; i++) {
      const struct insn *x = &ins[i];
      u32 w = x->w[0];
      int dsx = x->fmt == DS || x->dpp ||
                (x->fmt == VOP3 && ((w >> 16) & 0x3ff) == G10_VOP3_V_PERMLANEX16_B32);
      if (dsx && m0_dirty) {
        bad = x->off;
        break;
      }
      if (writes_sgpr(x, M0)) {
        /* an explicit "s_mov_b32 m0, -1" clears the hazard; anything else leaves M0 unknown */
        int to_minus1 = x->fmt == SOP1 && ((w >> 8) & 0xff) == G10_SOP1_S_MOV_B32 &&
                        ((w >> 16) & 0x7f) == M0 && (w & 0xff) == ALL_ONES;
        m0_dirty = !to_minus1;
      }
    }
    if (bad) {
      info->error = "M0 set to a non -1 value before an LDS op";
      info->error_at = bad;
      return -1;
    }
  }
  info->src_dwords = ins[n - 1].off + ins[n - 1].len;
  info->vgprs = mv + 1 + 3;
  info->sgprs = sp + 4 + 2 + (c.wave32 ? 1 : 0); /* + VCC (+ relocated VCC_HI for wave32) */
  if (info->vgprs > 256 || info->sgprs > 104) {
    info->error = "too many registers for gfx7";
    return -1;
  }
  /* pass 1: sizes and offsets; pass 2: final, with branch targets */
  for (int pass = 0; pass < 2; pass++) {
    struct out o = {pass ? out : 0, 0, out_cap, 0};
    for (u32 i = 0; i < n; i++) {
      if (!pass)
        map[i] = o.n;
      if (xlate(&o, &c, &ins[i], pass ? map : 0, ins, n, &why)) {
        info->error = why;
        info->error_at = ins[i].off;
        return -1;
      }
    }
    if (o.err) {
      info->error = "output buffer too small";
      return -1;
    }
    info->out_dwords = o.n;
  }
  info->rsrc1_vgprs = (info->vgprs + 3) / 4 - 1;
  info->rsrc1_sgprs = (info->sgprs + 7) / 8 - 1;
  return 0;
}

int rdna2gcn(const u32 *in, u32 in_dwords, u32 *out, u32 out_cap, struct rdna2gcn_info *info) {
  return rdna2gcn_compute(in, in_dwords, out, out_cap, 0, info);
}
