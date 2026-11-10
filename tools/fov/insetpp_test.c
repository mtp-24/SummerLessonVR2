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
u32 mv_at(u32 h) { return h; } /* (multiview.c: Hikari's own addresses) */
volatile u32 g_opt_insetaam = 2; /* (multiview.c's, in the report line) */
int agent_snprintf(char *b, size_t n, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(b, n, f, ap); va_end(ap); return r; }
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
static int g_ssr_calls;
static void f_ssr(u64 p, u64 c) { (void)p, (void)c; g_ssr_calls++; }
/* a reflections trace pass: its own output 0 (GetOutput, vtable +0x40) */
struct fake_ssr {
  u64 vt;
  u8 out[0x58];
};
static u64 ssr_get_output(u64 p, u32 id) { (void)id; return (u64)((struct fake_ssr *)p)->out; }
static u64 k_ssr_vt[9];
static float fmt_round(float x);
/* the eye's reflections: sharp-edged shapes over a soft gradient (a value a buffer pixel) */
static void fill_ssr(float *t, int w, int h, int e) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      double v = 0.1 + 0.05 * sin(x * 0.021 + e) * cos(y * 0.017);
      if (((x / 7) + (y / 5)) % 9 == 0)
        v += 0.6; /* thin bright bars (a window's frame reflected) */
      v += 0.8 * exp(-(pow(x - 700.0 - 1440.0 * e, 2) + pow(y - 820.0, 2)) / 50.0);
      t[y * w + x] = fmt_round((float)v);
    }
}
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

