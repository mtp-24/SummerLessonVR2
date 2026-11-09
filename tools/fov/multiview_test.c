/* agent/multiview.c's patch table applied to the game's own bytes on the host (make -C agent
 * first, for agent/nids_gen.h):
 *   cc -O2 -I../../agent -o /tmp/mvtest multiview_test.c && /tmp/mvtest <eboot.bin> <out.bin> [Chisato eboot] [Allison eboot] [Hikari 1.02 eboot]
 * Checks every entry's stock bytes against the eboot (file offset = address + 0x4000), writes the
 * patched code from 0x2f8ef50 to 0x2f8fc30 to <out.bin> for a disassembler.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#undef memcpy
#undef memset
#define memcpy agent_memcpy_unused
#define memset agent_memset_unused
#define memcmp agent_memcmp_unused
#define strlen agent_strlen_unused
#include "vr2.h"
#undef memcpy
#undef memset
#undef memcmp
#undef strlen
struct agent_header astro_hdr;
void agent_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("  log: ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
}
#define MV_HOST_TEST
void mv_count_stub(void) {}
static float g_fake_gaze[2];
static int g_fake_gaze_ok;
int fov_gaze(float t[2]) {
  t[0] = g_fake_gaze[0], t[1] = g_fake_gaze[1];
  return g_fake_gaze_ok;
}
static u64 g_fake_us;
static u64 fake_now(void) { return g_fake_us; }
u64 (*sceKernelGetProcessTime)(void) = fake_now;
static void *fake_self(void) { return (void *)1; }
int agent_snprintf(char *out, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return r < 0 ? 0 : (size_t)r >= n ? (int)n - 1 : r;
}
void *(*scePthreadSelf)(void) = fake_self;
#include "../../agent/multiview.c"
#include "../../agent/insetpp.c"

/* step 3 on the host: Draw's four calls for a frame (passes 1, 2, 2, 2) through mv_cs_wrap, with
 * the engine's two functions faked: the options as the plugin makes them for each eye, the view's
 * rects from the options (no screen percentage) */
static float k_proj[2][16];
static s32 k_rect[3][4] = {{0, 0, 960, 1080}, {960, 0, 1920, 1080}, {0, 0, 1920, 1624}};
static u8 g_view[3][0x1000]; /* (the game's views are larger: +0xa48 the vignette) */
static int g_nview;
static u32 g_fail_pass, g_fake_wide;
static u32 fake_opts(u64 lp, u8 *o, u64 vp, u64 drawer, u32 pass) {
  (void)lp, (void)vp, (void)drawer;
  if (pass && pass == g_fail_pass)
    return 0;
  memset(o, 0, 0x110);
  float org[3] = {pass == 1 ? -3.2f : pass == 2 ? 3.2f : 0.0f, 0, 0};
  memcpy(o, org, 12);
  float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  memcpy(o + 0x10, id, 64);
  memcpy(o + 0x50, k_proj[pass == 2], 64);
  memcpy(o + 0x90, k_rect[pass ? pass - 1 : 2], 16);
  memcpy(o + 0xa0, k_rect[pass ? pass - 1 : 2], 16);
  *(u64 *)(o + 0xb8) = 0x1234;
  *(u32 *)(o + 0x100) = pass;
  return 1;
}
static u8 g_last_opts[4][0x110];
static u64 fake_csv(u64 lp, u64 fam, u64 loc, u64 rot, u64 vp, u64 drawer, u32 pass) {
  (void)fam, (void)loc, (void)rot;
  u8 o[0x110];
  if (!(mv_opt_wrap(lp, o, vp, drawer, pass) & 0xff))
    return 0;
  memcpy(g_last_opts[g_nview & 3], o, sizeof o);
  u8 *v = g_view[g_nview++ % 3];
  *(float *)(v + 0xa48) = 0.4f; /* the engine's default vignette */
  for (int k = 0; k < 3; k++)
    memcpy(v + 0x38 + 16 * k, o + 0x90, 16);
  if (g_mv_k != 1.0f) { /* the engine: the view rect the options' times the screen percentage */
    s32 *r = (s32 *)(v + 0x38);
    for (int i = 0; i < 4; i++)
      r[i] = (s32)((float)r[i] * g_mv_k + 0.5f);
  }
  if (g_fake_wide && pass == 0) /* an unconstrained rect of the whole target */
    memcpy(v + 0x58, k_rect[2], 16);
  return (u64)v;
}
/* where a direction (x/z, y/z) lands in a projection's NDC */
static void ndc(const float *p, float tx, float ty, float *nx, float *ny) {
  /* a point at depth 1 (row vector times M): clip = (tx, ty, 1, 1) M */
  float w = tx * p[3] + ty * p[7] + p[11] + p[15];
  *nx = (tx * p[0] + ty * p[4] + p[8] + p[12]) / w;
  *ny = (tx * p[1] + ty * p[5] + p[9] + p[13]) / w;
}
static int step3_frames(void) {
  int bad = 0;
  /* PS VR2-like eyes: asymmetric, the lens centre off the middle */
  for (int e = 0; e < 2; e++) {
    float *p = k_proj[e];
    memset(p, 0, 64);
    p[0] = 0.92f, p[5] = 0.83f, p[8] = e ? -0.07f : 0.07f, p[9] = 0.03f, p[11] = 1.0f, p[14] = 10.0f;
  }
  o_csv = fake_csv;
  o_opts = fake_opts;
  /* the density: agent.insetppt=D pixels a unit of tangent both ways; the checks below with 0,
   * twice the periphery's */
  {
    float pe[16] = {0.92f, 0, 0, 0, 0, 0.83f, 0, 0, 0.07f, 0.03f, 0, 1.0f, 0, 0, 10.0f, 0};
    float wx, wy;
    g_opt_insetppt = 1200;
    inset_window(pe, 960, 1080, 872, 872, &wx, &wy);
    /* the window's half-width in tangent is wx / M00; 872 pixels over twice that */
    float dx = 872.0f / (2.0f * wx / pe[0]), dy = 872.0f / (2.0f * wy / pe[5]);
    if (fabsf(dx - 1200.0f) > 0.5f || fabsf(dy - 1200.0f) > 0.5f)
      printf("FAIL: agent.insetppt=1200 gives %f and %f\n", dx, dy), bad = 1;
    else
      printf("  agent.insetppt=1200: %.1f and %.1f pixels a unit of tangent across and up\n", dx, dy);
  }
  g_opt_insetppt = 0;
  for (int on = 0; on < 2; on++) {
    g_opt_insetviews = (u32)on;
    g_nview = 0;
    u32 passes[4] = {1, 2, 2, 2};
    int views = on ? 4 : 2;
    u64 got[4];
    for (int i = 0; i < views; i++)
      got[i] = mv_cs_wrap(1, 2, 3, 4, 5, 6, passes[i]);
    for (int i = 0; i < views; i++)
      if (!got[i])
        printf("FAIL: insetviews=%d view %d left out\n", on, i), bad = 1;
    if (!on)
      continue;
    for (int r = 0; r < 2; r++) {
      const u8 *o = g_last_opts[2 + r];
      const s32 *rc = (const s32 *)(o + 0x90);
      s32 want[4] = {r ? 1376 : 0, 1080, (r ? 1376 : 0) + 544, 1624};
      if (memcmp(rc, want, 16) || memcmp(o + 0xa0, want, 16))
        printf("FAIL: inset %d rect %d,%d-%d,%d\n", r, rc[0], rc[1], rc[2], rc[3]), bad = 1;
      if (*(const u64 *)(o + 0xb8) != 0)
        printf("FAIL: inset %d keeps a view state\n", r), bad = 1;
      if (*(const u32 *)(o + 0x100) != 0)
        printf("FAIL: inset %d pass %u\n", r, *(const u32 *)(o + 0x100)), bad = 1;
      float org[3];
      memcpy(org, o, 12);
      if (org[0] != (r ? 3.2f : -3.2f))
        printf("FAIL: inset %d origin %f (not its eye's)\n", r, org[0]), bad = 1;
      const float *pi = (const float *)(o + 0x50), *pe = k_proj[r];
      float x, y, ex, ey;
      /* straight ahead: the inset's centre */
      ndc(pi, 0, 0, &x, &y);
      if (x * x + y * y > 1e-8f)
        printf("FAIL: inset %d: straight ahead at %f,%f\n", r, x, y), bad = 1;
      /* the inset's edge (NDC 1) is where it should be in the eye, and the densities */
      float tx = 0.1f, ty = 0.1f;
      ndc(pi, tx, ty, &x, &y);
      ndc(pe, 0, 0, &ex, &ey);
      float ix, iy, jx, jy;
      ndc(pe, tx, ty, &jx, &jy);
      ix = (jx - ex) * 960 / 2, iy = (jy - ey) * 1080 / 2; /* periphery pixels for the tangent */
      float px = x * 544 / 2, py = y * 544 / 2;             /* inset pixels for it */
      if (fabsf(px / ix - 2.0f) > 1e-3f || fabsf(py / iy - 2.0f) > 1e-3f)
        printf("FAIL: inset %d density %f %f times the periphery's\n", r, px / ix, py / iy), bad = 1;
      else
        printf("  inset %d: %.0f inset pixels against %.0f periphery pixels for tan 0.1 across, "
               "%.0f against %.0f up\n", r, px, ix, py, iy);
    }
  }
  /* layer 1's angles: the inset's pixels a unit of tangent twice the eye's (960 wide for
   * 2 / M00 of tangent), centred on the axis (the window wasn't moved to stay inside) */
  {
    s32 lr[2][4];
    float lt[2][4];
    if (!multiview_layer(lr, lt, 0))
      printf("FAIL: no layer 1 after a frame with both insets\n"), bad = 1;
    for (int e = 0; !bad && e < 2; e++) {
      const float *p = k_proj[e];
      float eye_x = 2.0f / p[0], eye_y = 2.0f / p[5]; /* the eye's tangent across and up */
      float kx = (544.0f / (lt[e][0] + lt[e][1])) / (960.0f / eye_x);
      float ky = (544.0f / (lt[e][2] + lt[e][3])) / (1080.0f / eye_y);
      if (fabsf(kx - 2.0f) > 1e-3f || fabsf(ky - 2.0f) > 1e-3f || fabsf(lt[e][0] - lt[e][1]) > 1e-5f ||
          fabsf(lt[e][2] - lt[e][3]) > 1e-5f || lr[e][1] != 1080 || lr[e][3] != 1624)
        printf("FAIL: layer 1 eye %d: %f x %f the density, angles %f %f %f %f\n", e, kx, ky,
               lt[e][0], lt[e][1], lt[e][2], lt[e][3]), bad = 1;
      else
        printf("  layer 1 eye %d: %d,%d-%d,%d, angles %.4f %.4f %.4f %.4f (%.1fx the eye's pixels "
               "a tangent)\n", e, lr[e][0], lr[e][1], lr[e][2], lr[e][3], lt[e][0], lt[e][1],
               lt[e][2], lt[e][3], kx);
    }
  }
  /* the left eye's options refused this frame: its inset is left out (not made from the last
   * frame's), the right one's still made */
  g_opt_insetviews = 1;
  g_fail_pass = 1;
  u64 v[4];
  u32 passes[4] = {1, 2, 2, 2};
  for (int i = 0; i < 4; i++)
    v[i] = mv_cs_wrap(1, 2, 3, 4, 5, 6, passes[i]);
  g_fail_pass = 0;
  if (v[0] || !v[1] || v[2] || !v[3])
    printf("FAIL: with the left eye refused: views %d %d %d %d\n", !!v[0], !!v[1], !!v[2], !!v[3]),
        bad = 1;
  /* the engine's unconstrained rect past the inset's: set back to it, the others kept */
  g_fake_wide = 1;
  for (int i = 0; i < 4; i++)
    v[i] = mv_cs_wrap(1, 2, 3, 4, 5, 6, passes[i]);
  g_fake_wide = 0;
  {
    const s32 *r = (const s32 *)(v[2] + 0x38), want[4] = {0, 1080, 544, 1624};
    for (int k = 0; k < 3; k++)
      if (memcmp(r + 4 * k, want, 16))
        printf("FAIL: rect %d of inset 0 not the inset's\n", k), bad = 1;
    if (g_out[2] != 2 || g_out[0] || g_out[1])
      printf("FAIL: counts %u %u %u\n", g_out[0], g_out[1], g_out[2]), bad = 1;
  }
  /* a fifth view (never made by Draw) is refused */
  if (mv_cs_wrap(1, 2, 3, 4, 5, 6, 2))
    printf("FAIL: a fifth view made\n"), bad = 1;
  return bad;
}
/* the translucency volume: an inset (Views[2] or [3], pass 0) is given the left eye's bounds,
 * every other view its own (the engine's function faked to report the view it was given) */
