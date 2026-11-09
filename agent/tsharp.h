/* Texture descriptors: Hmd2's gfx10 (RDNA) T#s turned into GCN ones (gen1.c), in a header so the
 * host tests (tools/fov/insetwin_test.c) run the same code. Needs memcpy, u32/u64 and
 * g_display_pitch (vr2.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* gfx10 IMG FORMAT -> GCN data/number format, for the formats Hmd2 uses */
static int gfx10_fmt(u32 f, u32 *df, u32 *nf) {
  switch (f) {
  case 50: *df = 9, *nf = 0; return 1;   /* 2_10_10_10_UNORM: the headset */
  case 56: *df = 10, *nf = 0; return 1;  /* 8_8_8_8_UNORM */
  case 71: *df = 12, *nf = 7; return 1;  /* 16_16_16_16_FLOAT: Hmd2's 1D lookup table */
  case 130: *df = 10, *nf = 9; return 1; /* 8_8_8_8_SRGB: 1x1 placeholders */
  }
  return 0;
}

static int is_gfx10_tsharp(const u32 *t) {
  u32 type = t[3] >> 28, df, nf;
  if (type < 8 || type > 13 || !gfx10_fmt((t[1] >> 20) & 0x1ff, &df, &nf))
    return 0;
  for (int k = 0; k < 4; k++) {
    u32 sel = (t[3] >> (3 * k)) & 7;
    if (sel == 2 || sel == 3)
      return 0;
  }
  u64 base = ((u64)(t[1] & 0xff) << 40) | ((u64)t[0] << 8);
  return base >= 0x100000000ull && base < 0x10000000000ull;
}

/* linear only: GCN tiling index 8 (Display_LinearAligned) */
static void tsharp_to_gcn(u32 *t) {
  u32 df = 0, nf = 0, type = t[3] >> 28;
  gfx10_fmt((t[1] >> 20) & 0x1ff, &df, &nf);
  /* (the height 14 bits: other bits sit above it, restore_game_tsharp) */
  u32 w = (((t[2] & 0x3fff) << 2) | (t[1] >> 30)) + 1, h = ((t[2] >> 14) & 0x3fff) + 1;
  u32 pitch = w == 4000 ? g_display_pitch : (w + 63) & ~63u;
  u32 n[8] = {t[0],
              (t[1] & 0xff) | (df << 20) | (nf << 26),
              (w - 1) | ((h - 1) << 14) | (4u << 28),
              (t[3] & 0xfffff) | (8u << 20) | (type << 28),
              (pitch - 1) << 13,
              0, 0, 0};
  memcpy(t, n, 32);
}