/* ---- the insets' TAA: the band round each inset's history, and the matrix check ---- */
/* the eye's TAA output: fine detail and edges (a value a buffer pixel) */
static void fill_taa(float *t, int w, int h, int e) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      double v = 0.3 + 0.2 * sin(x * 0.05 + e) * cos(y * 0.031);
      if (((x + 2 * y) / 9) % 7 == 0)
        v += 0.9; /* thin diagonal bars */
      v += 1.5 * exp(-(pow(x - 600.0 - 1440.0 * e, 2) + pow(y - 900.0, 2)) / 30.0);
      t[y * w + x] = fmt_round((float)v);
    }
}
static int taa_tests(void) {
  int bad = 0;
  static u8 st[4][0x700];
  struct fake_rt *out[4];
  float *H[4];
  for (int v = 0; v < 4; v++) {
    memset(st[v], 0, sizeof st[v]);
    out[v] = new_rt(BW, BH, 0xb00 + 0x10 * (u64)v);
    /* this frame's output: in Pending (r.TemporalAAPauseCorrect) for the left eye and the right
     * inset, straight in the history for the others (and a stale history beside the Pending) */
    if (v == 0 || v == 3) {
      *(u64 *)(st[v] + 0x670) = (u64)out[v];
      *(u64 *)(st[v] + 0x668) = (u64)new_rt(BW, BH, 0xc00 + 0x10 * (u64)v);
    } else {
      *(u64 *)(st[v] + 0x668) = (u64)out[v];
    }
    *(u64 *)(g_view[v] + 0xd10) = (u64)st[v];
    g_view[v][0xb0c] = 2;
    H[v] = malloc(sizeof(float) * BW * BH);
    if (v < 2)
      fill_taa(H[v], BW, BH, v);
    else
      for (long i = 0; i < (long)BW * BH; i++)
        H[v][i] = NAN; /* (outside the rect: the engine's black; NaN shows any texel written) */
  }
  for (int i = 2; i < 4; i++) /* the inset's own TAA in its rect */
    for (int y = k_rects[i][1]; y < k_rects[i][3]; y++)
      for (int x = k_rects[i][0]; x < k_rects[i][2]; x++)
        H[i][y * BW + x] = 5.0f;
  *(s32 *)(g_sc + 0x210) = BW, *(s32 *)(g_sc + 0x214) = BH;
  g_opt_insetpp = 0, g_opt_insettaa = 1, g_quiet = 1;
  static const s32 k_band[2][4] = {{0, 1556, 1264, 2220}, {1136, 1556, 2464, 2220}};
  static const int k_draws[2] = {2, 3}; /* (the left inset at the buffer's left edge; both at its bottom) */
  for (int v = 0; v < 4; v++) {
    reset_list();
    ctx_for(v, (u64)&g_tm[v]);
    int calls = g_tm_calls;
    mv_tm((u64)&g_tm[v], (u64)g_ctx);
    if (g_tm_calls != calls + 1)
      printf("FAIL: TAA: view %d: the tonemapper not run once\n", v), bad = 1;
    if (v < 2) {
      if (g_rect_n)
        printf("FAIL: TAA: eye %d drew\n", v), bad = 1;
      continue;
    }
    int i = v - 2;
    if (g_rect_n != k_draws[i]) {
      printf("FAIL: TAA: inset %d: %d band draws, %d expected\n", i, g_rect_n, k_draws[i]), bad = 1;
      continue;
    }
    bad |= check_commands(g_rect_n, 0);
    u64 x[64]; u8 *c[64];
    int n = list_cmds(x, c, 64), d = 0;
    for (int j = 0; j < n; j++) {
      if (x[j] == X_PS_TEXTURE && *(u64 *)(c[j] + 0x20) != out[i]->srv)
        printf("FAIL: TAA: inset %d's band not from its eye's output\n", i), bad = 1;
      if (x[j] == X_PS_SAMPLER && *(u64 *)(c[j] + 0x20) != 0x5b1)
        printf("FAIL: TAA: inset %d's band not bilinear\n", i), bad = 1;
      if (x[j] != X_VIEWPORT)
        continue;
      s32 vx0 = *(s32 *)(c[j] + 0x10), vy0 = *(s32 *)(c[j] + 0x14);
      struct rect_call *r = &g_rects[d];
      if (g_rt_rec[d].tex != out[v]->target || !g_rt_rec[d].barrier || r->sw != BW || r->sh != BH)
        printf("FAIL: TAA: inset %d's draw %d: its target or source size\n", i, d), bad = 1;
      run_draw(r, vx0, vy0, H[v], BW, H[i], 0, 0);
      d++;
    }
    /* every band texel the eye's TAA output at its direction (no jitter: the TAA's output), the
     * rect itself untouched, nothing past the band */
    const s32 *ir = k_rects[v], *bb = k_band[i];
    double maxsteps = 0;
    long inside = 0, past = 0, unwritten = 0, nb = 0;
    for (int y = 0; y < BH; y++)
      for (int xx = 0; xx < BW; xx++) {
        float got = H[v][y * BW + xx];
        int in_rect = xx >= ir[0] && xx < ir[2] && y >= ir[1] && y < ir[3];
        int in_band = !in_rect && xx >= bb[0] && xx < bb[2] && y >= bb[1] && y < bb[3];
        if (in_rect) {
          inside += got != 5.0f;
        } else if (!in_band) {
          past += got == got;
        } else if (got != got) {
          unwritten++;
        } else {
          float truth = fmt_round(bilinear(H[i], BW, BH, truth_to_eye(i, 0, xx + 0.5, 0),
                                           truth_to_eye(i, 1, y + 0.5, 0)));
          int ex;
          frexpf(truth, &ex);
          double steps = fabs(got - truth) / ldexp(1.0, ex - 7);
          maxsteps = steps > maxsteps ? steps : maxsteps, nb++;
        }
      }
    printf("  TAA, inset %d: its history's band (%ld texels) the eye's TAA output at their directions: "
           "worst %.2f of a rounding step; %ld inside the rect changed, %ld past the band written, %ld "
           "band texels unwritten\n", i, nb, maxsteps, inside, past, unwritten);
    if (maxsteps > 1.0001 || inside || past || unwritten || nb == 0)
      printf("FAIL: TAA: inset %d's band\n", i), bad = 1;
  }
  /* no band: the inset's AA not TAA, the option off, its eye without a TAA output */
  struct { const char *what; int k; } k_no[3] = {{"FXAA in the inset", 0}, {"agent.insettaa=0", 1},
                                                {"no eye output", 2}};
  for (int t = 0; t < 3; t++) {
    if (k_no[t].k == 0)
      g_view[2][0xb0c] = 1;
    if (k_no[t].k == 1)
      g_opt_insettaa = 0;
    u64 keep = *(u64 *)(st[0] + 0x670), keep2 = *(u64 *)(st[0] + 0x668);
    if (k_no[t].k == 2)
      *(u64 *)(st[0] + 0x670) = 0, *(u64 *)(st[0] + 0x668) = 0;
    reset_list();
    ctx_for(2, (u64)&g_tm[2]);
    mv_tm((u64)&g_tm[2], (u64)g_ctx);
    if (g_rect_n)
      printf("FAIL: TAA: a band drawn with %s\n", k_no[t].what), bad = 1;
    g_view[2][0xb0c] = 2, g_opt_insettaa = 1;
    *(u64 *)(st[0] + 0x670) = keep, *(u64 *)(st[0] + 0x668) = keep2;
  }
  /* the matrix check: the inset's previous-frame projection against what it was made with */
  {
    memset(&g_tc, 0, sizeof g_tc);
    u8 *v = g_view[2];
    float *cur = (float *)(v + 0x70), *prev = (float *)(v + 0x17f0), *pj = (float *)(v + 0x17f0 + 0x174);
    float base[16], made[4][16];
    memcpy(base, cur, 64);
    for (int f = 0; f < 4; f++) { /* the window 5 pixels further right each frame */
      memcpy(made[f], base, 64);
      made[f][8] += (float)f * 5.0f / 600.0f;
    }
    const float jit[2] = {0.0004f, -0.0003f};
    /* frame 0: seen; 1: prev = frame 0's with its jitter (right); 2: prev = its own (a reset);
     * 3: prev = frame 0's (stale) */
    const float cjit[4][2] = {{0.0003f, 0.0001f}, {-0.0002f, 0.0004f}, {0.0001f, -0.0003f}, {0.0004f, 0.0002f}};
    for (int f = 0; f < 4; f++) { /* (the current projection still jittered, as on the console) */
      memcpy(cur, made[f], 64);
      cur[8] += cjit[f][0], cur[9] += cjit[f][1];
      memcpy(v + 0x1e4, cjit[f], 8);
      const float *p = f == 1 ? made[0] : f == 2 ? made[2] : made[0];
      memcpy(prev, p, 64);
      prev[8] += jit[0], prev[9] += jit[1], pj[0] = jit[0], pj[1] = jit[1];
      taa_matrices((u64)v, 2);
    }
    if (g_tc.frames[1] != 3 || g_tc.moved[1] != 3 || g_tc.ok[1] != 1 || g_tc.reset[1] != 1 ||
        g_tc.other[1] != 1 || g_tc.move_px[1] < 14.9f || g_tc.move_px[1] > 15.1f)
      printf("FAIL: TAA: the matrix check (%u frames, %u moved %.2f px, %u right, %u reset, %u other)\n",
             g_tc.frames[1], g_tc.moved[1], (double)g_tc.move_px[1], g_tc.ok[1], g_tc.reset[1], g_tc.other[1]), bad = 1;
    else
      printf("  TAA: the matrix check tells the frame before's (with its jitter), a reset and a stale one apart\n");
    memcpy(cur, base, 64);
    memset(v + 0x1e4, 0, 8);
  }
  g_quiet = 0;
  return bad;
}