static u64 g_tlv_seen;
static void fake_tlv(u64 v, u64 boxes, s32 n) {
  (void)boxes, (void)n;
  g_tlv_seen = v;
}
static int tlv_check(void) {
  static u8 vi[5][0x430];
  u64 views[4] = {(u64)vi[0], (u64)vi[1], (u64)vi[2], (u64)vi[3]};
  struct { u64 data; s32 n; } fam = {(u64)views, 4};
  u32 pass[5] = {1, 2, 0, 0, 0};
  for (int i = 0; i < 5; i++) {
    memset(vi[i], 0, sizeof vi[i]);
    *(u64 *)vi[i] = (u64)&fam;
    *(u32 *)(vi[i] + 0x420) = pass[i];
  }
  o_tlv = fake_tlv;
  int bad = 0;
  struct { int view; u32 views, on; s32 n; int want; const char *what; } k[] = {
      {0, 2, 1, 4, 0, "the left eye: its own"},
      {1, 2, 1, 4, 1, "the right eye: its own (the engine's caller gives it the left eye's)"},
      {2, 2, 1, 4, 0, "the left inset: the left eye's"},
      {3, 2, 1, 4, 0, "the right inset: the left eye's"},
      {4, 2, 1, 4, 4, "a view not in the family: its own"},
      {2, 0, 1, 4, 2, "insets off: its own"},
      {3, 2, 0, 4, 3, "agent.insettlv=0: its own"},
      {2, 2, 1, 2, 2, "a family of two: its own"},
  };
  for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {
    g_opt_insetviews = k[i].views, g_opt_insettlv = k[i].on, fam.n = k[i].n;
    g_tlv_seen = 0;
    mv_tlv((u64)vi[k[i].view], 0x1234, 2);
    if (g_tlv_seen != (u64)vi[k[i].want])
      printf("FAIL: the translucency volume, %s\n", k[i].what), bad = 1;
  }
  g_opt_insetviews = 0, g_opt_insettlv = 1;
  if (!bad)
    printf("  translucency volume: the insets take the left eye's, every other view its own\n");
  return bad;
}
/* the insets' eye adaptation passes left out: an inset (Views[2] or [3], pass 0), and no other */
static int eagate_check(void) {
  static u8 vi[5][0x430];
  u64 views[4] = {(u64)vi[0], (u64)vi[1], (u64)vi[2], (u64)vi[3]};
  struct { u64 data; s32 n; } fam = {(u64)views, 4};
  u32 pass[5] = {1, 2, 0, 0, 0};
  for (int i = 0; i < 5; i++) {
    memset(vi[i], 0, sizeof vi[i]);
    *(u64 *)vi[i] = (u64)&fam;
    *(u32 *)(vi[i] + 0x420) = pass[i];
  }
  int bad = 0;
  struct { int view; u32 views, on; s32 n; u32 want; const char *what; } k[] = {
      {0, 2, 1, 4, 0, "the left eye: its passes"},
      {1, 2, 1, 4, 0, "the right eye: the gate's own (cmp 2) test, not the agent's"},
      {2, 2, 1, 4, 1, "the left inset: left out"},
      {3, 2, 1, 4, 1, "the right inset: left out"},
      {4, 2, 1, 4, 0, "a pass-0 view not in the family (a flat screen): its passes"},
      {2, 0, 1, 4, 0, "insets off: its passes"},
      {3, 2, 0, 4, 0, "agent.insetea=0: its passes"},
      {2, 2, 1, 1, 0, "a family of one (no stereo): its passes"},
  };
  for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {
    g_opt_insetviews = k[i].views, g_opt_insetea = k[i].on, fam.n = k[i].n;
    if (mv_ea_skip((u64)vi[k[i].view]) != k[i].want)
      printf("FAIL: the eye adaptation gate, %s\n", k[i].what), bad = 1;
  }
  g_opt_insetviews = 0, g_opt_insetea = 1;
  if (!bad)
    printf("  eye adaptation: the insets' passes left out, every other view's kept\n");
  return bad;
}
/* the exposure getters: an inset (Views[2] or [3], pass 0, no state) reads the left eye's, every
 * other view its own; the engine's two functions faked to report the view they were given */
static u64 g_ea_seen;
static u64 fake_ea_get(u64 v, u64 cmd) {
  (void)cmd;
  g_ea_seen = v;
  return 0x77;
}
static u64 fake_effective(u64 v) {
  g_ea_seen = v;
  return *(u64 *)(v + 0xd10);
}
/* mv_lod: an inset's LOD asked with its own eye's view (the left inset the left eye's, the right
 * the right's), every other view its own; mv_occ_skip: no occlusion culling for an inset only */
