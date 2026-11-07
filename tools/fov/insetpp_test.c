/* agent/insetpp.c on the host: its two hooks against a fake engine (the four views, the passes'
 * inputs, a command list, the pool, the global shader map), then the GPU's sampling simulated
 * with the draws they recorded, against the eye's own bloom and AO at the inset's directions.
 *   cc -O2 -I../../agent -o /tmp/pptest insetpp_test.c -lm && /tmp/pptest
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
#define BLOOM_FMT 9 /* the eyes' bloom textures: R11G11B10 floats (PF_FloatRGB, logs/pp2-rec1) */
static int g_quiet;
void agent_log(const char *fmt, ...) {
  if (g_quiet)
    return;
  va_list ap;
  va_start(ap, fmt);
  printf("  log: ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
}
#define MV_HOST_TEST
#include "../../agent/insetpp.c"

/* ---- the fake engine ---- */
static u8 *g_mem; /* the game's statics: base + 0x4600000 .. + 0x4b00000 */
static u64 g_base;
static u8 *g_chunk;
static u64 g_root; /* the list's first command (its link starts here) */
static u8 g_cl[0x200];
static void f_alloc_chunk(u64 stack, s32 size) {
  u8 *c = calloc(1, 1 << 20);
  *(u64 *)stack = (u64)c, *(u64 *)(stack + 8) = (u64)c + (1 << 20);
  (void)size;
}
static struct { u64 level, bss, decl, vs, ps, gs; } g_bss_rec[16];
static int g_bss_n;
static void f_set_bss(u64 cl, u32 level, u64 bss, u64 decl, u64 vs, u64 ps, u64 gs) {
  (void)cl;
  g_bss_rec[g_bss_n].level = level, g_bss_rec[g_bss_n].bss = bss, g_bss_rec[g_bss_n].decl = decl;
  g_bss_rec[g_bss_n].vs = vs, g_bss_rec[g_bss_n].ps = ps, g_bss_rec[g_bss_n].gs = gs;
  g_bss_n++;
}
struct rect_call {
  float x, y, w, h, u, v, su, sv;
  s32 tw, th, sw, sh;
  u64 vs;
  u32 flags;
  u32 cmds_before; /* the list's count then */
};
static struct rect_call g_rects[16];
static int g_rect_n;
static void f_draw_rect(u64 cl, float x, float y, float w, float h, float u, float v, float su, float sv,
                        u64 target, u64 tex, u64 vs, u32 flags) {
  struct rect_call *r = &g_rects[g_rect_n++];
  r->x = x, r->y = y, r->w = w, r->h = h, r->u = u, r->v = v, r->su = su, r->sv = sv;
  r->tw = (s32)(u32)target, r->th = (s32)(target >> 32), r->sw = (s32)(u32)tex, r->sh = (s32)(tex >> 32);
  r->vs = vs, r->flags = flags, r->cmds_before = *(u32 *)(cl + 0x14);
}
static struct { u64 tex, depth; u32 barrier; } g_rt_rec[16];
static int g_rt_n;
static void f_set_rt(u64 cl, u64 tex, u64 depth, u32 barrier) {
  (void)cl;
  g_rt_rec[g_rt_n].tex = tex, g_rt_rec[g_rt_n].depth = depth, g_rt_rec[g_rt_n].barrier = barrier;
  g_rt_n++;
}
/* a pooled target: vtable (AddRef, Release), TargetableTexture, ShaderResourceTexture */
struct fake_rt {
  u64 vt, target, srv;
  int refs;
  s32 w, h;
};
static void rt_addref(u64 p) { ((struct fake_rt *)p)->refs++; }
static void rt_release(u64 p) { ((struct fake_rt *)p)->refs--; }
static u64 k_rt_vt[2] = {(u64)rt_addref, (u64)rt_release};
static struct fake_rt *new_rt(s32 w, s32 h, u64 tex) {
  struct fake_rt *r = calloc(1, sizeof *r);
  r->vt = (u64)k_rt_vt, r->target = tex, r->srv = tex + 1, r->w = w, r->h = h, r->refs = 1;
  return r;
}
static struct fake_rt *g_pool_last;
static int g_pool_n;
static s32 g_pool_fmt; /* the format F was asked in */
static u32 f_find_free(u64 pool, u64 cl, const void *desc, u64 *out, const u16 *name, u32 barrier) {
  (void)pool, (void)cl, (void)name, (void)barrier;
  const u8 *d = desc;
  g_pool_fmt = *(const s32 *)(d + 0x2c);
  g_pool_last = new_rt(*(const s32 *)(d + 0x14), *(const s32 *)(d + 0x18), 0x5000 + 0x10 * (u64)g_pool_n++);
  g_pool_last->refs = 2; /* the pool's and ours */
  *out = (u64)g_pool_last;
  return 0;
}
static u8 g_sc[0x300];
static u64 f_scene_ctx(u64 cl) { (void)cl; return (u64)g_sc; }
static void f_shader_rhi(u64 res) { *(u8 *)(res + 0x28) = 1, *(u64 *)(res + 0x38) = 0xa55, *(u64 *)(res + 0x40) = 0xb55; }
static u64 f_blend(void) { return 0xb1e; }
static u64 f_raster(void) { return 0xa57; }
static u64 f_depth(void) { return 0xde9; }
static u64 f_samp_bi(void) { return 0x5b1; }
static u64 f_samp_pt(void) { return 0x5b0; }

/* the global shader map with FScreenVS and FScreenPS */
static u8 g_map[0x80], g_vs[0x100], g_ps[0x100], g_res_vs[0x50], g_res_ps[0x50];
static u8 g_elems[24 * 2];
static s32 g_hash[8];
static u64 g_vs_structs[2];
static void make_map(void) {
  memset(g_hash, 0xff, sizeof g_hash);
  u64 keys[2] = {g_base + T_SCREEN_VS, g_base + T_SCREEN_PS}, vals[2] = {(u64)g_vs, (u64)g_ps};
  for (int i = 0; i < 2; i++) {
    u8 *e = g_elems + 24 * i;
    *(u64 *)e = keys[i], *(u64 *)(e + 8) = vals[i];
    u32 h = hash_combine((u32)(keys[i] >> 4), 0) & 7;
    *(s32 *)(e + 16) = g_hash[h];
    g_hash[h] = i;
  }
  *(u64 *)(g_map + 0x30) = (u64)g_elems, *(s32 *)(g_map + 0x38) = 2;
  *(u64 *)(g_map + 0x70) = (u64)g_hash, *(s32 *)(g_map + 0x78) = 8;
  g_vs_structs[0] = 0x1234, g_vs_structs[1] = g_base + G_DRAWRECT_UB;
  *(u64 *)(g_vs + 0x8) = (u64)g_vs_structs, *(s32 *)(g_vs + 0x10) = 2, *(u64 *)(g_vs + 0x48) = (u64)g_res_vs;
  *(u16 *)(g_ps + 0xbc) = 3, *(u16 *)(g_ps + 0xbe) = 1, *(u16 *)(g_ps + 0xc0) = 5, *(u16 *)(g_ps + 0xc2) = 1;
  *(u64 *)(g_ps + 0x48) = (u64)g_res_ps;
}

/* the views: buffer 2880x2220, eyes 1440x1620 side by side, insets 1200x600 below them */
#define BW 2880
#define BH 2220
static u8 g_view[4][0x1000];
static u64 g_views[4];
static u8 g_fam[0x20];
static const s32 k_rects[4][4] = {{0, 0, 1440, 1620}, {1440, 0, 2880, 1620}, {0, 1620, 1200, 2220},
                                  {1200, 1620, 2400, 2220}};
/* the window each inset shows, in its eye's NDC (multiview.c's cx, cy, sx, sy) */
static const float k_win[2][4] = {{0.12f, -0.07f, 0.41f, 0.23f}, {-0.09f, 0.05f, 0.41f, 0.23f}};
static const float k_jit[2][2] = {{0.00031f, -0.00022f}, {-0.00027f, 0.00035f}}; /* the eyes' TAA jitter */
static void eye_proj(int e, float *p) { /* PS VR-like, off-axis; row-major M[r][c] at r*4+c */
  memset(p, 0, 64);
  p[0] = 0.83f, p[5] = 0.93f, p[8] = e ? 0.081f : -0.081f, p[9] = 0.017f, p[11] = 1.0f, p[14] = 10.0f;
}
static void make_views(void) {
  for (int i = 0; i < 4; i++) {
    u8 *v = g_view[i];
    memset(v, 0, sizeof g_view[i]);
    *(u64 *)v = (u64)g_fam;
    memcpy(v + 0x38, k_rects[i], 16);
    *(u32 *)(v + 0x420) = i == 0 ? 1 : i == 1 ? 2 : 0;
    g_views[i] = (u64)v;
  }
  for (int e = 0; e < 2; e++) {
    float p[16], pi[16];
    eye_proj(e, p);
    for (int r = 0; r < 4; r++) { /* the inset: each row's x' = (x - cx w) / sx (unjittered) */
      pi[r * 4 + 0] = (p[r * 4 + 0] - k_win[e][0] * p[r * 4 + 3]) / k_win[e][2];
      pi[r * 4 + 1] = (p[r * 4 + 1] - k_win[e][1] * p[r * 4 + 3]) / k_win[e][3];
      pi[r * 4 + 2] = p[r * 4 + 2], pi[r * 4 + 3] = p[r * 4 + 3];
    }
    p[8] += k_jit[e][0], p[9] += k_jit[e][1]; /* the renderer's jitter in the eye's */
    memcpy(g_view[e] + 0x70, p, 64);
    memcpy(g_view[e] + 0x1e4, k_jit[e], 8);
    memcpy(g_view[2 + e] + 0x70, pi, 64);
  }
  *(u64 *)g_fam = (u64)g_views, *(s32 *)(g_fam + 8) = 4;
}
/* where an inset's direction lands in its eye's buffer (the truth: the window's own numbers),
 * with or without the eye's jitter */
static double truth_to_eye(int e, int axis, double p, int jittered) {
  const s32 *ir = k_rects[2 + e], *er = k_rects[e];
  double n = 2.0 * (p - ir[axis]) / (ir[axis + 2] - ir[axis]) - 1.0;
  if (axis)
    n = -n;
  double ne = k_win[e][axis + 2] * n + k_win[e][axis] + (jittered ? k_jit[e][axis] : 0.0);
  if (axis)
    ne = -ne;
  return er[axis] + (ne + 1.0) * 0.5 * (er[axis + 2] - er[axis]);
}

/* the passes: a tonemapper's inputs 0 (the scene) and 1 (the bloom) */
struct fake_src {
  u64 vt;
  u8 out[0x58];
};
static u64 src_get_output(u64 src, u32 id) { (void)id; return (u64)((struct fake_src *)src)->out; }
static u64 k_src_vt[9];
struct fake_tm {
  u64 vt;
  struct { u64 src; u32 id, pad; } in[3];
};
static u64 tm_get_input(u64 p, u32 id) { return id < 3 ? (u64)&((struct fake_tm *)p)->in[id] : 0; }
static u64 k_tm_vt[9];
static struct fake_src *new_src(s32 w, s32 h, struct fake_rt *rt) {
  struct fake_src *s = calloc(1, sizeof *s);
  s->vt = (u64)k_src_vt;
  *(s32 *)(s->out + 0x14) = w, *(s32 *)(s->out + 0x18) = h;
  *(s32 *)(s->out + 0x2c) = w == BW ? 10 : BLOOM_FMT; /* the scene FloatRGBA, the bloom FloatRGB */
  *(u64 *)(s->out + 0x50) = (u64)rt;
  return s;
}
static struct fake_tm g_tm[4];
static struct fake_rt *g_bloom_rt[4];
static void make_passes(void) {
  k_src_vt[8] = (u64)src_get_output;
  k_tm_vt[2] = (u64)tm_get_input;
  for (int i = 0; i < 4; i++) {
    g_tm[i].vt = (u64)k_tm_vt;
    g_tm[i].in[0].src = (u64)new_src(BW, BH, new_rt(BW, BH, 0x100 + 0x10 * (u64)i));
    g_bloom_rt[i] = new_rt(BW / 2, BH / 2, 0x200 + 0x10 * (u64)i);
    g_tm[i].in[1].src = (u64)new_src(BW / 2, BH / 2, g_bloom_rt[i]);
  }
}
static int g_tm_calls, g_ao_calls;
static u64 g_tm_in1_seen;
static void f_tm(u64 p, u64 c) {
  (void)c;
  g_tm_calls++;
  struct fake_tm *t = (struct fake_tm *)p;
  struct fake_src *src = (struct fake_src *)t->in[1].src;
  g_tm_in1_seen = *(u64 *)(src->out + 0x50);
}
static void f_ao(u64 p, u64 c) { (void)p, (void)c; g_ao_calls++; }
static u8 g_ctx[0x60];
static void ctx_for(int view, u64 pass) {
  memset(g_ctx, 0, sizeof g_ctx);
  *(u64 *)g_ctx = g_views[view], *(u64 *)(g_ctx + 0x10) = pass, *(u64 *)(g_ctx + 0x28) = (u64)g_cl;
  *(u32 *)(g_ctx + 0x40) = 3, *(u64 *)(g_ctx + 0x48) = (u64)g_map;
}
static void reset_list(void) {
  g_root = 0;
  memset(g_cl, 0, sizeof g_cl);
  *(u64 *)(g_cl + 0x8) = (u64)&g_root;
  *(u64 *)(g_cl + 0x30) = (u64)g_chunk, *(u64 *)(g_cl + 0x38) = (u64)g_chunk + 4096; /* a small chunk: the next one is asked for */
  g_rect_n = g_rt_n = g_bss_n = 0;
}
/* the list's commands in order: their execute function (less the base) */
static int list_cmds(u64 *out, u8 **cmd, int max) {
  int n = 0;
  for (u64 c = g_root; c && n < max; c = *(u64 *)c)
    out[n] = *(u64 *)(c + 8) - g_base, cmd[n] = (u8 *)c, n++;
  return n;
}

/* ---- the GPU's sampling ---- */
/* the eyes' bloom format (BLOOM_FMT), F's too: its red and green to 7 significant bits (a 6-bit
 * mantissa) and back (normal range) */
static float fmt_round(float x) {
  if (x == 0.0f)
    return 0.0f;
  int e;
  float m = frexpf(x, &e);         /* x = m 2^e, 0.5 <= |m| < 1 */
  m = roundf(m * 128.0f) / 128.0f; /* 7 significant bits */
  return ldexpf(m, e);
}
static int g_wq; /* the hardware's 8-bit filter weights */
static float wq(float f) { return g_wq ? roundf(f * 256.0f) / 256.0f : f; }
/* a texture w x h sampled at texel coordinates (centres at i + 0.5), clamped */
static float bilinear(const float *t, int w, int h, double tu, double tv) {
  double x = tu - 0.5, y = tv - 0.5;
  int x0 = (int)floor(x), y0 = (int)floor(y);
  float fx = wq((float)(x - x0)), fy = wq((float)(y - y0));
  int xa = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= w ? w - 1 : x0 + 1;
  int ya = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= h ? h - 1 : y0 + 1;
  float a = t[ya * w + xa] + fx * (t[ya * w + xb] - t[ya * w + xa]);
  float b = t[yb * w + xa] + fx * (t[yb * w + xb] - t[yb * w + xa]);
  return a + fy * (b - a);
}
static float point(const float *t, int w, int h, double tu, double tv) {
  int x = (int)floor(tu), y = (int)floor(tv);
  x = x < 0 ? 0 : x >= w ? w - 1 : x, y = y < 0 ? 0 : y >= h ? h - 1 : y;
  return t[y * w + x];
}
/* a draw recorded: each destination pixel of the viewport takes the source at its centre */
static void run_draw(const struct rect_call *r, s32 vx0, s32 vy0, float *dst, int dw, const float *src, int pt,
                     int quant8) {
  for (int y = 0; y < (int)r->h; y++)
    for (int x = 0; x < (int)r->w; x++) {
      double tu = r->u + (x + 0.5) * r->su / r->w, tv = r->v + (y + 0.5) * r->sv / r->h;
      float s = pt ? point(src, r->sw, r->sh, tu, tv) : bilinear(src, r->sw, r->sh, tu, tv);
      s = quant8 ? roundf(s * 255.0f) / 255.0f : fmt_round(s);
      dst[(vy0 + y) * dw + vx0 + x] = s;
    }
}

/* the eye's bloom: lights of every size (half-size texels), some near and across the insets' edges */
static void fill_bloom(float *t, int w, int h, double sc) { /* sc: the texels' size against half's */
  /* (the windows map to the eye's bloom texels ~255-551 x 339-526 (left) and ~900-1196 x
   * 291-478 (right): lights inside, on and just outside their edges) */
  static const struct { float x, y, a, s; } k[] = {
      {300, 400, 3.0f, 2.5f},  {310, 405, 1.5f, 8},   {450, 470, 2.0f, 3.5f}, {540, 520, 1.0f, 12},
      {256, 430, 4.0f, 2},     {250, 360, 2.5f, 3},   {553, 345, 3.0f, 2.2f}, {400, 528, 2.0f, 2.5f},
      {150, 450, 0.8f, 30},    {500, 300, 0.4f, 90},  {720, 400, 0.3f, 300},
      {1000, 400, 2.5f, 3},    {901, 300, 3.0f, 2},   {1196, 478, 3.0f, 2.2f}, {1100, 290, 2.0f, 2.5f},
      {1260, 405, 1.2f, 5},    {980, 320, 0.6f, 40},  {1300, 450, 0.5f, 140}, {1050, 380, 4.0f, 2}};
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      double v = 0.02;
      for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {
        double dx = x + 0.5 - k[i].x / sc, dy = y + 0.5 - k[i].y / sc, sg = k[i].s / sc < 1.2 ? 1.2 : k[i].s / sc;
        v += k[i].a * exp(-(dx * dx + dy * dy) / (2.0 * sg * sg));
      }
      t[y * w + x] = fmt_round((float)v);
    }
}
/* the eye's AO: soft shading with creases (one value a buffer pixel, 8 bits) */
static void fill_ao(float *t, int w, int h) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      double v = 0.75 + 0.2 * sin(x * 0.013) * cos(y * 0.011);
      v -= 0.4 * exp(-pow((x - 300.0 - 0.3 * y) / 3.0, 2)); /* a crease, a few pixels wide */
      v -= 0.3 * exp(-pow((y - 1150.0) / 6.0, 2));
      v -= 0.25 * exp(-(pow(x - 2000.0, 2) + pow(y - 1200.0, 2)) / 400.0);
      v = v < 0 ? 0 : v > 1 ? 1 : v;
      t[y * w + x] = roundf((float)v * 255.0f) / 255.0f;
    }
}

