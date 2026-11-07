/* Host test of the insets' window layer (agent/insetwin.h) and gen1.c's descriptor conversion
 * (agent/tsharp.h): the identity table, its gfx10 T# and the GCN one gen1 makes of it, and the
 * registered record 1 against the checks Hmd2 makes (0x1b630).
 *   cc -O2 -I../../agent -o /tmp/iwtest insetwin_test.c && /tmp/iwtest */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
static volatile u32 g_display_pitch = 4032;
#include "tsharp.h"
#include "insetwin.h"

static float half_to_float(u16 h) {
  u32 e = (h >> 10) & 0x1f, m = h & 0x3ff;
  if (!e)
    return ldexpf((float)m, -24);
  return ldexpf(1.0f + (float)m / 1024.0f, (int)e - 15);
}
/* a 1D bilinear, clamp-to-edge sample of the red channel at normalized u (GCN: texel space
 * u * n - 0.5) */
static float sample(const u16 *t, u32 n, float u) {
  float x = u * (float)n - 0.5f;
  int i0 = (int)floorf(x);
  float f = x - (float)i0;
  int i1 = i0 + 1;
  i0 = i0 < 0 ? 0 : i0 > (int)n - 1 ? (int)n - 1 : i0;
  i1 = i1 < 0 ? 0 : i1 > (int)n - 1 ? (int)n - 1 : i1;
  return half_to_float(t[4 * i0]) * (1.0f - f) + half_to_float(t[4 * i1]) * f;
}

int main(void) {
  int bad = 0;
  const u32 n = 1024;
  static u16 lut[1024 * 4];
  lut_fill(lut, n);
  /* 1. the table: each texel exactly its centre, alpha 1 */
  for (u32 i = 0; i < n; i++) {
    float want = (float)(2 * i + 1) / (float)(2 * n), got = half_to_float(lut[4 * i]);
    if (got != want || lut[4 * i + 3] != 0x3c00 || lut[4 * i + 1] || lut[4 * i + 2]) {
      printf("FAIL: texel %u: %.9f, not %.9f (alpha 0x%x)\n", i, got, want, lut[4 * i + 3]), bad = 1;
      break;
    }
  }
  /* sampled, it gives its coordinate back: exact between the first and last centres, within
   * half a texel at the two ends (the clamp) */
  float worst_in = 0, worst_edge = 0;
  for (int k = 0; k <= 100000; k++) {
    float u = (float)k / 100000.0f, e = fabsf(sample(lut, n, u) - u);
    if (u >= 0.5f / n && u <= 1.0f - 0.5f / n)
      worst_in = e > worst_in ? e : worst_in;
    else
      worst_edge = e > worst_edge ? e : worst_edge;
  }
  if (worst_in > 1e-6f || worst_edge > 0.5f / n + 1e-6f)
    printf("FAIL: identity off by %g inside, %g at the ends\n", worst_in, worst_edge), bad = 1;
  else
    printf("  table: %u texels exact; sampled back off by %.1g inside, %.6f (half a texel) at the ends\n",
           n, worst_in, worst_edge);
  /* 2. the T#: a gfx10 one gen1.c takes, and the GCN one it makes */
  const u64 base = 0x313240000ull; /* (where the reserve put the agent's buffers in a run) */
  u32 t[8];
  lut_tsharp(t, base, n);
  u32 gw = (((t[2] & 0xfff) << 2) | (t[1] >> 30)) + 1; /* gfx10's width (what Hmd2 checks +0xa0 against) */
  if (!is_gfx10_tsharp(t) || gw != n || ((t[2] >> 14) & 0x3fff) != 0 || ((t[1] >> 20) & 0x1ff) != 71 ||
      (t[3] >> 28) != 8)
    printf("FAIL: the gfx10 T#: taken %d, width %u\n", is_gfx10_tsharp(t), gw), bad = 1;
  u32 g[8];
  memcpy(g, t, 32);
  tsharp_to_gcn(g);
  u64 gbase = ((u64)(g[1] & 0xff) << 40) | ((u64)g[0] << 8);
  u32 df = (g[1] >> 20) & 0x3f, nf = (g[1] >> 26) & 0xf, w = (g[2] & 0x3fff) + 1, h = ((g[2] >> 14) & 0x3fff) + 1;
  u32 type = g[3] >> 28, tile = (g[3] >> 20) & 0x1f, pitch = ((g[4] >> 13) & 0x3fff) + 1;
  if (gbase != base || df != 12 || nf != 7 || w != n || h != 1 || type != 8 || tile != 8 || pitch != n ||
      (g[3] & 0xfff) != 0xfac)
    printf("FAIL: GCN T#: base 0x%lx fmt %u/%u %ux%u type %u tiling %u pitch %u sel 0x%x\n",
           (unsigned long)gbase, df, nf, w, h, type, tile, pitch, g[3] & 0xfff), bad = 1;
  else
    printf("  T#: gfx10 71 1D %u wide taken by gen1.c -> GCN 16_16_16_16 FLOAT, %ux%u, 1D, linear, pitch %u, xyzw\n",
           n, w, h, pitch);
  /* 3. record 1 against Hmd2's registration checks (0x1b630, 0x1b7a0) */
  u8 rec[2][0x130];
  memset(rec, 0, sizeof rec);
  *(u32 *)rec[0] = 0x10;
  u32 eye[8] = {0x31f00000u, 0x00a00003u, 0x0, 0x0, 0, 0, 0, 0}; /* any T# with a base */
  memcpy(rec[0] + 0x04, eye, 32), memcpy(rec[0] + 0x24, eye, 32);
  win_record(rec[1], rec[0], t);
  for (int r = 0; r < 2; r++) {
    const u8 *x = rec[r];
    int ok = *(const u32 *)x == 0x10 && (*(const u32 *)(x + 0x04) || (x[0x08])) &&
             (*(const u32 *)(x + 0x24) || x[0x28]);
    for (int k = 0x45; k <= 0x47; k++)
      ok &= !x[k];
    for (int k = 0x10c; k < 0x128; k++)
      ok &= !x[k];
    if (x[0x44] == 1)
      for (int o = 0x48; o <= 0xc8; o += 0x20)
        if (o != 0x88)
          ok &= *(const u32 *)(x + o) != 0 && !memcmp(x + o, t, 32);
    if (r == 0)
      ok &= *(const u64 *)(x + 0x128) == 0 && x[0x44] == 0;
    else
      ok &= x[0x44] == 1 && !memcmp(x, rec[0], 0x44) && *(const u32 *)(x + 0x108) == 0;
    if (!ok)
      printf("FAIL: record %d would be refused\n", r), bad = 1;
  }
  if (!bad)
    printf("  records: 0 as before, 1 = 0 with +0x44 1 and the table at +0x48/+0x68/+0xa8/+0xc8: Hmd2's registration checks pass\n");
  printf(bad ? "FAIL\n" : "ok\n");
  return bad;
}