static u64 g_lod_seen;
static u32 fake_lod(u64 meshes, u64 view, u64 origin, s32 forced, float radius, float scale) {
  g_lod_seen = view;
  return meshes == 1 && origin == 3 && forced == -1 && radius == 5.0f && scale == 0.5f ? 0x0102 : 0;
}
static int lod_occ_check(void) {
  static u8 vi[5][0x430];
  u64 views[4] = {(u64)vi[0], (u64)vi[1], (u64)vi[2], (u64)vi[3]};
  struct { u64 data; s32 n; } fam = {(u64)views, 4};
  u32 pass[5] = {1, 2, 0, 0, 0};
  for (int i = 0; i < 5; i++) {
    memset(vi[i], 0, sizeof vi[i]);
    *(u64 *)vi[i] = (u64)&fam;
    *(u32 *)(vi[i] + 0x420) = pass[i];
  }
  o_lod = fake_lod;
  int bad = 0;
  struct { int view; u32 views, lod, occ; int want, skip; const char *what; } k[] = {
      {0, 2, 1, 1, 0, 0, "the left eye: its own"},
      {1, 2, 1, 1, 1, 0, "the right eye: its own"},
      {2, 2, 1, 1, 0, 1, "the left inset: the left eye's"},
      {3, 2, 1, 1, 1, 1, "the right inset: the right eye's"},
      {4, 2, 1, 1, 4, 0, "a view not in the family: its own"},
      {2, 0, 1, 1, 2, 0, "insets off: its own"},
      {3, 2, 0, 0, 3, 0, "agent.insetlod=0, agent.insetocc=0: its own, culled"},
  };
  for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {
    g_opt_insetviews = k[i].views, g_opt_insetlod = k[i].lod, g_opt_insetocc = k[i].occ;
    g_lod_seen = 0;
    u32 r = mv_lod(1, (u64)vi[k[i].view], 3, -1, 5.0f, 0.5f);
    if (g_lod_seen != (u64)vi[k[i].want] || r != 0x0102)
      printf("FAIL: ComputeLODForMeshes, %s\n", k[i].what), bad = 1;
    if (mv_occ_skip((u64)vi[k[i].view]) != (u32)k[i].skip)
      printf("FAIL: DoOcclusionQueries, %s\n", k[i].what), bad = 1;
  }
  /* a family of 3 (no right inset): Views[2] still the left eye's */
  fam.n = 3;
  g_opt_insetviews = 2, g_opt_insetlod = 1, g_opt_insetocc = 1;
  mv_lod(1, (u64)vi[2], 3, -1, 5.0f, 0.5f);
  if (g_lod_seen != (u64)vi[0])
    printf("FAIL: ComputeLODForMeshes, a family of 3\n"), bad = 1;
  g_opt_insetviews = 0, g_opt_insetlod = 1, g_opt_insetocc = 1;
  return bad;
}

static int ea_check(void) {
  static u8 vi[5][0xd20], state[0x700];
  u64 views[4] = {(u64)vi[0], (u64)vi[1], (u64)vi[2], (u64)vi[3]};
  struct { u64 data; s32 n; } fam = {(u64)views, 4};
  u32 pass[5] = {1, 2, 0, 0, 0};
  for (int i = 0; i < 5; i++) {
    memset(vi[i], 0, sizeof vi[i]);
    *(u64 *)vi[i] = (u64)&fam;
    *(u32 *)(vi[i] + 0x420) = pass[i];
  }
  *(u64 *)(vi[0] + 0xd10) = (u64)state;
  *(u64 *)(vi[1] + 0xd10) = (u64)state + 8;
  state[0x628] = 1;
  o_ea_get = fake_ea_get;
  o_ea_effective = fake_effective;
  int bad = 0;
  struct { int view; u32 views, eye, own; int want; const char *what; } k[] = {
      {0, 1, 1, 0, 0, "the left eye: its own"},
      {1, 1, 1, 0, 1, "the right eye: its own (the engine's rule)"},
      {2, 1, 1, 0, 0, "the left inset: the left eye's"},
      {3, 1, 1, 0, 0, "the right inset: the left eye's"},
      {4, 1, 1, 0, 4, "a view not in the family: its own"},
      {2, 0, 1, 0, 2, "insets off: its own"},
      {3, 1, 0, 0, 3, "agent.inseteye=0: its own"},
      {2, 1, 1, 1, 0, "an inset with a state of its own: the left eye's"},
  };
  for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {
    g_opt_insetviews = k[i].views, g_opt_inseteye = k[i].eye;
    *(u64 *)(vi[2] + 0xd10) = k[i].own ? (u64)state + 16 : 0;
    u64 want = (u64)vi[k[i].want];
    g_ea_seen = 0;
    u64 r = mv_ea_get((u64)vi[k[i].view], 5);
    if (g_ea_seen != want || r != 0x77)
      printf("FAIL: GetEyeAdaptation, %s\n", k[i].what), bad = 1;
    g_ea_seen = 0;
    u32 valid = mv_ea_valid((u64)vi[k[i].view]);
    u32 want_valid = *(u64 *)(vi[k[i].want] + 0xd10) == (u64)state;
    if (g_ea_seen != want || valid != want_valid)
      printf("FAIL: HasValidEyeAdaptation, %s (%u)\n", k[i].what, valid), bad = 1;
  }
  /* the histogram eye adaptation pass gets its target (its own) through the same getter */
  g_opt_insetviews = 1, g_opt_inseteye = 1;
  *(u64 *)(vi[2] + 0xd10) = (u64)state + 16;
  g_ea_seen = 0;
  ea_get_from((u64)vi[2], 5, astro_hdr.game_base + EA_WRITER_RET);
  if (g_ea_seen != (u64)vi[2])
    printf("FAIL: the histogram pass's target for an inset not its own\n"), bad = 1;
  g_ea_seen = 0;
  ea_get_from((u64)vi[2], 5, astro_hdr.game_base + EA_WRITER_RET + 1);
  if (g_ea_seen != (u64)vi[0])
    printf("FAIL: a reader after the histogram pass's call not given the left eye's\n"), bad = 1;
  /* a family of two (insets off in Draw): no inset */
  fam.n = 2;
  g_opt_insetviews = 1, g_opt_inseteye = 1;
  *(u64 *)(vi[2] + 0xd10) = 0;
  g_ea_seen = 0;
  mv_ea_get((u64)vi[2], 5);
  if (g_ea_seen != (u64)vi[2])
    printf("FAIL: a family of two views redirected\n"), bad = 1;
  g_opt_insetviews = 0;
  if (!bad)
    printf("  exposure: the insets read the left eye's, every other view its own\n");
  return bad;
}
/* the insets' origins: UpdateViewMatrix sees an inset as its eye's pass, every other view as is */
static u32 g_uvm_pass_seen;
static void fake_uvm(u64 v) { g_uvm_pass_seen = *(u32 *)(v + 0x420); }
static int uvm_check(void) {
  static u8 vi[5][0xd20];
  u64 views[4] = {(u64)vi[0], (u64)vi[1], (u64)vi[2], (u64)vi[3]};
  struct { u64 data; s32 n; } fam = {(u64)views, 4};
  u32 pass[5] = {1, 2, 0, 0, 0};
  for (int i = 0; i < 5; i++) {
    memset(vi[i], 0, sizeof vi[i]);
    *(u64 *)vi[i] = (u64)&fam;
    *(u32 *)(vi[i] + 0x420) = pass[i];
  }
  o_uvm = fake_uvm;
  int bad = 0;
  struct { int view; u32 views, pos, want; const char *what; } k[] = {
      {0, 1, 1, 1, "the left eye: pass 1"}, {1, 1, 1, 2, "the right eye: pass 2"},
      {2, 1, 1, 1, "the left inset: its eye's pass 1"}, {3, 1, 1, 2, "the right inset: pass 2"},
      {4, 1, 1, 0, "a view not in the family: 0"}, {2, 0, 1, 0, "insets off: 0"},
      {3, 1, 0, 0, "agent.insetpos=0: 0"}};
  for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {
    g_opt_insetviews = k[i].views, g_opt_insetpos = k[i].pos;
    g_uvm_pass_seen = 99;
    mv_uvm((u64)vi[k[i].view]);
    if (g_uvm_pass_seen != k[i].want || *(u32 *)(vi[k[i].view] + 0x420) != pass[k[i].view])
      printf("FAIL: UpdateViewMatrix, %s (saw %u)\n", k[i].what, g_uvm_pass_seen), bad = 1;
  }
  g_opt_insetviews = 0, g_opt_insetpos = 1;
  if (!bad)
    printf("  origins: UpdateViewMatrix sees each inset as its eye, the pass put back after\n");
  return bad;
}
/* the garbage collector's references: the engine's function, then each inset state made so far
 * (its AddReferencedObjects, slot 2, with the same collector) */