static int check_commands(int draws, int bloom_eye) {
  u64 x[64];
  u8 *c[64];
  int n = list_cmds(x, c, 64), bad = 0, k = 0;
  /* each draw: viewport, blend, rasterizer and depth (the first only: filtered after), the
   * texture, the sampler, the resolve (SetRenderTarget, the bound shader state and DrawRectangle
   * are the engine's functions, faked here) */
  for (int d = 0; d < draws; d++) {
    if (k >= n || x[k] != X_VIEWPORT) { printf("FAIL: draw %d: no viewport\n", d); return 1; }
    float z0, z1;
    memcpy(&z0, c[k] + 0x18, 4), memcpy(&z1, c[k] + 0x24, 4);
    if (z0 != 0.0f || z1 != 1.0f || *(s32 *)(c[k] + 0x1c) - *(s32 *)(c[k] + 0x10) != (s32)g_rects[d].w)
      printf("FAIL: draw %d's viewport\n", d), bad = 1;
    k++;
    if (k >= n || x[k] != X_BLEND || *(u64 *)(c[k] + 0x10) != 0xb1e || memcmp(c[k] + 0x18, g_mem + 0xc0440, 16))
      printf("FAIL: draw %d's blend state\n", d), bad = 1;
    k++;
    if (d == 0) {
      if (k + 1 >= n || x[k] != X_RASTER || x[k + 1] != X_DEPTH || *(u64 *)(c[k] + 0x10) != 0xa57 ||
          *(u64 *)(c[k + 1] + 0x10) != 0xde9 || *(u64 *)(g_cl + 0x98) != (u64)c[k] ||
          *(u64 *)(g_cl + 0xa0) != (u64)c[k + 1])
        printf("FAIL: the first draw's rasterizer and depth states\n"), bad = 1;
      k += 2;
    }
    if (k + 2 >= n || x[k] != X_PS_TEXTURE || x[k + 1] != X_PS_SAMPLER || x[k + 2] != X_RESOLVE ||
        *(u64 *)(c[k] + 0x10) != 0xb55 || *(u32 *)(c[k] + 0x18) != 3 || *(u32 *)(c[k + 1] + 0x18) != 5)
      printf("FAIL: draw %d's texture, sampler, resolve\n", d), bad = 1;
    else if (*(s32 *)(c[k + 2] + 0x14) != -1 || *(s32 *)(c[k + 2] + 0x20) != -1 || c[k + 2][0x40] != 0)
      printf("FAIL: draw %d's resolve parameters\n", d), bad = 1;
    k += 3;
  }
  if (k != n)
    printf("FAIL: %d commands, %d expected\n", n, k), bad = 1;
  if (*(u32 *)(g_cl + 0x14) != (u32)n)
    printf("FAIL: the list's count %u, %d commands\n", *(u32 *)(g_cl + 0x14), n), bad = 1;
  for (int d = 0; d < g_bss_n; d++)
    if (g_bss_rec[d].vs != (u64)g_vs || g_bss_rec[d].ps != (u64)g_ps || g_bss_rec[d].level != 3 ||
        g_bss_rec[d].decl != 0xdec1 || g_bss_rec[d].gs)
      printf("FAIL: the bound shader state %d\n", d), bad = 1;
  (void)bloom_eye;
  return bad;
}