/* ---- our TAA (agent.taa): the engine's TAA shader found, checked, copied, switched ---- */
static u8 *g_taa_alloc;
static void *f_dalloc(u64 n) {
  free(g_taa_alloc); /* (one base at a time) */
  return g_taa_alloc = aligned_alloc(0x10000, (n + 0xffff) & ~0xffffull);
}
/* one base: the block as dumped (logs/taa/aa4-0.bin Hikari's PS10 with build 1, logs/taa/pop10-4.bin
 * Chisato's PS40 with build 2, and with build 4 for Hikari 1.02), the shader's static type for the map */
static int our_taa_base_tests(const char *path, u32 build, u32 type) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return printf("FAIL: our TAA: %s not readable\n", path), 1;
  static u8 blk_mem[0x1000] __attribute__((aligned(16)));
  u8 *blk = blk_mem;
  memset(blk, 0, sizeof blk_mem);
  size_t n = fread(blk, 1, sizeof blk_mem, f);
  fclose(f);
  int bad = 0;
  const struct taa_base *base = &k_taa_base[build >= 2]; /* (2-4: the newer engine) */
  const u32 TAA_CODE_OFF = base->code_off, TAA_CODE_LEN = base->code_len;
  memset(&g_taa, 0, sizeof g_taa);
  if (n < TAA_CODE_OFF + TAA_CODE_LEN)
    return printf("FAIL: our TAA: the block is %zu bytes\n", n), 1;
  /* the shader map with the TAA shader's type: FShader +0x48 resource, +0x40 RHI, +0x30 block */
  static u8 map[0x80], elems[24], sh[0x100], res[0x100], rhi[0x100];
  static s32 hash[8];
  memset(hash, 0xff, sizeof hash);
  u64 key = g_base + type;
  *(u64 *)elems = key, *(u64 *)(elems + 8) = (u64)sh, *(s32 *)(elems + 16) = -1;
  hash[hash_combine((u32)(key >> 4), 0) & 7] = 0;
  *(u64 *)(map + 0x30) = (u64)elems, *(s32 *)(map + 0x38) = 1, *(u64 *)(map + 0x70) = (u64)hash, *(s32 *)(map + 0x78) = 8;
  *(u64 *)(sh + 0x48) = (u64)res, *(u64 *)(res + 0x40) = (u64)rhi, *(u64 *)(rhi + 0x30) = (u64)blk;
  astro_hdr.build = build;
  g_e.dalloc = f_dalloc;
  u8 stock[0x1000];
  memcpy(stock, blk + TAA_CODE_OFF, TAA_CODE_LEN);
  u32 *regs = (u32 *)(blk + TAA_PSSHADER_OFF + 8), lo0 = regs[0], rsrc0 = regs[2];
  if (!plausible((u64)sh) || !plausible((u64)blk)) {
    /* (plausible() is the game's heap's range: where the host's addresses fall outside it, the
     * lookup is skipped and the state set as taa_init would) */
    printf("  our TAA: the host's addresses are outside plausible()'s range: the lookup not tested, the rest is\n");
    g_taa.regs = regs, g_taa.lo0 = regs[0], g_taa.hi0 = regs[1], g_taa.rsrc0 = regs[2], g_taa.base = base;
    g_taa.buf = f_dalloc(0x1000 * (TAA_NVAR + 3) + (build >= 2 ? TAA_WEIGHTS40 : 0));
    memset(g_taa.buf + 0x1000 * TAA_SLOTS_PAGE, 0, 0x2000);
    for (u32 v = 1; v <= TAA_NVAR; v++)
      taa_build(g_taa.buf + 0x1000 * v, blk + TAA_CODE_OFF, v);
    g_taa.ok = 1;
  }
  g_quiet = getenv("TAA_LOG") == 0;
  g_opt_taa = 1;
  taa_step((u64)map, 0, 0);
  if (!g_taa.ok)
    return printf("FAIL: our TAA: not set up from the block\n"), 1;
  /* every variant: the stock code but at its patches */
  for (u32 v = 1; v <= TAA_NVAR; v++) {
    const u8 *cv = g_taa.buf + 0x1000 * v;
    long diff = 0;
    for (u32 i = 0; i < TAA_CODE_LEN; i++) {
      int patched = 0;
      for (u32 k = 0; k < taa_var(v)->n; k++)
        patched |= i >= taa_var(v)->p[k].at && i < (u32)taa_var(v)->p[k].at + taa_var(v)->p[k].len;
      diff += !patched && cv[i] != stock[i];
    }
    for (u32 k = 0; k < taa_var(v)->n; k++) /* (the probe's placeholders, 0xd1a90001-04, filled in) */
      for (u32 b = 0; b < taa_var(v)->p[k].len; b += 4) {
        u32 want, got;
        memcpy(&want, taa_var(v)->p[k].bytes + b, 4);
        memcpy(&got, cv + taa_var(v)->p[k].at + b, 4);
        diff += (want >> 4) == (0xd1a90001u >> 4) && (want & 15) <= 8 ? got == want : got != want;
      }
    if (diff)
      printf("FAIL: our TAA: variant %u (%ld bytes off)\n", v, diff), bad = 1;
  }
  if (memcmp(g_taa.buf + 0x1000, stock, TAA_CODE_LEN) || memcmp(blk + TAA_CODE_OFF, stock, TAA_CODE_LEN))
    printf("FAIL: our TAA: variant 1 not the stock code, or the stock code touched\n"), bad = 1;
  /* a walk through the variants: the larger VGPR count always a frame ahead of code that needs it */
  static const u32 walk[] = {2, 6, 4, 5, 1, 3, 7, 11, 13, 14, 15, 9, 12, 10, 8, 15, 14, 13, 0};
  for (u32 w = 0; w < sizeof walk / sizeof *walk; w++) {
    g_opt_taa = walk[w];
    u32 from = g_taa.cur;
    for (int step = 0; step < 4; step++) {
      taa_step((u64)map, 0, 0);
      u64 at = (u64)regs[1] << 40 | (u64)regs[0] << 8;
      u32 running = at == ((u64)g_taa.hi0 << 40 | (u64)g_taa.lo0 << 8) ? 0 : (u32)((at - (u64)g_taa.buf) / 0x1000);
      u32 need = running ? taa_var(running)->rsrc1 & 0x3f : rsrc0 & 0x3f;
      u32 needs = running ? taa_var(running)->rsrc1 & 0x3c0 : rsrc0 & 0x3c0;
      if ((regs[2] & 0x3f) < need || (regs[2] & 0x3c0) < needs)
        printf("FAIL: our TAA: %u -> %u: variant %u running with RSRC1 0x%x\n", from, walk[w], running, regs[2]), bad = 1;
    }
    u32 rs = walk[w] ? taa_var(walk[w])->rsrc1 : rsrc0;
    if (g_taa.cur != walk[w] || regs[2] != rs)
      printf("FAIL: our TAA: %u -> %u not reached (RSRC1 0x%x)\n", from, walk[w], regs[2]), bad = 1;
  }
  if (regs[0] != lo0 || regs[2] != rsrc0)
    printf("FAIL: our TAA: not back to the engine's\n"), bad = 1;
  /* the views' thresholds (the left eye's height and right edge) set again when its rect changes */
  if (g_taa.rect_at[10][0] && g_taa.rect_at[10][1]) { /* (the probe tells the views apart in both bases) */
    static u8 eye[0x80];
    static const s32 rects[2][4] = {{0, 0, 1440, 1080}, {0, 0, 960, 1080}};
    for (int t = 0; t < 2; t++) {
      memcpy(eye + 0x38, rects[t], 16);
      taa_step((u64)map, 0, (u64)eye);
      for (u32 v = 1; v <= TAA_NVAR; v++)
        for (u32 k = 0; k < 2 && g_taa.rect_at[v][k]; k++) {
          float got;
          memcpy(&got, g_taa.buf + 0x1000 * v + g_taa.rect_at[v][k], 4);
          if (got != (float)rects[t][3 - k])
            printf("FAIL: our TAA: variant %u's threshold %u %.0f, the eye's rect says %d\n", v, k, (double)got, rects[t][3 - k]), bad = 1;
        }
    }
  } else
    printf("FAIL: our TAA: the probe (10) without the views' thresholds\n"), bad = 1;
  /* the newer engine's 13: an inset within a plane of its weights runs 13, a larger one 11; the
   * weights cleared as 13 comes in */
  if (build >= 2) {
    static u8 inset[0x100];
    s32 *ir = (s32 *)(inset + 0x38);
    u8 *wts = g_taa.buf + 0x1000 * (TAA_NVAR + 3);
    for (int big = 0; big < 2; big++) {
      ir[0] = 0, ir[1] = 1620, ir[2] = big ? 1800 : 1200, ir[3] = big ? 2320 : 2220; /* (1800x700: 315000 blocks) */
      g_opt_taa = 0;
      for (int f = 0; f < 4; f++)
        taa_step((u64)map, (u64)inset, 0);
      memset(wts, 0x5a, TAA_WEIGHTS40);
      g_opt_taa = 13;
      for (int f = 0; f < 4; f++)
        taa_step((u64)map, (u64)inset, 0);
      u32 want = big ? 11 : 13;
      if (g_taa.cur != want)
        printf("FAIL: our TAA: a %dx%d inset ran %u, not %u\n", ir[2] - ir[0], ir[3] - ir[1], g_taa.cur, want), bad = 1;
      if (!big && (wts[0] || wts[TAA_WEIGHTS40 - 1]))
        printf("FAIL: our TAA: 13's weights not cleared as it came in\n"), bad = 1;
    }
    g_opt_taa = 0;
    for (int f = 0; f < 4; f++)
      taa_step((u64)map, 0, 0);
  }
  /* a block whose code differs: refused, nothing written */
  memset(&g_taa, 0, sizeof g_taa);
  blk[TAA_CODE_OFF + 100] ^= 1;
  g_opt_taa = 2;
  if (plausible((u64)sh)) {
    taa_step((u64)map, 0, 0);
    if (!g_taa.failed || regs[0] != lo0 || regs[2] != rsrc0)
      printf("FAIL: our TAA: a different shader not refused\n"), bad = 1;
  }
  blk[TAA_CODE_OFF + 100] ^= 1;
  g_opt_taa = 0;
  memset(&g_taa, 0, sizeof g_taa);
  g_quiet = 0;
  if (!bad)
    printf("  our TAA (%s, build %u): found and checked, %d variants built (each only its patches' bytes off the stock "
           "code), switched through all with the VGPR count always a frame ahead; the views' thresholds follow the eye's rect\n", base->name, build, TAA_NVAR);
  return bad;
}
static int our_taa_tests(void) {
  /* (the dumped blocks: not in the repo) */
  const char *hikari = getenv("TAA_BLOCK"), *ps40 = getenv("TAA_BLOCK40");
  int bad = 0;
  if (hikari)
    bad |= our_taa_base_tests(hikari, 1, 0x49f0430);
  else
    printf("  our TAA: Hikari's skipped (TAA_BLOCK=<the dumped block, logs/taa/aa4-0.bin> to run it)\n");
  if (ps40) {
    bad |= our_taa_base_tests(ps40, 2, 0x4a94af0);
    /* Hikari 1.02, the newer engine: its PS40 taken as Chisato's code (the same block; on the console
     * the FNV check tells whether it is), at its own static type */
    bad |= our_taa_base_tests(ps40, 4, 0x49e09b0);
  } else
    printf("  our TAA: Chisato's and Allison's skipped (TAA_BLOCK40=<the dumped block, logs/taa/pop10-4.bin> to run it)\n");
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
  o_tm = f_tm, o_ao = f_ao, o_ssr = f_ssr;
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
  /* ---- reflections: the four views' traces, the eyes' copied 1:1 into S, the insets' drawn from it ---- */
  {
    k_ssr_vt[8] = (u64)ssr_get_output;
    static struct fake_ssr sp[4];
    float *E[4], *Sb[2] = {0, 0};
    struct fake_rt *srt[4], *S[2] = {0, 0};
    g_opt_insetpp = 7;
    for (int v = 0; v < 4; v++) {
      sp[v].vt = (u64)k_ssr_vt;
      srt[v] = new_rt(BW, BH, 0x900 + 0x10 * (u64)v);
      memset(sp[v].out, 0, sizeof sp[v].out);
      *(s32 *)(sp[v].out + 0x14) = BW, *(s32 *)(sp[v].out + 0x18) = BH, *(s32 *)(sp[v].out + 0x2c) = 10;
      *(u64 *)(sp[v].out + 0x50) = (u64)srt[v];
      E[v] = malloc(sizeof(float) * BW * BH);
      if (v < 2)
        fill_ssr(E[v], BW, BH, v);
      else
        for (long i = 0; i < (long)BW * BH; i++)
          E[v][i] = 9.0f; /* (the inset's own trace: all of its rect must be drawn over) */
    }
    float *E0[2];
    for (int e = 0; e < 2; e++) {
      E0[e] = malloc(sizeof(float) * BW * BH);
      memcpy(E0[e], E[e], sizeof(float) * BW * BH);
    }
    for (int v = 0; v < 4; v++) {
      reset_list();
      ctx_for(v, (u64)&sp[v]);
      int c0 = g_ssr_calls, p0 = g_pool_n;
      mv_ssr((u64)&sp[v], (u64)g_ctx);
      if (g_ssr_calls != c0 + 1)
        printf("FAIL: view %d: the engine's trace not run once\n", v), bad = 1;
      if (g_rect_n != 1) {
        printf("FAIL: view %d: %d reflection draws\n", v, g_rect_n), bad = 1;
        continue;
      }
      bad |= check_commands(1, 0);
      struct rect_call *r = &g_rects[0];
      u64 x[64]; u8 *c[64];
      int n = list_cmds(x, c, 64);
      u64 tex = 0, samp = 0;
      for (int i = 0; i < n; i++) {
        if (x[i] == X_PS_TEXTURE)
          tex = *(u64 *)(c[i] + 0x20);
        if (x[i] == X_PS_SAMPLER)
          samp = *(u64 *)(c[i] + 0x20);
      }
      if (v < 2) { /* S asked for once, the block point-copied from the eye's own output */
        if (g_pool_n != p0 + 1 || g_rt_rec[0].tex != g_pool_last->target || tex != srt[v]->srv ||
            samp != 0x5b0 || r->sw != BW || r->sh != BH || g_pool_fmt != 10)
          printf("FAIL: eye %d's copy: its target, source, sampler or format\n", v), bad = 1;
        S[v] = g_pool_last;
        Sb[v] = calloc((size_t)S[v]->w * S[v]->h, sizeof(float));
        for (long i = 0; i < (long)S[v]->w * S[v]->h; i++)
          Sb[v][i] = NAN; /* (a texel read with any weight before it is written shows) */
        run_draw(r, 0, 0, Sb[v], S[v]->w, E[v], 1, 0);
      } else { /* the inset's own output, its rect, from S (bilinear) */
        int e = v - 2;
        const s32 *ir = k_rects[v];
        if (g_pool_n != p0 || g_rt_rec[0].tex != srt[v]->target || tex != S[e]->srv || samp != 0x5b1 ||
            r->sw != S[e]->w || r->sh != S[e]->h || r->w != ir[2] - ir[0] || r->h != ir[3] - ir[1])
          printf("FAIL: inset %d's draw: its target, source, sampler or sizes\n", e), bad = 1;
        run_draw(r, ir[0], ir[1], E[v], BW, Sb[e], 0, 0);
      }
    }
    for (int e = 0; e < 2; e++) {
      const s32 *ir = k_rects[2 + e];
      /* (each sample is rounded to the format: a sample within float error of a rounding boundary
       * can land one step either side, so a difference of more than one step is the failure) */
      double maxsteps = 0;
      long nan_n = 0, flips = 0, nn = 0;
      for (int y = ir[1]; y < ir[3]; y++)
        for (int x = ir[0]; x < ir[2]; x++) {
          float truth = fmt_round(bilinear(E0[e], BW, BH, truth_to_eye(e, 0, x + 0.5, 1), truth_to_eye(e, 1, y + 0.5, 1)));
          float got = E[2 + e][y * BW + x];
          if (got != got)
            nan_n++;
          int ex;
          frexpf(truth, &ex);
          double steps = fabs(got - truth) / ldexp(1.0, ex - 7);
          maxsteps = steps > maxsteps ? steps : maxsteps, flips += steps > 0, nn++;
        }
      printf("  reflections, inset %d: the eye's at the inset's pixels through S (%dx%d): worst %.2f of a "
             "rounding step, %ld of %ld one step off, %ld unwritten texels read\n",
             e, S[e] ? S[e]->w : 0, S[e] ? S[e]->h : 0, maxsteps, flips, nn, nan_n);
      if (maxsteps > 1.0001 || nan_n || flips > nn / 1000)
        printf("FAIL: inset %d's reflections off the eye's\n", e), bad = 1;
      long changed = 0;
      for (long i = 0; i < (long)BW * BH; i++)
        changed += E[e][i] != E0[e][i];
      if (changed)
        printf("FAIL: eye %d's own reflections changed (%ld)\n", e, changed), bad = 1;
    }
    /* the next frame: S kept (no new target asked for), and nothing for an inset whose eye's copy
     * didn't come this frame */
    int p1 = g_pool_n;
    for (int v = 0; v < 2; v++) {
      reset_list(), ctx_for(v, (u64)&sp[v]);
      mv_ssr((u64)&sp[v], (u64)g_ctx);
    }
    reset_list(), ctx_for(2, (u64)&sp[2]);
    mv_ssr((u64)&sp[2], (u64)g_ctx);
    int n2 = g_rect_n;
    reset_list(), ctx_for(2, (u64)&sp[2]);
    mv_ssr((u64)&sp[2], (u64)g_ctx); /* (twice: the copy used once) */
    if (g_pool_n != p1 || n2 != 1 || g_rect_n != 0)
      printf("FAIL: the second frame: %d targets asked for, %d and %d draws\n", g_pool_n - p1, n2, g_rect_n), bad = 1;
    /* agent.insetpp without the reflections: S let go */
    g_opt_insetpp = 3;
    reset_list(), ctx_for(0, (u64)&sp[0]);
    mv_ssr((u64)&sp[0], (u64)g_ctx);
    if (g_rect_n || S[0]->refs != 1 || S[1]->refs != 1)
      printf("FAIL: S not let go with the reflections off (%d %d)\n", S[0]->refs, S[1]->refs), bad = 1;
  }
  bad |= taa_tests();
  bad |= our_taa_tests();
  printf(bad ? "FAIL\n" : "ok\n");
  return bad;
}