static u64 g_aro_lp, g_aro_coll, g_aro_states[4];
static int g_aro_n, g_aro_order;
static void fake_lp_aro(u64 lp, u64 coll) {
  g_aro_lp = lp, g_aro_coll = coll;
  g_aro_order = g_aro_n; /* states reported before it ran: 0, the engine's goes first */
}
static void fake_vs_aro(u64 st, u64 coll) {
  if (coll == g_aro_coll && g_aro_n < 4)
    g_aro_states[g_aro_n++] = st;
}
static int aro_check(void) {
  static u64 vt[8], st[2][0x700 / 8];
  vt[VS_ARO_SLOT] = (u64)fake_vs_aro;
  for (int i = 0; i < 2; i++)
    st[i][0] = (u64)vt, ((s32 *)st[i])[VS_MIDPOOL_N / 4] = 3 + i;
  o_lp_aro = fake_lp_aro;
  int bad = 0;
  for (int made = 0; made < 3; made++) {
    g_vsr_made[0] = made >= 1, g_vsr_made[1] = made >= 2;
    g_vsr[0][1] = (u64)st[0], g_vsr[1][1] = (u64)st[1];
    g_aro_n = 0, g_aro_order = -1, g_aro_lp = 0;
    g_aro_coll = 0x5150; /* the collector the states must be given */
    mv_lp_aro(0x1234, 0x5150);
    if (g_aro_lp != 0x1234 || g_aro_order != 0 || g_aro_n != made ||
        (made >= 1 && g_aro_states[0] != (u64)st[0]) || (made >= 2 && g_aro_states[1] != (u64)st[1]))
      printf("FAIL: the garbage collector's references with %d inset states (%d reported)\n", made,
             g_aro_n), bad = 1;
  }
  g_vsr_made[0] = g_vsr_made[1] = 0;
  g_vsr[0][1] = g_vsr[1][1] = 0;
  if (!bad)
    printf("  view states: the local player's references, then each inset state's\n");
  return bad;
}
/* the window following the gaze, and the insets side by side below the eyes (more rows than an
 * inset's height too: the stacked layout is gone, inset mode always has Hmd2's window test) */
/* agent.insetsnap: the window moved by whole inset pixels (the TAA's history then taken texel for
 * texel), within half a pixel of where the gaze puts it */
static int snap_check(void) {
  int bad = 0;
  u32 rows_was = g_multiview_rows;
  g_multiview_rows = 600, g_opt_insetsize = 600, g_opt_insetwidth = 1200, g_opt_insetppt = 1200, g_opt_insetviews = 1;
  g_opt_insetgaze = 1, g_fake_gaze_ok = 1, g_opt_gazefilter = 0, g_opt_gazehold = 0;
  float c0[2][2] = {{0, 0}, {0, 0}}, worst = 0, frac = 0, forced = 0;
  for (int k = 0; k < 40; k++) {
    float g[2] = {0.1f + 0.00137f * (float)k, -0.05f + 0.00091f * (float)(k * k % 17)};
    float c[3][2][2];
    for (int snap = 0; snap < 3; snap++) { /* (2: agent.insetsnap=0 with 11 running, snapped all the same) */
      g_opt_insetsnap = snap == 1 ? 2u : 0u, g_opt_taa = snap == 2 ? 11u : 0u;
      g_taa.ok = snap == 2, g_taa.failed = 0, g_taa.cur = g_opt_taa; /* (the variant running decides: taa_active) */
      g_fake_gaze[0] = g[0], g_fake_gaze[1] = g[1], g_nview = 0;
      mv_cs_wrap(1, 2, 3, 4, 5, 6, 1);
      mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
      mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
      mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
      for (int r = 0; r < 2; r++)
        c[snap][r][0] = g_mv_inset[r].ctan[0], c[snap][r][1] = g_mv_inset[r].ctan[1];
    }
    for (int r = 0; r < 2; r++)
      for (int a = 0; a < 2; a++) {
        float fo = fabsf(c[2][r][a] - c[1][r][a]);
        forced = fo > forced ? fo : forced;
        float off = fabsf(c[1][r][a] - c[0][r][a]) * 1200.0f; /* (inset pixels: 1200 a tangent) */
        worst = off > worst ? off : worst;
        if (k) {
          float d = (c[1][r][a] - c0[r][a]) * 600.0f, f = fabsf(d - roundf(d)); /* (in 2-pixel steps) */
          frac = f > frac ? f : frac;
        }
        c0[r][a] = k ? c0[r][a] : c[1][r][a];
      }
  }
  g_opt_taa = 0, g_taa.ok = 0, g_taa.cur = 0;
  /* agent.taa=11 asked but ours refused (not running): the window not forced to 2-pixel steps */
  g_opt_taa = 11, g_taa.ok = 0, g_taa.failed = 1, g_taa.cur = 0;
  if (taa_velfix(taa_active()))
    printf("FAIL: snapping: forced with our TAA refused\n"), bad = 1;
  g_opt_taa = 0, g_taa.failed = 0;
  if (forced != 0.0f)
    printf("FAIL: snapping: agent.taa=11 with agent.insetsnap=0 not in 2-pixel steps (%.6f of a tangent off)\n", forced), bad = 1;
  if (worst > 1.0f + 1e-3f || frac > 2e-3f)
    printf("FAIL: snapping: %.4f of a pixel from the gaze's place, moves %.4f of a pixel off whole\n", worst, frac), bad = 1;
  else
    printf("  snapping: the window within %.3f of a pixel of the gaze's place, every move whole 2-pixel steps (worst %.4f off), the same with agent.taa=11 and insetsnap=0\n",
           worst, frac);
  g_opt_insetsnap = 0, g_opt_insetgaze = 0, g_fake_gaze_ok = 0, g_opt_gazefilter = 1, g_opt_gazehold = 50;
  g_opt_insetwidth = 0, g_multiview_rows = rows_was;
  return bad;
}
static int gaze_layout_check(void) {
  int bad = 0;
  u32 rows_was = g_multiview_rows;
  g_multiview_rows = 960, g_opt_insetsize = 480, g_opt_insetppt = 1200, g_opt_insetviews = 1;
  g_opt_insetgaze = 1, g_fake_gaze_ok = 1, g_fake_gaze[0] = 0.2f, g_fake_gaze[1] = -0.1f;
  g_nview = 0;
  mv_cs_wrap(1, 2, 3, 4, 5, 6, 1);
  mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
  mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
  mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
  for (int r = 0; r < 2; r++) {
    const float *pi = (const float *)(g_last_opts[2 + r] + 0x50);
    float x, y;
    ndc(pi, 0.2f, -0.1f, &x, &y); /* the gaze's direction: the inset's centre */
    if (fabsf(x) > 1e-4f || fabsf(y) > 1e-4f)
      printf("FAIL: inset %d: the gaze lands at %f,%f, not its centre\n", r, x, y), bad = 1;
    struct mv_inset *mi = &g_mv_inset[r];
    if (fabsf(mi->ctan[0] - 0.2f) > 1e-4f || fabsf(mi->ctan[1] + 0.1f) > 1e-4f)
      printf("FAIL: inset %d: centre %f,%f in tangent\n", r, mi->ctan[0], mi->ctan[1]), bad = 1;
  }
  const s32 *l = g_mv_inset[0].rect, *rr = g_mv_inset[1].rect;
  /* the left eye's at its eye's left edge, the right eye's at its right edge, both right below
   * the eyes */
  if (l[0] != 0 || l[1] != 1080 || l[2] != 480 || l[3] != 1560 || rr[0] != 1440 || rr[1] != 1080 ||
      rr[2] != 1920 || rr[3] != 1560)
    printf("FAIL: the layout: left %d,%d-%d,%d, right %d,%d-%d,%d\n", l[0], l[1], l[2], l[3], rr[0],
           rr[1], rr[2], rr[3]), bad = 1;
  else
    printf("  gaze 0.2,-0.1: both insets centred on it; side by side: left %d,%d-%d,%d, right %d,%d-%d,%d\n",
           l[0], l[1], l[2], l[3], rr[0], rr[1], rr[2], rr[3]);
  /* agent.insetwidth: 960 by 600 in 1200 rows, the same density across and up */
  g_multiview_rows = 1200, g_opt_insetsize = 600, g_opt_insetwidth = 960, g_nview = 0;
  mv_cs_wrap(1, 2, 3, 4, 5, 6, 1);
  mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
  mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
  mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
  for (int r = 0; r < 2; r++) {
    const float *pi = (const float *)(g_last_opts[2 + r] + 0x50);
    float x, y;
    ndc(pi, 0.2f, -0.1f, &x, &y);
    struct mv_inset *mi = &g_mv_inset[r];
    if (fabsf(x) > 1e-4f || fabsf(y) > 1e-4f || fabsf(mi->ppt[2] - 1200.0f) > 1.0f ||
        fabsf(mi->ppt[3] - 1200.0f) > 1.0f)
      printf("FAIL: wide inset %d: the gaze at %f,%f, %f and %f pixels a tangent\n", r, x, y,
             mi->ppt[2], mi->ppt[3]), bad = 1;
  }
  if (rr[0] != 960 || rr[1] != 1080 || rr[2] != 1920 || rr[3] != 1680 || l[0] != 0 || l[1] != 1080 ||
      l[2] != 960 || l[3] != 1680)
    printf("FAIL: wide insets: left %d,%d-%d,%d, right %d,%d-%d,%d\n", l[0], l[1], l[2], l[3], rr[0],
           rr[1], rr[2], rr[3]), bad = 1;
  else
    printf("  wide insets 960x600: right %d,%d-%d,%d, left %d,%d-%d,%d, 1200 a tangent both ways (+-%.3f by +-%.3f of tangent)\n",
           rr[0], rr[1], rr[2], rr[3], l[0], l[1], l[2], l[3],
           (g_mv_inset[0].tan[0] + g_mv_inset[0].tan[1]) / 2, (g_mv_inset[0].tan[2] + g_mv_inset[0].tan[3]) / 2);
  g_opt_insetwidth = 0;
  /* at a screen percentage of 125 (resolution.txt 1200): the eyes 1200x1350 in the texture
   * (their options 960x1080), 1200x600 insets side by side below them in 600 rows */
  g_mv_k = 1.25f, g_multiview_rows = 600, g_opt_insetsize = 600, g_opt_insetwidth = 1200, g_nview = 0;
  u64 vv[4];
  vv[0] = mv_cs_wrap(1, 2, 3, 4, 5, 6, 1);
  vv[1] = mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
  vv[2] = mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
  vv[3] = mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
  {
    const s32 *ri0 = (const s32 *)(vv[2] + 0x38), *ri1 = (const s32 *)(vv[3] + 0x38);
    static const s32 w0[4] = {0, 1350, 1200, 1950}, w1[4] = {1200, 1350, 2400, 1950};
    static const s32 u0[4] = {0, 1080, 960, 1560}, u1[4] = {960, 1080, 1920, 1560};
    int ok = !memcmp(ri0, w0, 16) && !memcmp(ri0 + 4, u0, 16) && !memcmp(ri0 + 8, u0, 16) &&
             !memcmp(ri1, w1, 16) && !memcmp(ri1 + 4, u1, 16) && !memcmp(ri1 + 8, u1, 16);
    for (int r = 0; r < 2; r++) {
      const float *pi = (const float *)(g_last_opts[2 + r] + 0x50);
      float x, y;
      ndc(pi, 0.2f, -0.1f, &x, &y);
      struct mv_inset *mi = &g_mv_inset[r];
      ok &= fabsf(x) < 1e-4f && fabsf(y) < 1e-4f && fabsf(mi->ppt[2] - 1200.0f) < 1.0f &&
            fabsf(mi->ppt[3] - 1200.0f) < 1.0f;
    }
    if (!ok)
      printf("FAIL: at 125%%: inset 0 %d,%d-%d,%d (unscaled %d,%d-%d,%d), inset 1 %d,%d-%d,%d, ppt %f %f\n",
             ri0[0], ri0[1], ri0[2], ri0[3], ri0[4], ri0[5], ri0[6], ri0[7], ri1[0], ri1[1], ri1[2],
             ri1[3], g_mv_inset[0].ppt[2], g_mv_inset[0].ppt[3]), bad = 1;
    else
      printf("  at 125%%: 1200x600 insets side by side at 0,1350 and 1200,1350 in the texture, unscaled 960x480 at 1080, gaze centred, 1200 a tangent\n");
  }
  g_mv_k = 1.0f, g_opt_insetwidth = 0;
  g_multiview_rows = rows_was, g_opt_insetsize = 0, g_opt_insetppt = 0, g_opt_insetgaze = 0;
  g_fake_gaze_ok = 0, g_opt_insetviews = 0;
  return bad;
}
/* the gaze held: samples scattered around a look keep the window still; a saccade moves it at
 * once; a small move past the hold radius after 4 frames */
