/* The insets' layer with Hmd2's window test (vr2.c, agent.insetwin): its identity lookup table,
 * the table's gfx10 T#, and the layer record (registered as record 1), in a header for the host
 * test (tools/fov/insetwin_test.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* a float in (2^-14, 65504) as FP16 (normal, positive values only: the table's), truncated */
static inline u16 half_of(float v) {
  union { float f; u32 u; } x = {v};
  u32 e = (x.u >> 23) & 0xff, m = x.u & 0x7fffff;
  if (e < 113)
    return 0;
  return (u16)(((e - 112) << 10) | (m >> 13));
}
/* n RGBA16F texels, red = the texel's own centre (2i + 1) / 2n (exact in FP16 for n <= 1024),
 * alpha 1: sampled (bilinear, clamped) it gives back its coordinate */
static inline void lut_fill(u16 *t, u32 n) {
  for (u32 i = 0; i < n; i++) {
    t[4 * i + 0] = half_of((float)(2 * i + 1) / (float)(2 * n));
    t[4 * i + 1] = t[4 * i + 2] = 0;
    t[4 * i + 3] = 0x3c00;
  }
}
/* its gfx10 T#: 1D, 16_16_16_16 FLOAT (71), n wide, xyzw, linear (gen1.c makes it GCN's) */
static inline void lut_tsharp(u32 t[8], u64 base, u32 n) {
  u32 w = n - 1;
  for (int i = 0; i < 8; i++)
    t[i] = 0;
  t[0] = (u32)(base >> 8);
  t[1] = (u32)((base >> 40) & 0xff) | (71u << 20) | ((w & 3) << 30);
  t[2] = w >> 2; /* height 1 */
  t[3] = 0xfacu | (8u << 28);
}
/* record 1 (0x130 bytes): record 0's type and T#s, byte +0x44 = 1 (the window test), the table
 * as the four lookup T#s Hmd2 checks (+0x48, +0x68, +0xa8, +0xc8), the rest 0 (+0x88 and +0xe8
 * unused, +0x108 colour scale 0 = 1.0, +0x10c..+0x127 must be 0) */
static inline void win_record(u8 *r1, const u8 *r0, const u32 lut[8]) {
  memset(r1, 0, 0x130);
  memcpy(r1, r0, 0x44);
  r1[0x44] = 1;
  memcpy(r1 + 0x48, lut, 32), memcpy(r1 + 0x68, lut, 32);
  memcpy(r1 + 0xa8, lut, 32), memcpy(r1 + 0xc8, lut, 32);
}
