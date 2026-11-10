#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The patches for the engines' main TAA pixel shaders (agent/insetpp.c, agent.taa): agent/taa_patch.h.

    gen_taa_patch.py <Hikari's PS10 block.bin> <Chisato's PS40 block.bin> > agent/taa_patch.h

Each block is an RHI pixel shader's CPU copy (agent.taadump; tools/fov/taadump.py), its code found
by its `s_mov_b32 s107, <imm>` start, its Gnm::PsShader at +0x34 (the stage registers at +0x3c).
Read with tools/gcn7dis.py (docs/MULTIVIEW.md 30). Two bases:

  0  Hikari 1.00's FPostProcessTemporalAAPS<1,0> (logs/aa4-block, logs/taa/aa4-0.bin): the code at
     +0xa4, 2380 bytes, 64 VGPRs, 80 SGPRs.
       0x3e4 (block)  v_max_f32_e64 v52, |v0|, |v1|  the history's place hist = cur - BackN is known
                      here (v24, v23 the pixel's own place on the screen; v0, v1 the history's, before
                      the clamp; s40, s41 the screen-to-pixel scale, s42, s43 the viewport's centre);
       0x8d4 0x90c 0x934 0x954  v_madmk_f32 vD, vA, vB, 0.04  the blend: hist + 0.04 (cur - hist), in
                      YCoCg and alpha, a fixed 4% of the new frame whatever the motion.
     The added code uses v64 on and s76 on (the shader then 68-76 VGPRs, 96 SGPRs).
  1  Chisato's and Allison's FPostProcessTemporalAAPS<4,0> (logs/pop10, "taadump 4"; the same code in
     both games), the "faster" TAA their newer engine takes at r.PostProcessAAQuality 3, which the
     agent caps them at (the games' own 10 shimmers at 4+): the code at +0xa0, 2048 bytes, 32 VGPRs,
     64 SGPRs. The same shape with other registers: the view buffer's V# in s[0:3] (0x1c0 its
     ClipToPrevClip load), ScreenPosToPixel in s48-s51, the pixel's screen place v22, v10, the
     velocity test 0x2f0, the history's place 0x318 (v_max_f32_e64 v22, |v10|, |v0|), the history
     off the view in s[0:1] (0x468), the history's 5 Catmull-Rom taps RGB only (dmask 7: the alpha
     neither read nor kept, the output's is 0 at 0x888), the three blends in YCoCg 0x760, 0x7cc,
     0x838: v_mac_f32 vD, s7, vX, hist + s7 (filtered - hist) with s7 r.TemporalAACurrentFrameWeight
     (the games set 0.15), no dither. The added code uses v32 on and s76 on (37-41 VGPRs, 96 SGPRs).

Variants (agent.taa; base 1 builds those marked +, the others as the variant named in ALIAS40):
  1  the copy as is (+)
  2  weight = 0.04 + min(0.004 x the pixel's motion in pixels, 0.16): the engine's own 4% held,
     up to 20% at 40 pixels a frame (UE 4.15's later curve)
  3  no history at all (+: a test that the patch is in; base 1 takes every history as off the view,
     the plus-filtered frame alone)
  4  the velocity buffer left out (+)
  5-9  see hikari_variants; 10 a probe (+: base 0 one pixel's numbers, base 1 the view buffer's
     first 960 dwords each frame, for its layout)
  11 the velocity fixed, in the insets only: the inset's window move added back (+: base 0 from
     each view's last projection kept in the agent's memory; base 1 from the view buffer's own
     previous projection, PrevViewToClip; base 1's 7 is the agent's-memory form, for comparison)
  12 2 and 11
  13 11 with newly uncovered pixels converged as a running average, inset mode's default on
     Hikari 1.00 (+: base 0 keeps the weight in the history's alpha; base 1, whose history has no
     alpha, in the agent's memory: a byte per 2x2 block of an inset, two planes in turn)
  14, 15  13 with those pixels started from a soft picture (base 0 only)
Everything is assembled by llvm-mc (gfx701) here; nothing is typed as numbers. Base 0's bytes are
the same as before the second base was added (checked against the committed header when it was)."""
import re, struct, subprocess, sys, os

MC = os.path.join(os.environ.get("LLVM") or "/opt/homebrew/opt/llvm/bin", "llvm-mc")
NVAR = 15


def f32bits(v):
    return struct.unpack("<I", struct.pack("<f", float(v)))[0]


def asm(lines):
    r = subprocess.run([MC, "-triple=amdgcn", "-mcpu=gfx701", "-show-encoding"], input="\n".join(lines) + "\n",
                       capture_output=True, text=True)
    out = []
    for m in re.finditer(r"encoding: \[(.*?)\]", r.stdout):
        out.append(bytes(int(x, 16) for x in m.group(1).split(",")))
    if len(out) != len(lines) or r.stderr.strip():
        sys.exit("llvm-mc: %s" % r.stderr)
    return out


def fnv(b):
    h = 0x811c9dc5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xffffffff
    return h


def branch(frm, to):  # s_branch at frm to to (its offset from the next instruction, in dwords)
    return struct.pack("<I", 0xbf820000 | (((to - (frm + 4)) // 4) & 0xffff))


class Base:
    """an engine's TAA pixel shader as dumped: its block, the code in it, its stage registers"""

    def __init__(self, name, path, code_off, code_len, rsrc1, inputs):
        blk = open(path, "rb").read()
        self.name, self.off, self.len = name, code_off, code_len
        self.code = blk[code_off:code_off + code_len]
        self.regs = struct.unpack_from("<12I", blk, 0x3c)
        r = self.regs
        assert r[2] == rsrc1 and r[3] == 0x1c and r[5] == 4 and r[6] == inputs, "%s: not the TAA shader (RSRC1 0x%x inputs 0x%x)" % (path, r[2], r[6])
        assert self.code[:4] == b"\xff\x03\xeb\xbe" and self.code[-4:] == asm(["s_endpgm"])[0], "%s: the code is not at +0x%x, %d bytes" % (path, code_off, code_len)
        assert len(self.code) == code_len

    def rsrc(self, vgprs, sgprs=None):
        r = (self.regs[2] & ~0x3f) | ((vgprs + 3) // 4 - 1)
        if sgprs is not None:
            r = r & ~(0xf << 6) | (((sgprs + 7) // 8 - 1) << 6)
        return r


# ---- base 0: Hikari's FPostProcessTemporalAAPS<1,0> (as it was written, byte for byte) ----
def hikari_variants(B):
    code, regs, CODE_OFF, CODE_LEN = B.code, B.regs, B.off, B.len
    T1_SITE = 0x3e4 - CODE_OFF
    BLENDS = [0x8d4 - CODE_OFF, 0x90c - CODE_OFF, 0x934 - CODE_OFF, 0x954 - CODE_OFF]
    want = asm(["v_max_f32_e64 v52, |v0|, |v1|"])[0]
    assert code[T1_SITE:T1_SITE + 8] == want, "0x3e4 is not v_max_f32_e64 v52, |v0|, |v1|"
    blends = []
    for b in BLENDS:
        w0, k = struct.unpack_from("<II", code, b)
        assert w0 >> 25 == 32 and k == 0x3d23d70a, "0x%x is not v_madmk_f32 ..., 0.04" % b
        d, vb, a = (w0 >> 17) & 0xff, (w0 >> 9) & 0xff, w0 & 0x1ff
        sa = "v%d" % (a - 256) if a >= 256 else "s%d" % a
        blends.append((b, asm(["v_mad_f32 v%d, %s, v64, v%d" % (d, sa, vb)])[0]))
    tramp_at = CODE_LEN  # (after s_endpgm and the code's end; 4-aligned)

    chk = asm(["s_branch 5"])[0]
    assert chk == struct.pack("<I", 0xbf820005), chk.hex()

    def detour(site, site_len, original, body):
        """the site's instruction(s) replaced by a branch to code after the shader's end that does
        what they did, then body, then branches back"""
        pre = asm([original] + body)
        n = sum(len(x) for x in pre)
        tramp = b"".join(pre) + branch(tramp_at + n, site + site_len)
        assert code[site:site + len(pre[0])] == pre[0], "0x%x is not %s" % (site + CODE_OFF, original)
        sitebytes = branch(site, tramp_at) + b"".join(asm(["s_nop 0"]) * ((site_len - 4) // 4))
        return [(site, sitebytes), (tramp_at, tramp)]

    WEIGHT = ["v_sub_f32_e32 v64, v24, v0", "v_sub_f32_e32 v65, v23, v1",
              "v_mul_f32_e32 v64, s40, v64", "v_mul_f32_e32 v65, s41, v65",
              "v_max_f32_e64 v64, |v64|, |v65|", "v_mul_f32_e32 v64, 0x3b83126f, v64",
              "v_min_f32_e32 v64, 0x3e23d70a, v64", "v_add_f32_e32 v64, 0x3d23d70a, v64"]
    NOVEL_AT = 0x3c4 - CODE_OFF  # v_cmp_lt_f32_e32 vcc, 0, v0: the pixel's velocity used (x > 0)
    assert code[NOVEL_AT:NOVEL_AT + 4] == asm(["v_cmp_lt_f32_e32 vcc, 0, v0"])[0]
    novel = [(NOVEL_AT, asm(["v_cmp_f_f32_e32 vcc, 0, v0"])[0])]  # (never: the camera's reprojection everywhere)
    OUT_AT = 0x9d8 - CODE_OFF
    assert code[OUT_AT:OUT_AT + 8] == b"".join(asm(["v_cvt_pkrtz_f16_f32_e32 v1, v1, v2", "v_cvt_pkrtz_f16_f32_e32 v0, v4, v3"]))
    rsrc = lambda vgprs: (regs[2] & ~0x3f) | ((vgprs + 3) // 4 - 1)
    # The velocity's fix (docs/MULTIVIEW.md 30): the engine writes an object's velocity with this
    # frame's projection for its last place too, so an inset's velocity misses its window's move
    # (the crop's M[2][0], M[2][1]), and every object that writes velocity ghosts as the window
    # follows the gaze (aa9: with the velocity left out the date cards match a still window). The
    # move: this frame's projection's M[2][0], M[2][1] (the view uniform buffer's ViewToClip, at
    # byte 384, dword 104) less last frame's (PrevViewToClip, 1104, dword 284), both with their
    # frame's TAA jitter (under half a pixel each): with the window moved by whole 2-pixel steps
    # (agent.insetsnap=2) the move is a multiple of 4 / the view's width, and rounding to it takes
    # the jitters out; for an eye (a fixed projection) it is 0. Loaded at the view buffer's load
    # (0x1d0: s_buffer_load_dwordx16 s16, s[16:19], ClipToPrevClip) into s76-s79 (free: the shader
    # uses up to s75 of its 80), added at the velocity's test (0x3c4) to the decoded y (v1) and the
    # raw x (v0, before its decode x 4.008).
    LOAD_AT = 0x1d0 - CODE_OFF
    def velfix(sign):
        return (None, ["v_cmp_lt_f32_e32 vcc, 0, v0"] + velfix_body(sign, IDENT))

    def two_detours(first, second_site, second_len, second_orig, second_body):
        # (two pieces of added code one after the other after the shader's end)
        nonlocal_at = [tramp_at]
        out = []
        for k, (site, slen, orig, body) in enumerate(((first[0], first[1], first[2], first[3]),
                                                      (second_site, second_len, second_orig, second_body))):
            # (the first: the added loads BEFORE the original, which overwrites s16-s31, their
            # buffer's descriptor s[16:19] among them: a scalar load doesn't wait for the
            # registers another still writes; the second: the original first, its vcc used)
            pre = asm(body + [orig]) if k == 0 else asm([orig] + body)
            o = asm([orig])[0]
            n = sum(len(x) for x in pre)
            at = nonlocal_at[0]
            tramp = b"".join(pre) + branch(at + n, site + slen)
            assert code[site:site + len(o)] == o, "0x%x is not %s" % (site + CODE_OFF, orig)
            out += [(site, branch(site, at) + b"".join(asm(["s_nop 0"]) * ((slen - 4) // 4))), (at, tramp)]
            nonlocal_at[0] = at + len(tramp)
        return out, nonlocal_at[0]

    def velfix_patches(sign, weight):
        d1, body = velfix(sign)
        first = (LOAD_AT, 8, "s_buffer_load_dwordx16 s[16:31], s[16:19], 0x1b0",
                 ["s_buffer_load_dwordx2 s[76:77], s[16:19], 0x68", "s_buffer_load_dwordx2 s[78:79], s[16:19], 0x11c"])
        pl, end = two_detours(first, NOVEL_AT, 4, body[0], body[1:])
        if weight:  # (the weight's detour after both)
            pre = asm(["v_max_f32_e64 v52, |v0|, |v1|"] + WEIGHT)
            n = sum(len(x) for x in pre)
            tramp = b"".join(pre) + branch(end + n, T1_SITE + 8)
            pl += [(T1_SITE, branch(T1_SITE, end) + asm(["s_nop 0"])[0]), (end, tramp)] + [(b, e) for b, e in blends]
        return pl

    def probe_patches():
        first = (LOAD_AT, 8, "s_buffer_load_dwordx16 s[16:31], s[16:19], 0x1b0",
                 ["s_buffer_load_dwordx2 s[84:85], s[16:19], 0x68", "s_buffer_load_dwordx2 s[86:87], s[16:19], 0x11c"])
        body = ["v_mov_b32_e32 v68, 1.0", "v_cndmask_b32_e32 v68, 0, v68", "s_mov_b64 s[80:81], vcc",
                "s_mov_b64 s[82:83], exec",
                # (the pixel by its screen place, v24 and v23, still whole here: v2 and v3, its
                # position, are a dither's hash by now; within 0xd1a90005-06 of 0xd1a90003-04)
                "v_mov_b32_e32 v69, 0xd1a90003", "v_sub_f32_e32 v69, v24, v69", "v_mov_b32_e32 v70, 0xd1a90005",
                "v_cmp_gt_f32_e64 vcc, v70, |v69|", "s_and_b64 exec, exec, vcc",
                "v_mov_b32_e32 v69, 0xd1a90004", "v_sub_f32_e32 v69, v23, v69", "v_mov_b32_e32 v70, 0xd1a90006",
                "v_cmp_gt_f32_e64 vcc, v70, |v69|", "s_and_b64 exec, exec, vcc",
                # (the left inset only: its viewport's centre s42, s43 in the buffer's pixels, x left
                # of the eyes' middle, y below the eyes: the same screen place is in every view; the
                # eyes' height 0xd1a90007 and the left eye's right edge 0xd1a90008 filled in by the
                # agent, right at every frame rate's layout)
                "v_mov_b32_e32 v69, s43", "v_cmp_lt_f32_e32 vcc, 0xd1a90007, v69", "s_and_b64 exec, exec, vcc",
                "v_mov_b32_e32 v69, s42", "v_cmp_gt_f32_e32 vcc, 0xd1a90008, v69", "s_and_b64 exec, exec, vcc",
                "s_cbranch_execz 50",
                "s_mov_b32 s76, 0xd1a90001", "s_mov_b32 s77, 0xd1a90002", "s_mov_b32 s78, 128", "s_mov_b32 s79, 0x24fac",
                "s_waitcnt lgkmcnt(0)",
                "buffer_store_dword v0, off, s[76:79], 0", "buffer_store_dword v1, off, s[76:79], 0 offset:4",
                "buffer_store_dword v35, off, s[76:79], 0 offset:8", "buffer_store_dword v31, off, s[76:79], 0 offset:12",
                "buffer_store_dword v68, off, s[76:79], 0 offset:16", "buffer_store_dword v24, off, s[76:79], 0 offset:20",
                "buffer_store_dword v23, off, s[76:79], 0 offset:24",
                "v_mov_b32_e32 v66, s84", "buffer_store_dword v66, off, s[76:79], 0 offset:28",
                "v_mov_b32_e32 v66, s85", "buffer_store_dword v66, off, s[76:79], 0 offset:32",
                "v_mov_b32_e32 v66, s86", "buffer_store_dword v66, off, s[76:79], 0 offset:36",
                "v_mov_b32_e32 v66, s87", "buffer_store_dword v66, off, s[76:79], 0 offset:40",
                "v_mov_b32_e32 v66, s40", "buffer_store_dword v66, off, s[76:79], 0 offset:44",
                "v_mov_b32_e32 v66, s41", "buffer_store_dword v66, off, s[76:79], 0 offset:48",
                "v_mov_b32_e32 v66, s42", "buffer_store_dword v66, off, s[76:79], 0 offset:52",
                "v_mov_b32_e32 v66, s43", "buffer_store_dword v66, off, s[76:79], 0 offset:56",
                "s_mov_b64 exec, s[82:83]", "s_mov_b64 vcc, s[80:81]"]
        # (the skip's distance from what it skips, assembled: to the exec's restore)
        i = body.index("s_cbranch_execz 50")
        j = body.index("s_mov_b64 exec, s[82:83]")
        skip = sum(len(x) for x in asm(body[i + 1:j])) // 4
        body[i] = "s_cbranch_execz %d" % skip
        pl, end = two_detours(first, NOVEL_AT, 4, "v_cmp_lt_f32_e32 vcc, 0, v0", body)
        tramp = [b for a, b in pl if a != NOVEL_AT and a != LOAD_AT][-1]
        k = sum(len(x) for x in asm(["v_cmp_lt_f32_e32 vcc, 0, v0"] + body[:i]))
        target = k + 4 + skip * 4
        assert tramp[target:target + 4] == asm(["s_mov_b64 exec, s[82:83]"])[0], "the probe's skip misses the restore"
        return pl

    # The velocity's fix, second form (docs/MULTIVIEW.md 30; aa13's probe: in frames where only the
    # window moved, the velocity read is 0 for a still object, its error the whole move): each view's
    # own last projection kept on the GPU. At the view buffer's load, this frame's ViewToClip
    # M[2][0..1] (dword 104: the probe read it right) into s76-s77 and the frame counter the dither
    # uses (dword 49 of the buffer at table+0x2c) into s78. At the velocity's test: the view (0-3 from
    # its viewport's centre s42, s43), the frame's parity (s78 & 1); the pixels within a pixel of the
    # view's middle store this frame's M[2][0..1], the frame counter and the view's key (its centre,
    # x + 4096 y) into slot (view, parity) of the agent's memory (0xd1a90001-02, 16 bytes a slot);
    # every pixel loads slot (view, other parity), last frame's, past the caches (glc), and takes the
    # move only if that slot is this view's (the key) from the frame before (the counter one less,
    # mod 8) and the view is an inset (an eye's projection never moves: its correction would only
    # be what another draw left in its slot); the move rounded to 2 pixels (the window's steps),
    # whatever its size (it was 0 past 75 steps, 150 pixels: every larger jump of the window lost,
    # the pop-in), added to the velocity. (The code review: before, the
    # slot was taken whatever wrote it, and the storing pixel picked in screen units, none in a view
    # under 300 rows.)
    def velfix2_patches(weight):
        first = (LOAD_AT, 8, "s_buffer_load_dwordx16 s[16:31], s[16:19], 0x1b0",
                 ["s_buffer_load_dwordx2 s[76:77], s[16:19], 0x68", "s_buffer_load_dword s78, s[48:51], 0x31"])
        body = velfix2_body(IDENT)
        pl, end = two_detours(first, NOVEL_AT, 4, "v_cmp_lt_f32_e32 vcc, 0, v0", body)
        if weight is None:  # (the bare fix and where its added code ends: for 13)
            return pl, end
        if weight:
            pre = asm(["v_max_f32_e64 v52, |v0|, |v1|"] + WEIGHT)
            n = sum(len(x) for x in pre)
            tramp = b"".join(pre) + branch(end + n, T1_SITE + 8)
            pl += [(T1_SITE, branch(T1_SITE, end) + asm(["s_nop 0"])[0]), (end, tramp)] + [(b, e) for b, e in blends]
        return pl

    # Newly uncovered pixels converged as a running average (docs/MULTIVIEW.md 30, "the pop-in"):
    # where the history is off the view (the window jumped back over what it had left, a saccade),
    # the engine takes the current frame alone (vcc at 0x94c: the history replaced by it) and from
    # then on blends a fixed 4%, so that first frame's aliasing (one jitter's sample) fades by 0.96 a
    # frame: 70% of it left after 8 frames, 36% after 25 (Hikari's hair "popping in" as the gaze
    # came back to her, the user, worn). Each pixel's weight kept in the history's alpha, which the
    # engine blends as a fourth channel but nothing after the TAA reads (pop3: still pictures the
    # same): the weight kept negative, so what any other shader left there (a scene's alpha, 0 or
    # more, the moment 13 takes over) reads as the engine's 0.04 and resets nothing: the weight used
    # w = minus the history's alpha (its Catmull-Rom value, v28, before the clamp) within 0.04-1;
    # the alpha written -w / (1 + w), -0.5 where the history was off the view (w 1 then): 1, 1/2,
    # 1/3 ... down to the engine's 0.04, the average of every frame since the pixel came into view,
    # an 8-sample average after 8 frames. The three colour blends take w (v64), the alpha's blend
    # and its dither are replaced.
    def fresh_patches(pl, end, soft=None):
        at = end
        def place(site, slen, lines):
            nonlocal at
            pre = asm(lines)
            n = sum(len(x) for x in pre)
            tramp = b"".join(pre) + branch(at + n, site + slen)
            out = [(site, branch(site, at) + b"".join(asm(["s_nop 0"]) * ((slen - 4) // 4))), (at, tramp)]
            at += len(tramp)
            return out
        W_AT, A_AT, D_AT = 0x8c8 - CODE_OFF, 0x94c - CODE_OFF, 0x9d4 - CODE_OFF
        assert code[W_AT:W_AT + 8] == asm(["v_min3_f32 v3, v7, v10, v34"])[0]
        assert code[A_AT:A_AT + 16] == b"".join(asm(["v_cndmask_b32_e32 v3, v3, v8", "v_sub_f32_e32 v4, v8, v3",
                                                      "v_madmk_f32 v3, v4, 0x3d23d70a, v3"]))
        assert code[D_AT:D_AT + 4] == asm(["v_mac_f32_e32 v3, s36, v46"])[0]
        if soft is None:
            out = place(W_AT, 8, ["v_min3_f32 v3, v7, v10, v34", "v_sub_f32_e32 v64, 0, v28", "v_max_f32_e32 v64, 0x3d23d70a, v64",
                                  "v_min_f32_e32 v64, 1.0, v64"])
            out += place(A_AT, 16, ["v_add_f32_e32 v65, 1.0, v64", "v_rcp_f32_e32 v65, v65", "v_mul_f32_e32 v65, v64, v65",
                                    "v_sub_f32_e32 v65, 0, v65", "v_mov_b32_e32 v3, -0.5", "v_cndmask_b32_e32 v3, v65, v3"])
        else:
            # 14: where the history is off the view, the history taken from a soft picture of the
            # current frame instead of the frame itself, and its weight started at `soft` (1/4,
            # 1/8) instead of 1 (the soft picture counted as 3 or 7 frames): no first frame of one
            # jitter's aliasing, and what comes back sharpens over several frames from something
            # like the periphery's softer picture (the eye ~0.6 of the inset's density) instead of
            # jumping to the inset's detail at once (the user, worn with 13: still a pop, the
            # calendar's date too). The soft picture: at the view's position test
            # (T1_SITE, the history's place v52 known, v24/v23 still the pixel's screen place),
            # for the waves with a pixel off the view only, four bilinear taps of the shader's own
            # current-frame texture (s[68:75]) with the history's bilinear sampler (s[52:55]) at
            # +-1/2 pixel (a 3x3 tent), each in the shader's space (Y' = R + 2G + B, Co' = 2R - 2B,
            # Cg' = 2G - R - B, times 1 / (1 + v45 Y'), as its own taps 0x554-0x5f4), averaged into
            # v72-v74; the three replacements of the history by the current frame (0x8ac, 0x904,
            # 0x92c) take them instead. The texture and sampler the engine's own for this draw: no
            # other view's or pooled target read, every tap clamped by the texture unit.
            C_AT = [(0x8ac - CODE_OFF, "v_cndmask_b32_e32 v15, v15, v2", "v_cndmask_b32_e32 v15, v15, v72"),
                    (0x904 - CODE_OFF, "v_cndmask_b32_e32 v1, v1, v15", "v_cndmask_b32_e32 v1, v1, v73"),
                    (0x92c - CODE_OFF, "v_cndmask_b32_e32 v0, v0, v9", "v_cndmask_b32_e32 v0, v0, v74")]
            for a, was, now in C_AT:
                assert code[a:a + 4] == asm([was])[0], "0x%x is not %s" % (a + CODE_OFF, was)
            taps = []
            for k, (sx, sy) in enumerate(((-0.5, -0.5), (0.5, -0.5), (-0.5, 0.5), (0.5, 0.5))):
                op = "v_mul_f32_e32" if k == 0 else "v_mac_f32_e32"
                taps += ["v_add_f32_e32 v68, %s, v70" % sx, "v_mul_f32_e32 v68, s34, v68",
                         "v_add_f32_e32 v69, %s, v71" % sy, "v_mul_f32_e32 v69, s35, v69",
                         "image_sample_lz v[64:67], v[68:69], s[68:75], s[52:55] dmask:0xf", "s_waitcnt vmcnt(0)",
                         "v_add_f32_e32 v68, v64, v66", "v_mac_f32_e32 v68, 2.0, v65",
                         "v_mad_f32 v69, v45, v68, 1.0", "v_rcp_f32_e32 v69, v69",
                         "v_sub_f32_e32 v67, v64, v66", "v_add_f32_e32 v67, v67, v67",
                         "v_mad_f32 v65, 2.0, v65, -v64", "v_sub_f32_e32 v65, v65, v66",
                         "%s v72, v69, v68" % op, "%s v73, v69, v67" % op, "%s v74, v69, v65" % op]
            soft_body = (["v_max_f32_e64 v52, |v0|, |v1|", "s_mov_b64 s[82:83], vcc", "v_cmp_le_f32_e32 vcc, 1.0, v52",
                          "s_and_saveexec_b64 s[80:81], vcc", "s_cbranch_execz 99", "s_waitcnt lgkmcnt(0)",
                          # the pixel's place in the buffer's pixels, its centre (as the history's, 0x3f8-0x40c)
                          "v_mov_b32_e32 v70, s42", "v_mac_f32_e32 v70, s40, v24", "v_add_f32_e32 v70, 0.5, v70",
                          "v_mov_b32_e32 v71, s43", "v_mac_f32_e32 v71, s41, v23", "v_add_f32_e32 v71, 0.5, v71"]
                         + taps + ["v_mul_f32_e32 v72, 0x3e800000, v72", "v_mul_f32_e32 v73, 0x3e800000, v73",
                                   "v_mul_f32_e32 v74, 0x3e800000, v74",
                                   "s_mov_b64 exec, s[80:81]", "s_mov_b64 vcc, s[82:83]"])
            i = soft_body.index("s_cbranch_execz 99")
            j = soft_body.index("s_mov_b64 exec, s[80:81]")
            skip = sum(len(x) for x in asm(soft_body[i + 1:j])) // 4
            soft_body[i] = "s_cbranch_execz %d" % skip
            assert code[T1_SITE:T1_SITE + 8] == asm(["v_max_f32_e64 v52, |v0|, |v1|"])[0]
            t1_at = at
            out = place(T1_SITE, 8, soft_body)
            tr = out[1][1]
            k = sum(len(x) for x in asm(soft_body[:i]))
            assert tr[k + 4 + skip * 4:k + 8 + skip * 4] == asm(["s_mov_b64 exec, s[80:81]"])[0], "the skip misses the restore"
            # the weight: `soft` where the history is off the view (vcc, 0x54c's), else from the alpha
            out += place(W_AT, 8, ["v_min3_f32 v3, v7, v10, v34", "v_sub_f32_e32 v64, 0, v28", "v_max_f32_e32 v64, 0x3d23d70a, v64",
                                   "v_min_f32_e32 v64, 1.0, v64", "v_mov_b32_e32 v65, 0x%08x" % struct.unpack("<I", struct.pack("<f", soft))[0],
                                   "v_cndmask_b32_e32 v64, v64, v65"])
            out += place(A_AT, 16, ["v_add_f32_e32 v65, 1.0, v64", "v_rcp_f32_e32 v65, v65", "v_mul_f32_e32 v65, v64, v65",
                                    "v_sub_f32_e32 v3, 0, v65"])
            out += [(a, asm([now])[0]) for a, was, now in C_AT]
        out += [(D_AT, asm(["s_nop 0"])[0])] + [(b, e) for b, e in blends if b != BLENDS[3]]
        assert at <= 0x1000 - 0x10, "the added code past the copy's page"
        return pl + out

    variants = {
        1: ([], regs[2]),
        2: (detour(T1_SITE, 8, "v_max_f32_e64 v52, |v0|, |v1|", WEIGHT) + [(b, e) for b, e in blends], rsrc(68)),
        3: (detour(T1_SITE, 8, "v_max_f32_e64 v52, |v0|, |v1|", ["v_mov_b32_e32 v64, 1.0"]) + [(b, e) for b, e in blends], rsrc(68)),
        4: (novel, regs[2]),
        5: (detour(T1_SITE, 8, "v_max_f32_e64 v52, |v0|, |v1|", WEIGHT) + [(b, e) for b, e in blends] + novel, rsrc(68)),
        # 6: a picture of the velocity buffer's error: where a pixel takes its velocity, red the
        # velocity's x less the camera's reprojection's (|.| x 20, screen units), green y, blue 0.5
        6: (detour(NOVEL_AT, 4, "v_cmp_lt_f32_e32 vcc, 0, v0",
                   ["v_madak_f32 v67, v22, v0, 0xc000412b", "v_sub_f32_e32 v66, v67, v35",
                    "v_sub_f32_e32 v67, v1, v31", "v_mul_f32_e32 v66, 0x41a00000, v66",
                    "v_mul_f32_e32 v67, 0x41a00000, v67", "v_max_f32_e64 v66, |v66|, |v66|",
                    "v_max_f32_e64 v67, |v67|, |v67|", "v_cndmask_b32_e32 v66, 0, v66",
                    "v_cndmask_b32_e32 v67, 0, v67", "v_mov_b32_e32 v68, 0.5", "v_cndmask_b32_e32 v68, 0, v68"])
            + [(OUT_AT, b"".join(asm(["v_cvt_pkrtz_f16_f32_e32 v1, v66, v67", "v_cvt_pkrtz_f16_f32_e32 v0, v68, v3"])))], rsrc(69)),
        7: (velfix_patches(+1, False), rsrc(69)),  # the engine's blend, the velocity fixed
        8: (velfix_patches(+1, True), rsrc(69)),   # motion-weighted, the velocity fixed
        9: (velfix_patches(-1, False), rsrc(69)),  # (the fix's sign the other way: a check)
        # 10: a probe (with the engine's blend): one pixel's numbers into the agent's memory each
        # frame (its address and the pixel, 0xd1a90001-04 here, set by the agent: taa_build): the raw
        # velocity x, the decoded velocity y, the camera's reprojection x and y, 1 if it takes its
        # velocity, its place, and the view buffer's ViewToClip M[2][0..1] and PrevViewToClip's
        # (s84-s87, loaded with ClipToPrevClip; the shader then 88 SGPRs)
        10: (probe_patches(), rsrc(71) & ~(0xf << 6) | (10 << 6)),
        11: (velfix2_patches(False), rsrc(73) & ~(0xf << 6) | (11 << 6)),  # the engine's blend, the velocity fixed (2)
        12: (velfix2_patches(True), rsrc(73) & ~(0xf << 6) | (11 << 6)),   # motion-weighted, the velocity fixed (2)
        13: (fresh_patches(*velfix2_patches(None)), rsrc(73) & ~(0xf << 6) | (11 << 6)),  # 11, new pixels converged quickly
        14: (fresh_patches(*velfix2_patches(None), soft=0.25), rsrc(75) & ~(0xf << 6) | (11 << 6)),  # 13, new pixels from a soft picture at 1/4
        15: (fresh_patches(*velfix2_patches(None), soft=0.125), rsrc(75) & ~(0xf << 6) | (11 << 6)),  # the same at 1/8
    }
    return variants


# base 1's 13: its weights' layout, the shader's V# and the agent's block both from these (taa_patch.h's
# TAA_WEIGHTS_OFF40, TAA_WEIGHTS_PLANE40, TAA_WEIGHTS40): the weights at the slots' page + 0x200 +
# WEIGHTS_OFF40, four planes (two a view, in turn) of WEIGHTS_PLANE40 bytes, one a 2x2 block (insets
# up to 0x40000 blocks: 1448x724; the agent runs 11 for a larger one)
WEIGHTS_OFF40, WEIGHTS_PLANE40 = 0x1e00, 0x40000


# ---- the velocity fix's bodies, shared by the bases (the registers renamed per base) ----
IDENT = {}


def velfix_body(sign, m):
    """at the velocity's test, after the original v_cmp: the window's move since last frame from
    the view buffer's own previous projection, this frame's ViewToClip M[2][0..1] (s76-s77) less
    PrevViewToClip's (s78-s79, dword 284; both loaded at the view buffer's load), rounded to the
    window's 2-pixel steps (s40, s41 half the viewport in pixels), added to the decoded velocity
    y (v1) and the raw x (v0); v66-v68 scratch. Hikari's first form (7-9), where dword 284 holds
    something else (-2, 0: the probe); the form the newer engine's PS40 uses (11, in the insets only),
    whose buffer has the frame before's jittered projection there (taa40-1's probe)"""
    k = "" if sign > 0 else "-"
    return ren(["s_waitcnt lgkmcnt(0)",
                "v_mov_b32_e32 v67, s78", "v_sub_f32_e32 v66, s76, v67", "v_mul_f32_e32 v66, s40, v66",
                "v_mul_f32_e32 v66, 0.5, v66", "v_rndne_f32_e32 v66, v66", "v_rcp_f32_e32 v67, s40",
                "v_mul_f32_e32 v66, %s2.0, v66" % k, "v_mul_f32_e32 v66, v66, v67",
                "v_mov_b32_e32 v68, s79", "v_sub_f32_e32 v67, s77, v68", "v_mul_f32_e32 v67, s41, v67",
                "v_mul_f32_e32 v67, 0.5, v67", "v_rndne_f32_e32 v67, v67", "v_rcp_f32_e32 v68, s41",
                "v_mul_f32_e32 v67, %s2.0, v67" % k, "v_mul_f32_e32 v67, v67, v68",
                "v_add_f32_e32 v1, v1, v67", "v_mac_f32_e32 v0, 0x3e7f7ced, v66"], m)


def ren(lines, m):
    """the lines with the registers renamed by m: {"v24": "v22", "s40": "s48", ...} (whole registers
    and the same inside ranges, v[69:72] -> v[37:40]); no map: the lines as they are"""
    if not m:
        return list(lines)
    def one(r):
        return m.get(r, r)
    def rng(mm):
        k, a, b = mm.group(1), int(mm.group(2)), int(mm.group(3))
        a2, b2 = one("%s%d" % (k, a)), one("%s%d" % (k, b))
        return "%s[%s:%s]" % (k, a2[1:], b2[1:])
    out = []
    for ln in lines:
        ln = re.sub(r"\b([vs])\[(\d+):(\d+)\]", rng, ln)
        ln = re.sub(r"\b([vs]\d+)\b", lambda mm: one(mm.group(1)), ln)
        out.append(ln)
    return out


# The frame counter Chisato's and Allison's shader lacks (no dither; the engine's FrameNumber is
# in the Frame uniform buffer, which that shader does not bind): one of the agent's own, a dword
# after the views' slots (+128 of the slots' V#, 144 bytes long here), which the left eye's TAA
# draw (view 0) raises by one each frame: its one middle pixel (the pixel whose column and row
# the viewport's centre falls in) loads it and stores it one more. The insets' draws come after
# the eyes' in the frame (each view's post-processing in turn, with the engine's barriers
# between), so every inset pixel reads the same count for the frame, one more than the count its
# view stored with last frame's slot: Hikari's check (the slot's counter one less) holds as it is.
COUNTER40 = ["s_mov_b32 s88, 0xd1a90001", "s_mov_b32 s89, 0xd1a90002", "s_mov_b32 s90, 144", "s_mov_b32 s91, 0x24fac",
             "buffer_load_dword v71, off, s[88:91], 0 offset:128 glc", "s_waitcnt vmcnt(0)", "v_readfirstlane_b32 s78, v71",
             "s_cmp_eq_u32 s79, 0", "s_cbranch_scc0 @nobump",
             "v_mov_b32_e32 v68, s42", "v_mac_f32_e32 v68, s40, v24", "v_floor_f32_e32 v68, v68", "v_mov_b32_e32 v70, s42",
             "v_floor_f32_e32 v70, v70", "v_cmp_eq_f32_e32 vcc, v68, v70", "s_and_b64 exec, exec, vcc",
             "v_mov_b32_e32 v68, s43", "v_mac_f32_e32 v68, s41, v23", "v_floor_f32_e32 v68, v68", "v_mov_b32_e32 v70, s43",
             "v_floor_f32_e32 v70, v70", "v_cmp_eq_f32_e32 vcc, v68, v70", "s_and_b64 exec, exec, vcc",
             "v_add_i32_e32 v71, vcc, 1, v71", "buffer_store_dword v71, off, s[88:91], 0 offset:128 glc",
             "s_mov_b64 exec, s[82:83]",
             "nobump:"]


def view_lines(sreg="s79", va="v69", vb="v70"):
    """the view a pixel's wave draws (its scalar sreg): 2 (y below the eyes) + 1 (x right of the
    eyes' middle), the eyes' height 0xd1a90007 and the left eye's right edge 0xd1a90008 filled in
    by the agent; in base 0's registers (s42-s43 the viewport's centre; va, vb scratch, vcc
    overwritten)"""
    a, b = va, vb
    return ["v_mov_b32_e32 %s, s43" % a, "v_cmp_lt_f32_e32 vcc, 0xd1a90007, %s" % a, "v_mov_b32_e32 %s, 0" % a,
            "v_mov_b32_e32 %s, 2.0" % b, "v_cndmask_b32_e32 %s, 0, %s" % (a, b), "v_mov_b32_e32 %s, s42" % b,
            "v_cmp_lt_f32_e32 vcc, 0xd1a90008, %s" % b,
            "v_mov_b32_e32 %s, 1.0" % b, "v_cndmask_b32_e32 %s, 0, %s" % (b, b), "v_add_f32_e32 %s, %s, %s" % (a, a, b),
            "v_cvt_u32_f32_e32 %s, %s" % (a, a), "v_readfirstlane_b32 %s, %s" % (sreg, a)]


def velfix2_body(m, counter=()):
    """at the velocity's test, after the original v_cmp (vcc = the pixel takes its velocity): the
    move of the inset's window since last frame added to the velocity (docs/MULTIVIEW.md 30; the
    comment at hikari_variants' velfix2_patches). Written in base 0's registers: s76-s77 this
    frame's ViewToClip M[2][0..1], s78 the frame counter (loaded at the view buffer's load, or by
    `counter`, lines after the view is known), s40-s41 half the viewport's size in pixels and
    s42-s43 its centre (ScreenPosToPixel), v24, v23 the pixel's screen place, v1 the decoded
    velocity y, v0 the raw velocity x; v66-v72 and s79-s91 scratch; renamed by m for another base"""
    return ren(["s_mov_b64 s[80:81], vcc", "s_mov_b64 s[82:83], exec", "s_waitcnt lgkmcnt(0)",
                # the view (view_lines)
                ] + view_lines() + list(counter) + [
                # slot offsets: (view x 2 + parity) x 16 for this frame (s84), the other parity (s85)
                "s_and_b32 s86, s78, 1", "s_lshl_b32 s84, s79, 1", "s_add_u32 s84, s84, s86",
                "s_xor_b32 s85, s84, 1", "s_lshl_b32 s84, s84, 4", "s_lshl_b32 s85, s85, 4",
                "s_mov_b32 s88, 0xd1a90001",
                "s_mov_b32 s89, 0xd1a90002", "s_mov_b32 s90, 128", "s_mov_b32 s91, 0x24fac",
                # last frame's slot for this view: M[2][0], M[2][1], its frame counter, its key
                "buffer_load_dwordx4 v[69:72], off, s[88:91], s85 glc", "s_waitcnt vmcnt(0)",
                # taken (s[86:87]) only for an inset, its own key, the frame before's counter
                "s_cmp_ge_u32 s79, 2", "s_cselect_b64 s[86:87], -1, 0",
                "v_mov_b32_e32 v68, s42", "v_mov_b32_e32 v66, 0x45800000", "v_mac_f32_e32 v68, s43, v66",
                "v_cmp_eq_f32_e32 vcc, v68, v72", "s_and_b64 s[86:87], s[86:87], vcc",
                "v_sub_i32_e32 v71, vcc, s78, v71", "v_and_b32_e32 v71, 7, v71", "v_cmp_eq_u32_e32 vcc, 1, v71",
                "s_and_b64 s[86:87], s[86:87], vcc",
                # the move, x then y, rounded to the window's 2-pixel steps, 0 past 4096 of them (any
                # move within a view: the slot's key and frame counter already refuse a stale or
                # first frame; the cap of 75 steps, 150 pixels, it had dropped every larger jump of
                # the window, a look away and back, a saccade: the velocity objects' history then
                # taken from the wrong place in the old window, the pop-in the user saw on Hikari
                # and the date cards, never the background)
                "v_sub_f32_e32 v66, s76, v69", "v_mul_f32_e32 v66, s40, v66", "v_mul_f32_e32 v66, 0.5, v66",
                "v_rndne_f32_e32 v66, v66", "v_mov_b32_e32 v68, 0x45800000", "v_cmp_gt_f32_e64 vcc, v68, |v66|",
                "v_cndmask_b32_e32 v66, 0, v66", "v_rcp_f32_e32 v68, s40", "v_mul_f32_e32 v66, 2.0, v66",
                "v_mul_f32_e32 v66, v66, v68",
                "v_sub_f32_e32 v67, s77, v70", "v_mul_f32_e32 v67, s41, v67", "v_mul_f32_e32 v67, 0.5, v67",
                "v_rndne_f32_e32 v67, v67", "v_mov_b32_e32 v68, 0x45800000", "v_cmp_gt_f32_e64 vcc, v68, |v67|",
                "v_cndmask_b32_e32 v67, 0, v67", "v_rcp_f32_e32 v68, s41", "v_mul_f32_e32 v67, 2.0, v67",
                "v_mul_f32_e32 v67, v67, v68",
                "s_mov_b64 vcc, s[86:87]", "v_cndmask_b32_e32 v66, 0, v66", "v_cndmask_b32_e32 v67, 0, v67",
                # this frame's slot: the pixels within a pixel of the view's middle, both ways (s40,
                # s41 half the view's size in pixels; a few store the same four numbers)
                "v_mov_b32_e32 v72, s42", "v_mov_b32_e32 v71, 0x45800000", "v_mac_f32_e32 v72, s43, v71",
                "v_mov_b32_e32 v71, s78", "v_mov_b32_e32 v69, s76", "v_mov_b32_e32 v70, s77",
                "v_mul_f32_e32 v68, s40, v24", "v_cmp_gt_f32_e64 vcc, 1.0, |v68|", "s_and_b64 exec, exec, vcc",
                "v_mul_f32_e32 v68, s41, v23", "v_cmp_gt_f32_e64 vcc, 1.0, |v68|", "s_and_b64 exec, exec, vcc",
                "buffer_store_dwordx4 v[69:72], off, s[88:91], s84 glc",
                "s_mov_b64 exec, s[82:83]", "s_mov_b64 vcc, s[80:81]",
                "v_add_f32_e32 v1, v1, v67", "v_mac_f32_e32 v0, 0x3e7f7ced, v66"], m)


# ---- base 1: Chisato's and Allison's FPostProcessTemporalAAPS<4,0> ----
# base 0's registers renamed: the added code in v32-v42 (the shader's own end at v31), the same
# SGPRs (s76 on; the shader's own end at s59); ScreenPosToPixel s48-s51, the screen place v22, v10
REN40 = {"v%d" % (64 + k): "v%d" % (32 + k) for k in range(12)}
REN40.update({"s40": "s48", "s41": "s49", "s42": "s50", "s43": "s51", "v24": "v22", "v23": "v10"})
# which variant base 1 builds in place of one it has no code for
# (8 as 7; 13's quick convergence keeps its weight in the agent's memory, fresh_buf: this engine's
# TAA history is R11G11B10, no alpha for it, taa40-4's probe read its T#'s data format 6)
ALIAS40 = {2: 1, 5: 4, 6: 1, 8: 7, 9: 7, 12: 11, 14: 13, 15: 13}


class Placer:
    """detours after a shader's end: each site's instruction(s) replaced by a branch to code there
    that does what they did (or something else), then branches back"""

    def __init__(self, B):
        self.B, self.at, self.out = B, B.len, []

    def check(self, site, lines):
        want = b"".join(asm(lines))
        assert self.B.code[site:site + len(want)] == want, "0x%x is not %s" % (site + self.B.off, lines)
        return len(want)

    def detour(self, site, slen, lines, labels=None):
        """the detour's code: lines (labels resolved: a line "name:" marks a place, "s_cbranch_x @name"
        branches to it); the site's slen bytes become the branch out (and s_nop)"""
        lines = list(lines)
        names = {}
        plain = []
        for ln in lines:
            if ln.endswith(":"):
                names[ln[:-1]] = len(plain)
            else:
                plain.append(ln)
        sizes = [len(x) for x in asm([re.sub(r"@\w+", "0", ln) for ln in plain])]
        pos = [sum(sizes[:i]) for i in range(len(plain) + 1)]
        fixed = []
        for i, ln in enumerate(plain):
            mm = re.search(r"@(\w+)", ln)
            if mm:
                ln = ln.replace(mm.group(0), str((pos[names[mm.group(1)]] - pos[i + 1]) // 4))
            fixed.append(ln)
        pre = asm(fixed)
        assert [len(x) for x in pre] == sizes
        n = sum(sizes)
        tramp = b"".join(pre) + branch(self.at + n, site + slen)
        self.out += [(site, branch(site, self.at) + b"".join(asm(["s_nop 0"]) * ((slen - 4) // 4))), (self.at, tramp)]
        self.at += len(tramp)
        assert self.at <= 0x1000 - 0x10, "the added code past the copy's page"

    def replace(self, site, was, now):
        n = self.check(site, [was])
        b = asm([now])[0]
        assert len(b) == n, "%s is not %d bytes" % (now, n)
        self.out.append((site, b))


def type40_variants(B):
    O = B.off
    LOAD_AT, NOVEL_AT, T1_AT, OFF_AT = 0x1c0 - O, 0x2f0 - O, 0x318 - O, 0x468 - O
    HIST_AT, W_AT, OUT_AT = 0x450 - O, 0x74c - O, 0x888 - O
    HISTPX_AT, HISTPX = 0x334 - O, "v_add_f32_e32 v0, 0.5, v8"  # (v8, v13 the history's pixel by now: 0x32c, 0x330)
    BLENDS = [(0x760 - O, "v_mac_f32_e32 v15, s7, v13"), (0x7cc - O, "v_mac_f32_e32 v9, s7, v7"), (0x838 - O, "v_mac_f32_e32 v2, s7, v1")]
    LOAD = "s_buffer_load_dwordx16 s[16:31], s[0:3], 0x1b0"  # ClipToPrevClip; the view buffer's V# in s[0:3] till 0x1e8
    NOVEL = "v_cmp_lt_f32_e32 vcc, 0, v0"                     # the pixel's velocity used (x > 0)
    OFF = "v_cmp_le_f32_e64 s[0:1], 1.0, v22"                 # the history off the view (max |hist.xy| >= 1)
    HIST = "image_sample_lz v[8:10], v[13:14], s[32:39], s[52:55] dmask:0x7"  # the history's first Catmull-Rom tap, RGB
    W = "v_cndmask_b32_e64 v15, v11, v13, s[0:1]"             # the history's Y (clamped), or the filtered frame's off the view
    OUT = "v_cvt_pkrtz_f16_f32_e64 v1, -v1, 0"                # the output's B and alpha (0)
    p0 = Placer(B)
    for site, lines in ((LOAD_AT, [LOAD]), (NOVEL_AT, [NOVEL]), (HISTPX_AT, [HISTPX]), (T1_AT, ["v_max_f32_e64 v22, |v10|, |v0|"]), (OFF_AT, [OFF]),
                        (HIST_AT, [HIST]), (W_AT, [W]), (OUT_AT, [OUT])):
        p0.check(site, lines)
    for site, was in BLENDS:
        p0.check(site, [was])
    rs = lambda v: B.rsrc(v, 92)  # (s76-s91 used: 96 SGPRs; the VGPRs as the variant needs)

    def nohist():
        # (and the output's R and G swapped: a picture no one can miss, so a capture tells that the
        # copy is what draws the view; the no-history shimmer alone was hard to tell apart from a
        # scene's own changes, taa40-1)
        p = Placer(B)
        p.out.append((OFF_AT, asm(["s_mov_b64 s[0:1], -1"])[0] + asm(["s_nop 0"])[0]))
        p.replace(OUT_AT - 8, "v_cvt_pkrtz_f16_f32_e64 v0, -v0, -v2", "v_cvt_pkrtz_f16_f32_e64 v0, -v2, -v0")
        return p.out

    def novel():
        p = Placer(B)
        p.replace(NOVEL_AT, NOVEL, "v_cmp_f_f32_e32 vcc, 0, v0")
        return p.out

    # the loads the fix needs, before the original (which overwrites s16-s31; the V# in s[0:3] is
    # read by both, a scalar load doesn't wait for registers another still writes)
    FIX_LOADS = ["s_buffer_load_dwordx2 s[76:77], s[0:3], 0x68"]  # ViewToClip M[2][0..1] (the frame counter: COUNTER40)

    def probe():
        """the view uniform buffer's first 960 dwords (its V# s[0:3] copied to s[84:87] at the load),
        then M[2][0..1], ScreenPosToPixel and the pixel's screen place at +0xf00, stored each frame
        by the pixels within a pixel of the left inset's middle into the agent's page
        (0xd1a90001-02): for the buffer's layout (a frame counter, the previous projection)"""
        p = Placer(B)
        p.detour(LOAD_AT, 8, FIX_LOADS + ["s_mov_b64 s[84:85], s[0:1]", "s_mov_b64 s[86:87], s[2:3]", LOAD])
        p.detour(NOVEL_AT, 4, [NOVEL] + ren([
            "s_mov_b64 s[80:81], vcc", "s_mov_b64 s[82:83], exec", "s_waitcnt lgkmcnt(0)"] + view_lines() + [
            "s_cmp_eq_u32 s79, 2", "s_cbranch_scc0 @skip",
            "v_mul_f32_e32 v68, s40, v24", "v_cmp_gt_f32_e64 vcc, 1.0, |v68|", "s_and_b64 exec, exec, vcc",
            "v_mul_f32_e32 v68, s41, v23", "v_cmp_gt_f32_e64 vcc, 1.0, |v68|", "s_and_b64 exec, exec, vcc",
            "s_cbranch_execz @skip",
            "s_mov_b32 s88, 0xd1a90001", "s_mov_b32 s89, 0xd1a90002", "s_mov_b32 s90, 0x1000", "s_mov_b32 s91, 0x24fac",
            "s_mov_b32 s92, 0",
            "loop:",
            "buffer_load_dwordx4 v[68:71], off, s[84:87], s92", "s_waitcnt vmcnt(0)",
            "buffer_store_dwordx4 v[68:71], off, s[88:91], s92",
            "s_add_u32 s92, s92, 16", "s_cmp_lt_u32 s92, 0xf00", "s_cbranch_scc1 @loop",
            "v_mov_b32_e32 v68, s76", "v_mov_b32_e32 v69, s77", "v_mov_b32_e32 v70, s40", "v_mov_b32_e32 v71, s41",
            "buffer_store_dwordx4 v[68:71], off, s[88:91], 0 offset:0xf00",
            "s_nop 1",  # (a 128-bit store's data must not be written by the VALU the next cycle)
            "v_mov_b32_e32 v68, s42", "v_mov_b32_e32 v69, s43", "v_mov_b32_e32 v70, v24", "v_mov_b32_e32 v71, v23",
            "buffer_store_dwordx4 v[68:71], off, s[88:91], 0 offset:0xf10",
            "s_nop 1",
            # the history's T# (s[32:39], loaded at 0x2b0, waited above): its data format (has it an
            # alpha for the quick convergence's weight?)
            "v_mov_b32_e32 v68, s32", "v_mov_b32_e32 v69, s33", "v_mov_b32_e32 v70, s34", "v_mov_b32_e32 v71, s35",
            "buffer_store_dwordx4 v[68:71], off, s[88:91], 0 offset:0xf20",
            "s_nop 1",
            "v_mov_b32_e32 v68, s36", "v_mov_b32_e32 v69, s37", "v_mov_b32_e32 v70, s38", "v_mov_b32_e32 v71, s39",
            "buffer_store_dwordx4 v[68:71], off, s[88:91], 0 offset:0xf30",
            "skip:",
            "s_mov_b64 exec, s[82:83]", "s_mov_b64 vcc, s[80:81]"], REN40))
        return p.out

    # the velocity fix from the view buffer's own previous projection (PrevViewToClip, dword 284:
    # in this engine's buffer the frame before's jittered projection, taa40-1's probe): no slots,
    # no stores, no counter; inset mode's 11 and 13
    def velfix_prev():
        p = Placer(B)
        p.detour(LOAD_AT, 8, FIX_LOADS + ["s_buffer_load_dwordx2 s[78:79], s[0:3], 0x11c", LOAD])
        # the insets only, as Hikari's 11-15 (s79 >= 2): an eye's own two projections differ by
        # its jitter alone, which would round to a 2-pixel move where it reaches half a step; the
        # view found in s82 (s79 holds PrevViewToClip's M[2][1]) with the body's own scratch v66-v67
        # (no more VGPRs), vcc kept in s[80:81]
        p.detour(NOVEL_AT, 4, [NOVEL] + ren(["s_mov_b64 s[80:81], vcc", "s_waitcnt lgkmcnt(0)"] + view_lines("s82", "v66", "v67") +
                                            ["s_mov_b64 vcc, s[80:81]", "s_cmp_lt_u32 s82, 2", "s_cbranch_scc1 @eye"], REN40) +
                 velfix_body(+1, REN40) + ["eye:"])
        return p.out

    # 13 on this shader: 11 with newly uncovered inset pixels converged as a running average, the
    # weight kept in the agent's memory since this history (R11G11B10) has no alpha. One byte per
    # 2x2 block of an inset's pixels, two planes a view, written and read in turn by the frame
    # counter's parity (the left eye's middle pixel raises it, as COUNTER40; the insets are drawn
    # after the eyes): this frame's in the counter's plane, last frame's read from the other at the
    # history's place (0x32c-0x330: v8, v13 its pixel). The byte is the next frame's weight x 255:
    # w / (1 + w), 0.5 where the history is off the view (s[0:1]); 0, never written or decayed,
    # reads as the engine's own s7. The eyes keep s7 (v40 = 0) and store nothing.
    # The memory: the slots' page + 0x200 (0xd1a90001-02) + WOFF, PLANE bytes a plane (insets up to
    # 1448x724, 2x2 blocks), a V# whose num_records ends at the last plane: a store or load past it
    # is dropped by the hardware, so nothing outside the agent's block is ever touched.
    WOFF, PLANE = WEIGHTS_OFF40, WEIGHTS_PLANE40
    NREC = WOFF + 4 * PLANE

    def fresh_buf():
        p = Placer(B)
        p.detour(LOAD_AT, 8, FIX_LOADS + ["s_buffer_load_dwordx2 s[78:79], s[0:3], 0x11c", LOAD])
        # the view (s82), the V# (s[84:87]), the counter (s83, raised by the left eye's middle
        # pixel), this pixel's block (v37, -1 off the insets) and the two planes' offsets (s89 this
        # frame's, s90 last frame's); vcc kept in s[80:81], exec in s[88:89] (before s88-s90 take the planes)
        pre = ren(["s_mov_b64 s[80:81], vcc", "s_waitcnt lgkmcnt(0)"] + view_lines("s82", "v66", "v67"), REN40) + [
            "s_mov_b32 s84, 0xd1a90001", "s_mov_b32 s85, 0xd1a90002", "s_mov_b32 s86, 0x%x" % NREC, "s_mov_b32 s87, 0x24fac",
            "buffer_load_dword v37, off, s[84:87], 0 offset:128 glc", "s_waitcnt vmcnt(0)", "v_readfirstlane_b32 s83, v37",
            "s_cmp_eq_u32 s82, 0", "s_cbranch_scc0 @nobump",
            "s_mov_b64 s[88:89], exec",
            "v_mov_b32_e32 v38, s50", "v_mac_f32_e32 v38, s48, v22", "v_floor_f32_e32 v38, v38", "v_floor_f32_e32 v39, s50",
            "v_cmp_eq_f32_e32 vcc, v38, v39", "s_and_b64 exec, exec, vcc",
            "v_mov_b32_e32 v38, s51", "v_mac_f32_e32 v38, s49, v10", "v_floor_f32_e32 v38, v38", "v_floor_f32_e32 v39, s51",
            "v_cmp_eq_f32_e32 vcc, v38, v39", "s_and_b64 exec, exec, vcc",
            "v_add_i32_e32 v37, vcc, 1, v37", "buffer_store_dword v37, off, s[84:87], 0 offset:128 glc",
            "s_mov_b64 exec, s[88:89]",
            "nobump:",
            "v_mov_b32_e32 v37, -1",
            "s_cmp_lt_u32 s82, 2", "s_cbranch_scc1 @noidx",
            # planes: (parity x 2 + the inset) x PLANE + WOFF, this frame's and last frame's
            "s_and_b32 s88, s83, 1", "s_lshl_b32 s89, s88, 1", "s_add_u32 s89, s89, s82", "s_sub_u32 s89, s89, 2",
            "s_mul_i32 s89, s89, 0x%x" % PLANE, "s_add_u32 s89, s89, 0x%x" % WOFF,
            "s_xor_b32 s88, s88, 1", "s_lshl_b32 s90, s88, 1", "s_add_u32 s90, s90, s82", "s_sub_u32 s90, s90, 2",
            "s_mul_i32 s90, s90, 0x%x" % PLANE, "s_add_u32 s90, s90, 0x%x" % WOFF,
        ] + block_index("v37", "v38", "v39", "v22*", "v10*") + [
            "noidx:",
            "s_mov_b64 vcc, s[80:81]", "s_cmp_lt_u32 s82, 2", "s_cbranch_scc1 @eye"]
        p.detour(NOVEL_AT, 4, [NOVEL] + pre + velfix_body(+1, REN40) + ["eye:"])
        # last frame's weight at the history's place (v8, v13: its pixel, absolute), into v40
        p.detour(HISTPX_AT, 4, [HISTPX, "v_mov_b32_e32 v40, 0", "s_cmp_lt_u32 s82, 2", "s_cbranch_scc1 @hx"] +
                 block_index("v38", "v38", "v39", "v8", "v13") +
                 ["buffer_load_ubyte v40, v38, s[84:87], s90 offen", "s_waitcnt vmcnt(0)",
                  "v_cvt_f32_ubyte0_e32 v40, v40", "v_mul_f32_e32 v40, 0x3b808081, v40",  # (1/255)
                  "hx:"])
        # the weight w = last frame's within s7 and 1 (v32), the next one (v33)
        p.detour(W_AT, 8, [W, "v_max_f32_e32 v32, s7, v40", "v_min_f32_e32 v32, 1.0, v32",
                           "v_add_f32_e32 v33, 1.0, v32", "v_rcp_f32_e32 v33, v33", "v_mul_f32_e32 v33, v32, v33",
                           "v_mov_b32_e32 v34, 0.5", "v_cndmask_b32_e64 v33, v33, v34, s[0:1]"])
        for site, was in BLENDS:
            p.replace(site, was, was.replace("s7", "v32"))
        # this frame's byte, for the insets
        p.detour(OUT_AT, 8, [OUT, "s_cmp_lt_u32 s82, 2", "s_cbranch_scc1 @ox",
                             "v_mul_f32_e32 v33, 0x437f0000, v33", "v_cvt_u32_f32_e32 v33, v33",  # (x 255)
                             "buffer_store_byte v33, v37, s[84:87], s89 offen", "ox:"])
        return p.out

    def block_index(dst, ta, tb, x, y):
        """the 2x2 block's byte offset in a plane (u32 in dst) of the inset pixel: x, y its absolute
        pixel (VGPRs), or, ending in *, its screen place (then made a pixel with ScreenPosToPixel,
        s48-s51); local to the view's rect (its top left s50 - s48, s51 + s49), blocks s48 a row;
        clamped to the plane"""
        if x.endswith("*"):
            xs = ["v_mov_b32_e32 %s, s48" % ta, "v_mac_f32_e32 %s, s48, %s" % (ta, x[:-1])]          # s48 (x + 1)
            ys = ["v_add_f32_e32 %s, -1.0, %s" % (tb, y[:-1]), "v_mul_f32_e32 %s, s49, %s" % (tb, tb)]  # s49 (y - 1)
        else:
            xs = ["v_subrev_f32_e32 %s, s50, %s" % (ta, x), "v_add_f32_e32 %s, s48, %s" % (ta, ta)]   # x - s50 + s48
            ys = ["v_subrev_f32_e32 %s, s51, %s" % (tb, y), "v_subrev_f32_e32 %s, s49, %s" % (tb, tb)]  # y - s51 - s49
        return xs + ys + [
            "v_mul_f32_e32 %s, 0.5, %s" % (ta, ta), "v_floor_f32_e32 %s, %s" % (ta, ta), "v_max_f32_e32 %s, 0, %s" % (ta, ta),
            "v_mul_f32_e32 %s, 0.5, %s" % (tb, tb), "v_floor_f32_e32 %s, %s" % (tb, tb), "v_max_f32_e32 %s, 0, %s" % (tb, tb),
            "v_mac_f32_e32 %s, s48, %s" % (ta, tb),
            "v_min_f32_e32 %s, 0x%x, %s" % (ta, f32bits(PLANE - 1), ta),
            "v_cvt_u32_f32_e32 %s, %s" % (dst, ta)]

    # the same from each view's last projection kept in the agent's slots, with the agent's
    # own frame counter (COUNTER40): 7 and 8 here, for comparison
    def velfix_ring():
        p = Placer(B)
        p.detour(LOAD_AT, 8, FIX_LOADS + [LOAD])
        p.detour(NOVEL_AT, 4, [NOVEL] + velfix2_body(REN40, COUNTER40))
        return p.out

    variants = {
        1: ([], B.regs[2]),
        3: (nohist(), B.regs[2]),
        4: (novel(), B.regs[2]),
        7: (velfix_ring(), rs(41)),
        10: (probe(), rs(40)),
        11: (velfix_prev(), rs(37)),
        13: (fresh_buf(), rs(41)),
    }
    for v, a in ALIAS40.items():
        variants[v] = (None, a)
    return variants


def emit(bases):
    print("/* agent/taa_patch.h: made by tools/gen_taa_patch.py from the engines' TAA shader blocks: Hikari's PS10")
    print(" * (logs/aa4-block), Chisato's PS40 (logs/pop10; Allison's is the same code). */")
    print("#define TAA_PSSHADER_OFF 0x34 /* the Gnm::PsShader in a block; its stage registers at +8 */")
    print("#define TAA_NVAR %d /* variants 1..%d */" % (NVAR, NVAR))
    print("#define TAA_NBASE %d /* the shaders patched: 0 Hikari 1.00's PS10 (build 1), 1 the newer engine's PS40 (builds 2-4: Chisato, Allison, Hikari 1.02) */" % len(bases))
    print("#define TAA_WEIGHTS_OFF40 0x%x /* base 1's 13: its weights from the slots' page + 0x200 */" % WEIGHTS_OFF40)
    print("#define TAA_WEIGHTS_PLANE40 0x%x /* a plane's bytes, one a 2x2 block of an inset */" % WEIGHTS_PLANE40)
    print("#define TAA_WEIGHTS40 0x%x /* the four planes */" % (4 * WEIGHTS_PLANE40))
    print("struct taa_patch { u16 at, len; const u8 *bytes; };")
    print("struct taa_variant { const struct taa_patch *p; u32 n, rsrc1, alias; }; /* alias: the variant built in its place (its own rsrc1), 0 none */")
    print("struct taa_base { const char *name; u32 code_off, code_len, fnv, rsrc1, inputs; const struct taa_variant *var; };")
    for bi, (B, variants) in enumerate(bases):
        for v, (pl, rs) in sorted(variants.items()):
            if pl is None:
                continue
            for i, (at, b) in enumerate(pl):
                print("static const u8 k_taa_b%d_v%d_%d[%d] = {%s};" % (bi, v, i, len(b), ", ".join("0x%02x" % x for x in b)))
            print("static const struct taa_patch k_taa_b%d_v%d[%d] = {%s};" % (bi, v, max(1, len(pl)), ", ".join(
                "{0x%x, %d, k_taa_b%d_v%d_%d}" % (at, len(b), bi, v, i) for i, (at, b) in enumerate(pl)) or "{0, 0, 0}"))
        rows = ["{0, 0, 0, 0}"]
        for v in range(1, NVAR + 1):
            pl, rs = variants[v]
            if pl is None:
                rows.append("{0, 0, 0x%08xu, %d}" % (variants[rs][1], rs))
            else:
                rows.append("{k_taa_b%d_v%d, %d, 0x%08xu, 0}" % (bi, v, len(pl), rs))
        print("static const struct taa_variant k_taa_b%d_var[%d] = {%s};" % (bi, NVAR + 1, ", ".join(rows)))
    print("static const struct taa_base k_taa_base[TAA_NBASE] = {%s};" % ", ".join(
        '{"%s", 0x%x, %d, 0x%08xu, 0x%08xu, 0x%x, k_taa_b%d_var}' % (B.name, B.off, B.len, fnv(B.code), B.regs[2], B.regs[6], bi)
        for bi, (B, variants) in enumerate(bases)))


def main(hikari, ps40):
    B0 = Base("Hikari's FPostProcessTemporalAAPS10", hikari, 0xa4, 2380, 0x002c024f, 0x320)
    B1 = Base("the newer engine's FPostProcessTemporalAAPS40 (Chisato's, Allison's, Hikari 1.02's)", ps40, 0xa0, 2048, 0x002c01c7, 0x20)
    emit([(B0, hikari_variants(B0)), (B1, type40_variants(B1))])


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