static int gaze_hold_check(void) {
  int bad = 0;
  float t[2];
  u32 gen_was = g_gen;
  g_opt_insetgaze = 1, g_opt_gazehold = 50, g_fake_gaze_ok = 1, g_opt_gazefilter = 0;
  srand(7);
  g_fake_gaze[0] = 0.1f, g_fake_gaze[1] = 0.05f;
  g_gen++, inset_gaze(t); /* a jump from straight ahead: there at once */
  if (fabsf(t[0] - 0.1f) > 1e-5f || fabsf(t[1] - 0.05f) > 1e-5f)
    printf("FAIL: gaze hold: the first look not taken (%f,%f)\n", t[0], t[1]), bad = 1;
  float c0 = t[0], c1 = t[1], moved = 0;
  for (int i = 0; i < 300; i++) { /* noise of +-0.025 (about 1.4 degrees) */
    g_fake_gaze[0] = 0.1f + ((float)(rand() % 1001) / 1000.0f - 0.5f) * 0.05f;
    g_fake_gaze[1] = 0.05f + ((float)(rand() % 1001) / 1000.0f - 0.5f) * 0.05f;
    g_gen++, inset_gaze(t);
    float d = fabsf(t[0] - c0) + fabsf(t[1] - c1);
    moved = d > moved ? d : moved;
  }
  if (moved > 0.0f)
    printf("FAIL: gaze hold: the window moved %f with fixation noise\n", moved), bad = 1;
  g_fake_gaze[0] = -0.2f, g_fake_gaze[1] = 0.05f; /* a saccade of 0.3 */
  g_gen++, inset_gaze(t);
  if (fabsf(t[0] + 0.2f) > 1e-5f)
    printf("FAIL: gaze hold: the saccade not followed at once (%f)\n", t[0]), bad = 1;
  g_fake_gaze[0] = -0.13f; /* a small move of 0.07: past 0.05, under 0.1 */
  int frames = 0;
  for (; frames < 30 && t[0] < -0.19f; frames++)
    g_gen++, inset_gaze(t);
  if (frames < 4 || frames > 12 || fabsf(t[0] + 0.13f) > 0.03f)
    printf("FAIL: gaze hold: a small move followed after %d frames to %f\n", frames, t[0]), bad = 1;
  g_opt_gazehold = 0; /* off: every sample */
  g_fake_gaze[0] = -0.125f;
  g_gen++, inset_gaze(t);
  if (fabsf(t[0] + 0.125f) > 1e-5f)
    printf("FAIL: gaze hold off: the sample not followed (%f)\n", t[0]), bad = 1;
  if (!bad)
    printf("  gaze hold: still through 300 frames of +-0.025 noise, a saccade at once, 0.07 after %d frames\n", frames);
  g_opt_insetgaze = 0, g_opt_gazehold = 50, g_fake_gaze_ok = 0, g_gen = gen_was, g_opt_gazefilter = 1;
  return bad;
}
/* the gaze smoothed by its speed (agent.gazefilter=1): the tracker's scatter at rest barely
 * moves the window; a look held while the head tilts (the gaze drifting 0.35 of tangent a
 * second) is followed smoothly, without steps; a saccade at once */