int main(void) {
  g_mem = calloc(1, 0x500000);
  g_base = (u64)g_mem - 0x4600000;
  g_chunk = calloc(1, 1 << 20);
  float white[4] = {1, 1, 1, 1};
  memcpy(g_mem + 0xc0440, white, 16);   /* FLinearColor::White */
  *(s32 *)(g_mem + 0x6d854) = 1;        /* the list's state filtering on */
  *(u64 *)(g_mem + 0x4050f0) = 0xdec1;  /* GFilterVertexDeclaration's RHI declaration */
  insetpp_boot(g_base);
  g_e.alloc_chunk = f_alloc_chunk, g_e.set_bss = f_set_bss, g_e.draw_rect = f_draw_rect;
  g_e.set_rt = f_set_rt, g_e.find_free = f_find_free, g_e.scene_ctx = f_scene_ctx;
  g_e.shader_rhi = f_shader_rhi, g_e.blend = f_blend, g_e.raster = f_raster, g_e.depth = f_depth;
  g_e.samp_bi = f_samp_bi, g_e.samp_pt = f_samp_pt;
  o_tm = f_tm, o_ao = f_ao;
  make_map(), make_views(), make_passes();
  int bad = 0;
  g_opt_insetpp = 3;
  if (g_tm_rest != g_base + 0xcb1634 || g_ao_rest != g_base + 0xbf3b51)
    printf("FAIL: the rests\n"), bad = 1;

  for (int fi = 0; fi < 2; fi++) {
  int frac = fi ? 16 : 2;
  /* ---- bloom: a frame's four tonemappers ---- */
  float *be[2];
  s32 ew = (BW + frac - 1) / frac, eh = (BH + frac - 1) / frac;
  for (int v = 0; v < 4; v++) { /* the bloom inputs at this size */
    g_bloom_rt[v] = new_rt(ew, eh, 0x200 + 0x10 * (u64)v);
    g_tm[v].in[1].src = (u64)new_src(ew, eh, g_bloom_rt[v]);
  }
  printf("  bloom at 1/%d of the buffer (%dx%d):\n", frac, ew, eh);
  for (int e = 0; e < 2; e++) {
    be[e] = malloc(sizeof(float) * ew * eh);
    fill_bloom(be[e], ew, eh, frac / 2.0);
  }
  float *F = malloc(sizeof(float) * BW * BH);
  for (long i = 0; i < (long)BW * BH; i++)
    F[i] = NAN; /* (a texel read with any weight before it is written shows) */
  struct fake_rt *frt = 0;
  for (int v = 0; v < 4; v++) {
    reset_list();
    ctx_for(v, (u64)&g_tm[v]);
    int calls = g_tm_calls, pools = g_pool_n;
    mv_tm((u64)&g_tm[v], (u64)g_ctx);
    if (g_tm_calls != calls + 1)
      printf("FAIL: view %d: the engine's tonemapper not run once\n", v), bad = 1;
    u64 in1 = *(u64 *)(((struct fake_src *)g_tm[v].in[1].src)->out + 0x50);
    if (in1 != (u64)g_bloom_rt[v])
      printf("FAIL: view %d: its bloom input not put back\n", v), bad = 1;
    if (v < 2) {
      if (g_rect_n != 1 || g_rt_n != 1 || g_bss_n != 1) {
        printf("FAIL: eye %d: %d rectangles, %d targets, %d shader states\n", v, g_rect_n, g_rt_n, g_bss_n);
        bad = 1;
        continue;
      }
      if (g_pool_n != pools + (v == 0))
        printf("FAIL: eye %d: F allocated %d times\n", v, g_pool_n - pools), bad = 1;
      if (v == 0)
        frt = (struct fake_rt *)g_f.rt;
      bad |= check_commands(1, 1);
      struct rect_call *r = &g_rects[0];
      if (r->vs != (u64)g_vs || r->flags != 0)
        printf("FAIL: eye %d: DrawRectangle's shader or flags\n", v), bad = 1;
      if (v == 0 && g_pool_fmt != BLOOM_FMT)
        printf("FAIL: F asked in format %d, not the eyes' bloom's %d\n", g_pool_fmt, BLOOM_FMT), bad = 1;
      if (!frt || frt->w != BW || frt->h != BH || g_f.rt != (u64)frt || frt->refs != 2 ||
          g_rt_rec[0].tex != frt->target || !g_rt_rec[0].barrier)
        printf("FAIL: eye %d: F's size, references or target\n", v), bad = 1;
      if (r->sw != ew || r->sh != eh)
        printf("FAIL: eye %d: the source's size %dx%d\n", v, r->sw, r->sh), bad = 1;
      u64 x[64]; u8 *c[64];
      int n = list_cmds(x, c, 64);
      s32 vx0 = -1, vy0 = -1, vx1 = -1, vy1 = -1;
      for (int i = 0; i < n; i++) {
        if (x[i] == X_PS_TEXTURE && *(u64 *)(c[i] + 0x20) != g_bloom_rt[v]->srv)
          printf("FAIL: eye %d: the source isn't its bloom\n", v), bad = 1;
        if (x[i] == X_PS_SAMPLER && *(u64 *)(c[i] + 0x20) != 0x5b1)
          printf("FAIL: eye %d: not sampled bilinearly\n", v), bad = 1;
        if (x[i] == X_VIEWPORT)
          vx0 = *(s32 *)(c[i] + 0x10), vy0 = *(s32 *)(c[i] + 0x14), vx1 = *(s32 *)(c[i] + 0x1c), vy1 = *(s32 *)(c[i] + 0x20);
      }
      /* the rect and its ring, not over the other inset, inside the buffer */
      static const s32 k_want[2][4] = {{0, 1619, 1200, 2220}, {1200, 1619, 2401, 2220}};
      if (vx0 != k_want[v][0] || vy0 != k_want[v][1] || vx1 != k_want[v][2] || vy1 != k_want[v][3])
        printf("FAIL: eye %d: F's rect %d,%d-%d,%d\n", v, vx0, vy0, vx1, vy1), bad = 1;
      run_draw(r, vx0, vy0, F, BW, be[v], 0, 0);
    } else {
      if (g_rect_n || g_rt_n || g_bss_n)
        printf("FAIL: inset %d drew\n", v - 2), bad = 1;
      if (g_tm_in1_seen != (u64)frt)
        printf("FAIL: inset %d's tonemapper didn't see F as its bloom\n", v - 2), bad = 1;
      if (v == 3 && (g_f.rt || !frt || frt->refs != 1))
        printf("FAIL: F not let go after the right inset's\n"), bad = 1;
      if (v == 2 && g_f.rt != (u64)frt)
        printf("FAIL: F let go before the right inset's\n"), bad = 1;
    }
  }
  /* the comparison: the inset's tonemapper samples F at each pixel's centre; the eye's tonemapper
   * its bloom at the same direction (unjittered: bloom is made from the TAA's output) */
  for (int wqi = 0; wqi < 2; wqi++) {
    g_wq = wqi;
    for (int e = 0; e < 2; e++) {
      const s32 *ir = k_rects[2 + e];
      double maxerr = 0, maxedge = 0, sq = 0, peak = 0, maxrel = 0;
      long nn = 0, nan = 0;
      for (int y = ir[1]; y < ir[3]; y++)
        for (int x = ir[0]; x < ir[2]; x++) {
          double ours = bilinear(F, BW, BH, x + 0.5, y + 0.5);
          double xe = truth_to_eye(e, 0, x + 0.5, 0), ye = truth_to_eye(e, 1, y + 0.5, 0);
          double truth = bilinear(be[e], ew, eh, xe * ew / BW, ye * eh / BH);
          if (ours != ours) {
            nan++;
            continue;
          }
          double d = fabs(ours - truth);
          int edge = x < ir[0] + 2 || x >= ir[2] - 2 || y < ir[1] + 2 || y >= ir[3] - 2;
          maxerr = d > maxerr ? d : maxerr;
          if (edge)
            maxedge = d > maxedge ? d : maxedge;
          double rel = d / (truth > 0.05 ? truth : 0.05);
          maxrel = rel > maxrel ? rel : maxrel;
          sq += d * d, nn++;
          peak = truth > peak ? truth : peak;
        }
      printf("  bloom, inset %d%s: the eye's at the inset's pixels, worst %.5f (edges %.5f), rms "
             "%.6f, peak %.3f; worst relative %.5f\n", e, wqi ? " (8-bit filter weights)" : "", maxerr,
             maxedge, sqrt(sq / (nn ? nn : 1)), peak, maxrel);
      if (nan)
        printf("FAIL: inset %d: %ld pixels read an unwritten texel\n", e, nan), bad = 1;
      /* (F holds each value rounded once more in the eyes' format: a 6-bit mantissa's half step,
       * up to 1/128 of the value; with the texture unit's 8-bit weights a sample position a float
       * rounding apart can take a weight 1/256 off: the filter's own precision, which the eye's
       * samples have too) */
      if (maxrel > (wqi ? 0.016 : 0.0079) || peak < (frac == 2 ? 3.0 : 0.5)) /* (the weights' row: a sanity bound; at a 16th the test's lights are 1.2 texels, far sharper than a real bloom there) */
        printf("FAIL: inset %d's bloom off the eye's by %.5f (peak %.3f)\n", e, maxrel, peak), bad = 1;
    }
  }
  g_wq = 0;
  {
    long touched = 0;
    for (int y = 0; y < BH; y++)
      for (int x = 0; x < BW; x++)
        if (F[y * BW + x] == F[y * BW + x] && !(y >= 1619 && x < 2401))
          touched++;
    if (touched)
      printf("FAIL: %ld of F's texels written past the insets' rects and rings\n", touched), bad = 1;
  }

  }
  /* ---- a frame where the insets' tonemappers don't come: F let go at the next left eye's ---- */
  reset_list();
  ctx_for(0, (u64)&g_tm[0]);
  mv_tm((u64)&g_tm[0], (u64)g_ctx);
  struct fake_rt *held = (struct fake_rt *)g_f.rt;
  reset_list();
  mv_tm((u64)&g_tm[0], (u64)g_ctx);
  if (!held || held->refs != 1 || g_f.rt == (u64)held)
    printf("FAIL: an unused F not let go at the next frame\n"), bad = 1;
  /* only the left eye's drawn: the inset's tonemapper keeps its own bloom */
  reset_list();
  ctx_for(2, (u64)&g_tm[2]);
  mv_tm((u64)&g_tm[2], (u64)g_ctx);
  if (g_tm_in1_seen != (u64)g_bloom_rt[2])
    printf("FAIL: F given to an inset without both insets drawn\n"), bad = 1;
  /* agent.insetpp=0: nothing drawn, F let go, the engine's tonemapper still run */
  g_opt_insetpp = 0;
  held = (struct fake_rt *)g_f.rt;
  reset_list();
  ctx_for(0, (u64)&g_tm[0]);
  int calls = g_tm_calls;
  mv_tm((u64)&g_tm[0], (u64)g_ctx);
  if (g_rect_n || g_tm_calls != calls + 1 || g_f.rt || !held || held->refs != 1)
    printf("FAIL: agent.insetpp=0\n"), bad = 1;
  g_opt_insetpp = 3;
  /* two views only (the insets off): nothing */
  *(s32 *)(g_fam + 8) = 2;
  reset_list();
  mv_tm((u64)&g_tm[0], (u64)g_ctx);
  if (g_rect_n)
    printf("FAIL: drawn with two views\n"), bad = 1;
  *(s32 *)(g_fam + 8) = 4;

  /* ---- AO: the inset's last AO pass ---- */
  {
    float *ao = malloc(sizeof(float) * BW * BH), *ao_eye = malloc(sizeof(float) * BW * BH);
    fill_ao(ao, BW, BH);
    memcpy(ao_eye, ao, sizeof(float) * BW * BH);
    struct fake_rt *aort = new_rt(BW, BH, 0x700);
    *(u64 *)(g_sc + 0xd8) = (u64)aort;
    *(s32 *)(g_sc + 0x210) = BW, *(s32 *)(g_sc + 0x214) = BH;
    u8 aopass[0x100];
    memset(aopass, 0, sizeof aopass);
    for (int v = 0; v < 4; v++) {
      for (int setup = 0; setup < 2; setup++) {
        aopass[0xdc] = (u8)setup;
        reset_list();
        ctx_for(v, (u64)aopass);
        int c0 = g_ao_calls;
        mv_ao((u64)aopass, (u64)g_ctx);
        if (g_ao_calls != c0 + 1)
          printf("FAIL: the engine's AO pass not run once\n"), bad = 1;
        int want = v >= 2 && !setup;
        if (g_rect_n != want) {
          printf("FAIL: view %d (setup %d): %d AO draws\n", v, setup, g_rect_n), bad = 1;
          continue;
        }
        if (!want)
          continue;
        bad |= check_commands(1, 0);
        struct rect_call *r = &g_rects[0];
        const s32 *ir = k_rects[v];
        if (g_rt_rec[0].tex != aort->target || !g_rt_rec[0].barrier || r->sw != BW || r->sh != BH ||
            r->w != ir[2] - ir[0] || r->h != ir[3] - ir[1])
          printf("FAIL: the AO draw's target or sizes\n"), bad = 1;
        u64 x[64]; u8 *c[64];
        int n = list_cmds(x, c, 64);
        for (int i = 0; i < n; i++) {
          if (x[i] == X_PS_TEXTURE && *(u64 *)(c[i] + 0x20) != aort->srv)
            printf("FAIL: the AO draw's source\n"), bad = 1;
          if (x[i] == X_VIEWPORT && (*(s32 *)(c[i] + 0x10) != ir[0] || *(s32 *)(c[i] + 0x14) != ir[1]))
            printf("FAIL: the AO draw's viewport\n"), bad = 1;
        }
        run_draw(r, ir[0], ir[1], ao, BW, ao, 0, 1);
      }
    }
    /* each inset pixel reads its own texel; the eye's AO at its direction (jittered: AO is made
     * from the frame as drawn) */
    for (int wqi = 0; wqi < 2; wqi++) {
      g_wq = wqi;
      for (int e = 0; e < 2; e++) {
        const s32 *ir = k_rects[2 + e];
        double maxerr = 0, sq = 0;
        long nn = 0;
        for (int y = ir[1]; y < ir[3]; y++)
          for (int x = ir[0]; x < ir[2]; x++) {
            double truth = bilinear(ao_eye, BW, BH, truth_to_eye(e, 0, x + 0.5, 1), truth_to_eye(e, 1, y + 0.5, 1));
            double d = fabs(ao[y * BW + x] - truth);
            maxerr = d > maxerr ? d : maxerr, sq += d * d, nn++;
          }
        printf("  AO, inset %d%s: the eye's at the inset's pixels, worst %.5f (%.2f of 1/255), rms %.6f\n", e,
               wqi ? " (8-bit filter weights)" : "", maxerr, maxerr * 255.0, sqrt(sq / nn));
        if (maxerr > (wqi ? 2.0 : 0.51) / 255.0)
          printf("FAIL: inset %d's AO off the eye's by %.5f\n", e, maxerr), bad = 1;
      }
    }
    g_wq = 0;
    /* the eyes' own AO untouched */
    long changed = 0;
    for (int y = 0; y < 1620; y++)
      for (int x = 0; x < BW; x++)
        changed += ao[y * BW + x] != ao_eye[y * BW + x];
    if (changed)
      printf("FAIL: %ld of the eyes' AO texels changed\n", changed), bad = 1;
    /* the jitter: mapped without it, the AO would sit up to a texel off */
    double off = fabs(truth_to_eye(0, 0, 500.5, 1) - truth_to_eye(0, 0, 500.5, 0));
    printf("  (the eye's jitter moves the AO's place %.3f of its texel: AO mapped with it, bloom without)\n", off);
  }
  printf(bad ? "FAIL\n" : "ok\n");
  return bad;
}