static int gaze_smooth_check(void) {
  int bad = 0;
  float t[2];
  u32 gen_was = g_gen;
  g_opt_insetgaze = 1, g_opt_gazehold = 50, g_opt_gazefilter = 1, g_fake_gaze_ok = 1;
  g_opt_gazemincut = 10, g_opt_gazebeta = 1;
  srand(11);
  g_fake_gaze[0] = 0.1f, g_fake_gaze[1] = 0.0f;
  g_fake_us += 16667, g_gen++, inset_gaze(t);
  float lo = 1, hi = -1;
  for (int i = 0; i < 600; i++) { /* noise of +-0.02 */
    g_fake_gaze[0] = 0.1f + ((float)(rand() % 1001) / 1000.0f - 0.5f) * 0.04f;
    g_fake_gaze[1] = ((float)(rand() % 1001) / 1000.0f - 0.5f) * 0.04f;
    g_fake_us += 16667, g_gen++, inset_gaze(t);
    if (i > 60)
      lo = t[0] < lo ? t[0] : lo, hi = t[0] > hi ? t[0] : hi;
  }
  if (hi - lo > 0.012f)
    printf("FAIL: gaze smooth: the window wandered %f with +-0.02 noise at rest\n", hi - lo), bad = 1;
  float x = 0.1f, step = 0.0f, lag = 0.0f;
  for (int i = 0; i < 90; i++) { /* 1.5 s drifting at 0.35 a second, with the noise */
    x += 0.35f / 60.0f;
    g_fake_gaze[0] = x + ((float)(rand() % 1001) / 1000.0f - 0.5f) * 0.04f;
    float was = t[0];
    g_fake_us += 16667, g_gen++, inset_gaze(t);
    if (i > 30) {
      step = t[0] - was > step ? t[0] - was : step;
      lag = x - t[0] > lag ? x - t[0] : lag;
    }
  }
  if (step > 0.02f || lag > 0.06f)
    printf("FAIL: gaze smooth: a drift followed with steps up to %f, lag up to %f\n", step, lag), bad = 1;
  g_fake_gaze[0] = -0.3f; /* a saccade */
  g_fake_us += 16667, g_gen++, inset_gaze(t);
  if (fabsf(t[0] + 0.3f) > 1e-4f)
    printf("FAIL: gaze smooth: a saccade not taken at once (%f)\n", t[0]), bad = 1;
  g_opt_gazehold = 0; /* no jumps taken at once: a jump smoothed, not followed in one frame */
  g_fake_gaze[0] = 0.2f;
  g_fake_us += 16667, g_gen++, inset_gaze(t);
  if (t[0] > 0.0f)
    printf("FAIL: gaze smooth with the hold at 0: the window jumped to %f\n", t[0]), bad = 1;
  g_opt_gazehold = 50;
  if (!bad)
    printf("  gaze smooth: %.4f wander at rest (+-0.02 noise), a 0.35/s drift in steps of at most %.4f (%.4f a frame moving) with lag %.3f, a saccade at once\n",
           hi - lo, step, 0.35f / 60.0f, lag);
  g_opt_insetgaze = 0, g_fake_gaze_ok = 0, g_gen = gen_was;
  return bad;
}
/* each frame's insets found again by the head pose its views were made with: the frame handed
 * over (made with pose 1000) gets its own window, not the newer frame's (pose 2000) */
static int frame_pose_check(void) {
  int bad = 0;
  u32 rows_was = g_multiview_rows;
  g_multiview_rows = 960, g_opt_insetsize = 480, g_opt_insetppt = 1200, g_opt_insetviews = 1;
  g_opt_insetgaze = 1, g_opt_gazehold = 0, g_fake_gaze_ok = 1, g_opt_gazefilter = 0; /* (held at 0: every sample) */
  float want[2][2] = {{-0.2f, 0.1f}, {0.25f, -0.05f}};
  for (int fr = 0; fr < 2; fr++) {
    multiview_note_pose(1000u * (u64)(fr + 1));
    g_fake_gaze[0] = want[fr][0], g_fake_gaze[1] = want[fr][1];
    g_nview = 0;
    mv_cs_wrap(1, 2, 3, 4, 5, 6, 1);
    mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
    mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
    mv_cs_wrap(1, 2, 3, 4, 5, 6, 2);
    multiview_note_pose(1000u * (u64)(fr + 1) + 500u); /* the render thread's late pose */
  }
  /* (views: the right eye in g_view[1], inset 0 in [2], inset 1 in [0]) */
  if (*(float *)(g_view[1] + 0xa48) != 0.0f || *(float *)(g_view[2] + 0xa48) != 0.0f ||
      *(float *)(g_view[0] + 0xa48) != 0.0f)
    printf("FAIL: the vignette (agent.vignette=0): eye %f, insets %f %f\n", *(float *)(g_view[1] + 0xa48),
           *(float *)(g_view[2] + 0xa48), *(float *)(g_view[0] + 0xa48)), bad = 1;
  else
    printf("  the vignette off in every view (agent.vignette=0)\n");
  for (int fr = 0; fr < 2; fr++) { /* the hand-over carries the late pose */
    float c[2][2];
    multiview_centres(c, 1000u * (u64)(fr + 1) + 500u);
    for (int e = 0; e < 2; e++)
      if (fabsf(c[e][0] - want[fr][0]) > 1e-4f || fabsf(c[e][1] - want[fr][1]) > 1e-4f)
        printf("FAIL: the frame's late pose %u: eye %d's window %f,%f, not %f,%f\n",
               1000 * (fr + 1) + 500, e, c[e][0], c[e][1], want[fr][0], want[fr][1]), bad = 1;
  }
  for (int fr = 0; fr < 2; fr++) {
    float c[2][2];
    multiview_centres(c, 1000u * (u64)(fr + 1));
    s32 lr[2][4];
    float lt[2][4];
    int ok = multiview_layer(lr, lt, 1000u * (u64)(fr + 1));
    for (int e = 0; e < 2; e++)
      if (!ok || fabsf(c[e][0] - want[fr][0]) > 1e-4f || fabsf(c[e][1] - want[fr][1]) > 1e-4f)
        printf("FAIL: the frame made with pose %u: eye %d's window %f,%f, not %f,%f\n",
               1000 * (fr + 1), e, c[e][0], c[e][1], want[fr][0], want[fr][1]), bad = 1;
  }
  float c[2][2];
  multiview_centres(c, 0);
  if (fabsf(c[0][0] - want[1][0]) > 1e-4f)
    printf("FAIL: the newest window %f, not %f\n", c[0][0], want[1][0]), bad = 1;
  u32 missed = g_hist_missed;
  multiview_centres(c, 777); /* a pose no frame was made with: the newest */
  if (g_hist_missed != missed + 1 || fabsf(c[0][0] - want[1][0]) > 1e-4f)
    printf("FAIL: an unknown pose: %f (missed %u)\n", c[0][0], g_hist_missed - missed), bad = 1;
  if (!bad)
    printf("  frames by pose: the older frame's window found by its own pose and by its late (render thread) pose with a newer one made; an unknown pose gives the newest\n");
  g_multiview_rows = rows_was, g_opt_insetsize = 0, g_opt_insetppt = 0, g_opt_insetgaze = 0;
  g_opt_gazehold = 50, g_fake_gaze_ok = 0, g_opt_insetviews = 0, g_opt_gazefilter = 1;
  return bad;
}
/* Chisato's, Allison's and Hikari 1.02's eboots (argv[3], argv[4], argv[5]): the boot as that build gives as many entries,
 * each on that game's stock bytes; the calls land on the pads; the hooks' rests are where the
 * prologues the agent's copies run end, the same bytes as Hikari's; every address the code names
 * is in the table (the boot looks each up) */
static unsigned char *load_file(const char *path, long *n) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  fseek(f, 0, SEEK_END);
  *n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *d = malloc(*n);
  fread(d, 1, *n, f);
  fclose(f);
  return d;
}
static int other_builds(int argc, char **argv, unsigned hikari_n) {
  int bad = 0;
  static const char *k_name[5] = {0, 0, "Chisato", "Allison", "Hikari 1.02"};
  struct { u64 rest; u32 len; } rests[8];
  for (u32 b = 2; b <= BUILD_N; b++) {
    if (argc <= (int)b + 1) {
      printf("(no %s eboot given: not checked)\n", k_name[b]);
      continue;
    }
    long n;
    unsigned char *e = load_file(argv[b + 1], &n);
    if (!e) {
      printf("FAIL: %s's eboot\n", k_name[b]), bad = 1;
      continue;
    }
    /* Hikari's prologues the copies run (from its boot's rests) */
    astro_hdr.build = 1;
    multiview_boot(544);
    u64 base = astro_hdr.game_base;
    rests[0].rest = g_uvm_rest, rests[0].len = 18, rests[1].rest = g_lp_aro_rest, rests[1].len = 20;
    rests[2].rest = g_tlv_rest, rests[2].len = 20, rests[3].rest = g_tm_rest, rests[3].len = 20;
    rests[4].rest = g_tmf_rest, rests[4].len = 20, rests[5].rest = g_ao_rest, rests[5].len = 17;
    rests[6].rest = g_lod_rest, rests[6].len = 17;
    rests[7].rest = g_ssr_rest, rests[7].len = 20;
    unsigned char want[8][20];
    long hn;
    unsigned char *h = load_file(argv[1], &hn);
    for (int i = 0; i < 8; i++)
      memcpy(want[i], h + (rests[i].rest - base) - rests[i].len + 0x4000, rests[i].len);
    free(h);
    astro_hdr.build = b;
    struct a_patches *t = (struct a_patches *)multiview_boot(544);
    if (!t || t->n != hikari_n || g_at_missing) {
      printf("FAIL: %s: the boot gave %u entries (Hikari %u), missing 0x%x\n", k_name[b], t ? t->n : 0,
             hikari_n, g_at_missing), bad = 1;
      free(e);
      continue;
    }
    for (unsigned i = 0; i < t->n; i++)
      if (t->p[i].at + t->p[i].len + 0x4000 > (u64)n ||
          memcmp(e + t->p[i].at + 0x4000, t->p[i].was, t->p[i].len) != 0)
        printf("FAIL: %s: entry %u at 0x%x: not the stock bytes\n", k_name[b], i, t->p[i].at), bad = 1;
    rests[0].rest = g_uvm_rest, rests[1].rest = g_lp_aro_rest, rests[2].rest = g_tlv_rest;
    rests[3].rest = g_tm_rest, rests[4].rest = g_tmf_rest, rests[5].rest = g_ao_rest;
    rests[6].rest = g_lod_rest, rests[7].rest = g_ssr_rest;
    for (int i = 0; i < 8; i++)
      if (memcmp(e + (rests[i].rest - base) - rests[i].len + 0x4000, want[i], rests[i].len) != 0)
        printf("FAIL: %s: hook %d's first %u bytes before its rest\n", k_name[b], i, rests[i].len), bad = 1;
    /* the calls land on the pads (written into a copy) */
    for (unsigned i = 0; i < t->n; i++)
      memcpy(e + t->p[i].at + 0x4000, t->p[i].now, t->p[i].len);
    u32 calls[4][2] = {{DRAW_COUNT, PAD_COUNT}, {DRAW_CSV, PAD_CSV}, {CSV_OPTS, PAD_OPTS}, {OCC_CALL, PAD_OCC}};
    for (int i = 0; i < 4; i++) {
      int32_t rel;
      memcpy(&rel, e + calls[i][0] + 1 + 0x4000, 4);
      if (e[calls[i][0] + 0x4000] != 0xe8 || calls[i][0] + 5 + rel != calls[i][1] ||
          e[calls[i][1] + 0x4000] != 0xff || e[calls[i][1] + 1 + 0x4000] != 0x25)
        printf("FAIL: %s: call %d\n", k_name[b], i), bad = 1;
    }
    if (o_csv != (void *)(base + CSV_FN) || o_opts != (void *)(base + OPTS_FN))
      printf("FAIL: %s: the originals\n", k_name[b]), bad = 1;
    printf("%s: %u entries on its stock bytes, the calls on the pads, the hooks' prologues Hikari's%s\n",
           k_name[b], t->n, bad ? " (FAIL above)" : "");
    free(e);
  }
  astro_hdr.build = 1;
  multiview_boot(544);
  return bad;
}

/* The native rates from the agent's table (docs/MULTIVIEW.md 28): the payload's want_fps 90 or
 * 120 (framerate.txt) gives 60's table and the two frame-rate stores in slots of their own (26
 * entries, AGENT_PATCH_MAX), every entry on each game's stock bytes, the stores' value 2 or 3;
 * once applied (a copy of the eboot standing in for the game), the agent's confirm sets the frame
 * pace 1 and the refresh. */
static int native_check(int argc, char **argv, unsigned hikari_n) {
  int bad = 0;
  static const char *nm[5] = {0, "Hikari", "Chisato", "Allison", "Hikari 1.02"};
  u64 base = astro_hdr.game_base;
  for (u32 b = 1; b <= BUILD_N; b++) {
    const char *path = b == 1 ? argv[1] : argc > (int)b + 1 ? argv[b + 1] : 0;
    if (!path)
      continue;
    long n;
    unsigned char *e = load_file(path, &n);
    if (!e)
      continue;
    for (int r = 0; r < 2; r++) {
      u32 fps = r ? 90 : 120, rows = r ? 540 : 480;
      astro_hdr.build = b, astro_hdr.game_base = base, astro_hdr.want_fps = fps;
      struct a_patches *t = (struct a_patches *)multiview_boot(rows);
      if (!t || t->n != hikari_n + 2 || g_multiview_rows != rows || g_mv_fps != fps ||
          multiview_rate_preset()->fps != fps) {
        printf("FAIL: %s native %u: %u entries (60's %u + 2), rows %u, rate %u\n", nm[b], fps,
               t ? t->n : 0, hikari_n, g_multiview_rows, g_mv_fps), bad = 1;
        continue;
      }
      int stores = 0, hooks = 0;
      for (unsigned i = 0; i < t->n; i++) {
        const struct a_patch *p = &t->p[i];
        if (p->at + p->len + 0x4000 > (u64)n || memcmp(e + p->at + 0x4000, p->was, p->len) != 0)
          printf("FAIL: %s native %u: entry %u at 0x%x not on the stock bytes\n", nm[b], fps, i, p->at), bad = 1;
        if (p->at == k_insetpp_at[0] || p->at == k_insetpp_at[3])
          hooks++;
        if (p->at == k_fps_at[b - 1][0] && p->len == 10 && p->now[6] == (r ? 2 : 3))
          stores++;
        if (p->at == k_fps_at[b - 1][1] && p->len == 11 && p->now[7] == (r ? 2 : 3))
          stores++;
      }
      if (stores != 2 || hooks != 2)
        printf("FAIL: %s native %u: %d of the 2 stores, %d of the older tonemapper's and the "
               "reflections' hooks\n", nm[b], fps, stores, hooks), bad = 1;
      /* the confirm (once a process: Allison at 120 or the last game given) */
      if (b == 3 || (b == 1 && argc < 5)) {
        if (!r) {
          unsigned char *g = malloc(n);
          memcpy(g, e, n);
          for (unsigned i = 0; i < t->n; i++)
            memcpy(g + t->p[i].at + 0x4000, t->p[i].now, t->p[i].len);
          astro_hdr.game_base = (u64)(g + 0x4000), astro_hdr.frame_pace = 2, astro_hdr.refresh_hz = 0;
          multiview_native_confirm(); /* before the boot is over: nothing */
          if (astro_hdr.frame_pace != 2)
            printf("FAIL: the confirm before the boot returned\n"), bad = 1;
          multiview_booted();
          multiview_native_confirm();
          if (astro_hdr.frame_pace != 1 || astro_hdr.refresh_hz != 0)
            printf("FAIL: %s: the confirm gave pace %u refresh %u\n", nm[b], astro_hdr.frame_pace,
                   astro_hdr.refresh_hz), bad = 1;
          astro_hdr.game_base = base, astro_hdr.frame_pace = 2;
          free(g);
        }
      }
      printf("%s native %u: %u entries on the stock bytes, the stores %u, rows %u\n", nm[b], fps, t->n,
             r ? 2 : 3, g_multiview_rows);
    }
    free(e);
  }
  /* back to 60: 60's table */
  astro_hdr.build = 1, astro_hdr.want_fps = 0;
  struct a_patches *t60 = (struct a_patches *)multiview_boot(544);
  if (g_mv_fps != 0 || multiview_rate_preset()->fps != 60 || !t60 || t60->n != hikari_n ||
      multiview_rate_preset()->eye != 1440)
    printf("FAIL: back to 60\n"), bad = 1;
  return bad;
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  FILE *f = fopen(argv[1], "rb");
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *d = malloc(n);
  fread(d, 1, n, f);
  fclose(f);
  g_opt_insetstate = 0; /* (on by default: it calls the game's allocator, not here) */
  g_opt_insetsnap = 0; /* (the exact-centring checks below; snap_check tests it) */
  float pct = 100.0f; /* resolution.txt 960 */
  astro_hdr.game_base = 0x400000000ull;
  astro_hdr.build = 1; /* Hikari */
  memcpy(&astro_hdr.screen_pct, &pct, 4);
  struct a_patches *t = (struct a_patches *)multiview_boot(544);
  int bad = !t || t->magic != AGENT_PATCH_MAGIC;
  for (unsigned i = 0; !bad && i < t->n; i++) {
    struct a_patch *e = &t->p[i];
    if (memcmp(d + e->at + 0x4000, e->was, e->len) != 0) {
      printf("FAIL: entry %u at 0x%x: not the stock bytes\n", i, e->at);
      bad = 1;
    }
  }
  for (unsigned i = 0; !bad && i < t->n; i++)
    memcpy(d + t->p[i].at + 0x4000, t->p[i].now, t->p[i].len);
  if (multiview_boot(0) != 0)
    printf("FAIL: agent.multiview=0 gave a table\n"), bad = 1;
  FILE *o = fopen(argv[2], "wb");
  fwrite(d + 0x2f8ef50 + 0x4000, 1, 0x2f8fc30 - 0x2f8ef50, o);
  fclose(o);
  /* the jump pads hold the agent's three entries, the calls land on the pads */
  static const struct { u32 at, pad; u64 to; } k_calls[] = {
      {0x15d44bb, 0x15d3a82, 0}, {0x15d452a, 0x15d3912, 0}, {0x16de89c, 0x16df982, 0}};
  u64 to[3] = {(u64)mv_count_stub, (u64)mv_cs_wrap, (u64)mv_opt_wrap};
  for (int i = 0; !bad && i < 3; i++) {
    const unsigned char *c = d + k_calls[i].at + 0x4000, *j = d + k_calls[i].pad + 0x4000;
    int32_t rel;
    memcpy(&rel, c + 1, 4);
    u64 a;
    memcpy(&a, j + 6, 8);
    if (c[0] != 0xe8 || k_calls[i].at + 5 + rel != k_calls[i].pad || j[0] != 0xff || j[1] != 0x25 ||
        j[2] | j[3] | j[4] | j[5] || a != to[i])
      printf("FAIL: call %d at 0x%x\n", i, k_calls[i].at), bad = 1;
  }
  if (!bad && (d[0xcb1a3e + 0x4000] != 0x85 || d[0xcde69f + 0x4000] != 0x85))
    printf("FAIL: the clears' jumps\n"), bad = 1;
  /* the two getters' first 14 bytes: jmp [rip] to the agent's */
  static const struct { u32 at; } k_ea[] = {{0xb4cd90}, {0xb4d100}, {0x17abc20}, {0x16e1290}, {0xbc0bf0}, {0xc62741}};
  u64 ea_to[6] = {(u64)mv_ea_get, (u64)mv_ea_valid, (u64)mv_uvm, (u64)mv_lp_aro, (u64)mv_tlv, (u64)mv_ea_skip};
  for (int i = 0; !bad && i < 6; i++) {
    const unsigned char *j = d + k_ea[i].at + 0x4000;
    u64 a;
    memcpy(&a, j + 6, 8);
    if (j[0] != 0xff || j[1] != 0x25 || j[2] | j[3] | j[4] | j[5] || a != ea_to[i])
      printf("FAIL: the exposure getter at 0x%x\n", k_ea[i].at), bad = 1;
  }
  { /* mv_lp_aro_orig runs ULocalPlayer::AddReferencedObjects's first 20 bytes, then 0x16e12a4 */
    static const unsigned char k20[20] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb,
                                          0x49, 0x89, 0xf6, 0x48, 0x8b, 0xbb, 0x98, 0x00, 0x00, 0x00};
    unsigned char *e = malloc(n);
    FILE *g = fopen(argv[1], "rb");
    fread(e, 1, n, g);
    fclose(g);
    if (memcmp(e + 0x16e1290 + 0x4000, k20, 20) || memcmp(e + 0x16e12a4 + 0x4000, "\x48\x85\xff", 3) ||
        g_lp_aro_rest != astro_hdr.game_base + 0x16e12a4)
      printf("FAIL: ULocalPlayer::AddReferencedObjects's first 20 bytes or its rest\n"), bad = 1;
    /* mv_tlv_orig runs CalcTranslucencyLightingVolumeBounds's first 20 bytes, then 0xbc0c04 */
    static const unsigned char t20[20] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                          0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x88, 0x01, 0x00, 0x00};
    if (memcmp(e + 0xbc0bf0 + 0x4000, t20, 20) || g_tlv_rest != astro_hdr.game_base + 0xbc0c04)
      printf("FAIL: CalcTranslucencyLightingVolumeBounds's first 20 bytes or its rest\n"), bad = 1;
    /* insetpp.c: mv_tm_orig runs the tonemapper's first 20 bytes then 0xcb1634, mv_ao_orig the AO
     * pass's first 17 then 0xbf3b51; their first 14 bytes are the jumps to mv_tm / mv_ao */
    static const unsigned char tm20[20] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                           0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xf8, 0x01, 0x00, 0x00};
    static const unsigned char ao17[17] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                           0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};
    if (memcmp(e + 0xcb1620 + 0x4000, tm20, 20) || g_tm_rest != astro_hdr.game_base + 0xcb1634 ||
        memcmp(e + 0xbf3b40 + 0x4000, ao17, 17) || g_ao_rest != astro_hdr.game_base + 0xbf3b51)
      printf("FAIL: the tonemapper's or the AO pass's first bytes or their rests\n"), bad = 1;
    /* (the instructions the rests start at: mov rbx,[rip+..] and mov rax,[rip+..], in place) */
    if (memcmp(e + 0xcb1634 + 0x4000, "\x48\x8b\x1d", 3) || memcmp(e + 0xbf3b51 + 0x4000, "\x48\x8b\x05", 3))
      printf("FAIL: the rests' first instructions\n"), bad = 1;
    if (memcmp(e + 0xcaebb0 + 0x4000, tm20, 20) || g_tmf_rest != astro_hdr.game_base + 0xcaebc4 ||
        memcmp(e + 0xcaebc4 + 0x4000, "\x48\x8b\x1d", 3))
      printf("FAIL: the film tonemapper's first bytes or its rest\n"), bad = 1;
    /* mv_ssr_orig runs the reflections' trace's first 20 bytes (its sub 0x98), then 0xd4da44 */
    static const unsigned char ssr20[20] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                            0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x98, 0x00, 0x00, 0x00};
    if (memcmp(e + 0xd4da30 + 0x4000, ssr20, 20) || g_ssr_rest != astro_hdr.game_base + 0xd4da44 ||
        memcmp(e + 0xd4da44 + 0x4000, "\x48\x8b\x05", 3))
      printf("FAIL: the reflections' trace's first bytes or its rest\n"), bad = 1;
    for (int i = 0; i < 4; i++) {
      const unsigned char *j = d + k_insetpp_at[i] + 0x4000;
      u64 a;
      memcpy(&a, j + 6, 8);
      if (j[0] != 0xff || j[1] != 0x25 || j[2] | j[3] | j[4] | j[5] ||
          a != (i == 1 ? (u64)mv_ao : i == 2 ? (u64)mv_tmf : i == 3 ? (u64)mv_ssr : (u64)mv_tm))
        printf("FAIL: insetpp's jump at 0x%x\n", k_insetpp_at[i]), bad = 1;
    }

    /* mv_lod_orig runs ComputeLODForMeshes's first 17 bytes, then 0x17a7781 (a rip-relative load,
     * in place); the occlusion call lands on its pad, which jumps to mv_occ_skip here */
    static const unsigned char lod17[17] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                            0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28};
    if (memcmp(e + 0x17a7770 + 0x4000, lod17, 17) || g_lod_rest != astro_hdr.game_base + 0x17a7781 ||
        memcmp(e + 0x17a7781 + 0x4000, "\x48\x8b\x05", 3))
      printf("FAIL: ComputeLODForMeshes's first bytes or its rest\n"), bad = 1;
    {
      const unsigned char *c = d + 0xb540e4 + 0x4000, *j = d + 0xb4d212 + 0x4000, *l = d + 0x17a7770 + 0x4000;
      int32_t rel;
      u64 a, la;
      memcpy(&rel, c + 1, 4);
      memcpy(&a, j + 6, 8);
      memcpy(&la, l + 6, 8);
      if (c[0] != 0xe8 || 0xb540e4 + 5 + rel != 0xb4d212 || j[0] != 0xff || j[1] != 0x25 ||
          a != (u64)mv_occ_skip || g_occ_fn != astro_hdr.game_base + 0xb4d530 || l[0] != 0xff ||
          la != (u64)mv_lod)
        printf("FAIL: the occlusion call, its pad or the LOD jump\n"), bad = 1;
      /* the stock call went to DoOcclusionQueries */
      memcpy(&rel, e + 0xb540e4 + 1 + 0x4000, 4);
      if (e[0xb540e4 + 0x4000] != 0xe8 || 0xb540e4 + 5 + rel != 0xb4d530)
        printf("FAIL: the stock call at 0xb540e4\n"), bad = 1;
    }
    free(e);
  }
  bad |= step3_frames();
  bad |= ea_check();
  bad |= lod_occ_check();
  bad |= tlv_check();
  bad |= eagate_check();
  bad |= uvm_check();
  bad |= gaze_layout_check();
  bad |= snap_check();
  bad |= gaze_hold_check();
  bad |= gaze_smooth_check();
  bad |= frame_pose_check();
  bad |= aro_check();
  unsigned hikari_n = t ? t->n : 0;
  bad |= other_builds(argc, argv, hikari_n);
  bad |= native_check(argc, argv, hikari_n);
  printf(bad ? "FAIL\n" : "ok: %u entries on the stock bytes\n", hikari_n);
  return bad;
}
