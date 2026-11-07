/* Multi-view step 4 (docs/MULTIVIEW.md): the inset views as Hmd2's second layer. Layer 1 is the
 * game's own texture with the insets' rects as its viewports and their angles, in straight alpha;
 * Hmd2 doesn't clip a layer to its window, so the texture's alpha (which layer 0 doesn't read) is
 * written here for each frame before it goes to the headset: 0 everywhere but the insets, a ramp
 * from 0 at their edge to 1 INSET_RAMP texels in (tools/gpu/inset.c). Inset mode
 * (agent.multiview=N, agent.insetviews=2).
 *
 * One compute buffer on Hmd2's queue a frame (acb.c), held on the queue until its stores
 * are in memory, so Hmd2's work submitted after it reads them. It goes from the TV mirror's
 * thread as soon as the frame's own label is written (PS VR's "texture ready"): at the
 * hand-over the game has only submitted the frame, and its drawing (the alpha too) went over a
 * pass made then; after the "drawn" labels the TV waits for, Hmd2 had already shown the frame
 * (2026-11-07, agent.insetdebug). The game times its frames to be done about 1.5 ms before the
 * first refresh that shows them; the pass takes ~0.04 ms. Its copy of the game's T# has the
 * read-only memory type cleared (stores through it are dropped). Each texel is read and written
 * back whole with its alpha; vr2.c sends layer 1 for a frame the pass was given.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

#include "inset_gcn.h"

#define NSLOT 2
#define SHADER_SPACE 0x4000
#define INSET_RAMP 40.0f
_Static_assert(sizeof k_inset_gcn <= SHADER_SPACE, "the inset shader outgrew its space");

/* struct inset_k in tools/gpu/inset.c, the same layout (the pads: fields since removed, the offsets
 * kept) */
struct consts {
  u32 tex[8];
  s32 rect[2][4];
  s32 w, h;
  float ramp;
  s32 debug, axis[2][2];
  float ppt[4]; /* pixels a unit of tangent: the periphery's across and up, the insets' */
  s32 pad0, stage, pad1, oy; /* stage 2: the menu; oy: the pass's first row (the insets' rows only
                              * with the window test) */
  s32 pad6[10];
  float gaze[2]; /* agent.gazedot: the gaze's tangents */
  s32 gazedot, pad2[3];
  s32 pad7[8];
  float menu[4]; /* the debug menu's place in tangent (the insets give way under it) */
  s32 menuon;
  float menu_dx; /* the menu's shift in each eye (its stereo: the left eye's +, the right's -) */
  s32 pad3[2];
  u32 mtex[8];   /* stage 2: the menu's picture */
  s32 mat[2][2]; /* stage 2: its top-left in each eye */
  s32 mw, mh, pad4[2];
  float ictan[4]; /* each inset's window centre in tangent (multiview_centres) */
  s32 pad5[4];
};
_Static_assert(sizeof(struct consts) == 352, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, ictan) == 320, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, mtex) == 256, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, menu) == 224, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, gaze) == 168, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, stage) == 116, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, w) == 64, "inset constants");

struct __attribute__((aligned(256))) slot {
  struct consts k;
  u32 cb[128];
  volatile u32 label; /* seq * 8 + 4 when done */
  u32 pad;
  volatile u64 ts[2]; /* the GPU's clock at the start and the end */
};

static struct {
  u8 *mem; /* the shader, then the slots */
  u32 seq, accounted, failed, off;
  u64 report_at, cal_cpu, cal_gpu, done_cpu, done_gpu;
  u32 n, n_gpu, skipped_busy, skipped_odd;
  u64 s_gpu, gpu_max;
} g_i;

static struct slot *slot(u32 i) { return (struct slot *)(g_i.mem + SHADER_SPACE) + i; }

static u32 build(struct slot *s, u32 seq, u32 w, u32 h) {
  u32 *cb = s->cb, n = 0;
  u64 shader = (u64)g_i.mem, k = (u64)&s->k;
  n = acb_acquire(cb, n, 1); /* the constants and the commands were written by the CPU */
  cb[n++] = acb_pm4(0x76, 4), cb[n++] = 0x204, cb[n++] = 0, cb[n++] = 0, cb[n++] = 0; /* START */
  cb[n++] = acb_pm4(0x76, 4), cb[n++] = 0x207, cb[n++] = 8, cb[n++] = 8, cb[n++] = 1; /* NUM_THREAD */
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x20c; /* PGM_LO/HI */
  cb[n++] = (u32)(shader >> 8), cb[n++] = (u32)(shader >> 40);
  u32 rsrc1 = ((K_INSET_GCN_VGPRS - 1) / 4) | (((K_INSET_GCN_SGPRS - 1) / 8) << 6) |
              (0xc0u << 12) | (1u << 21);
  u32 rsrc2 = (2u << 1) | (1u << 7) | (1u << 8) | (1u << 11);
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x212, cb[n++] = rsrc1, cb[n++] = rsrc2;
  /* (the timestamps without cache actions: the barrier after the dispatch writes our stores back
   * for Hmd2's work, and the timestamps with them) */
  n = acb_timestamp(cb, n, (u64)&s->ts[0]);
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x240, cb[n++] = (u32)k, cb[n++] = (u32)(k >> 32);
  cb[n++] = acb_pm4(0x15, 4);
  cb[n++] = (w + 7) / 8, cb[n++] = (h + 7) / 8, cb[n++] = 1, cb[n++] = 0x41;
  n = acb_timestamp(cb, n, (u64)&s->ts[1]);
  return acb_barrier(cb, n, (u64)&s->label, seq * 8 + 4);
}

#ifndef RELEASE
/* Once, before the first pass (debug builds only: 1 MB of the game's direct memory, a wait of
 * up to 50 ms): the shader's stores tried on two textures of our own, read back by the
 * processor. The game's texture never showed agent.insetdebug's magenta (2026-11-07, even
 * to the mirror's copy right after the pass on the same queue), and image stores from this queue
 * had never been seen to land: a linear 64x64 texture, and one tiled as the game's (its T# with
 * our base, 256x256, the pass over 64x64 of it). Logged: whether the stores landed. */
static void self_test(const u32 *gt) {
  enum { NT = 4, SZ = 0x40000 };
  u8 *m = vr2_dalloc(NT * SZ + 0x10000, 0x10000);
  if (!m) {
    LOG("inset: self-test: no memory");
    return;
  }
  struct consts *k = (struct consts *)(m + NT * SZ);
  u32 *cb = (u32 *)(m + NT * SZ + 0x1000);
  volatile u32 *label = (volatile u32 *)(m + NT * SZ + 0x2000);
  static const char *const what[NT] = {"linear", "the game's T#, word 3 bit 26 clear",
                                       "the game's, bit 26 clear, words 6-7 0",
                                       "the game's, linear tiling, bit 26 clear"};
  memset(m, 0x11, NT * SZ);
  *label = 0;
  memset(k, 0, NT * sizeof *k);
  for (int i = 0; i < NT; i++) {
    u64 a = (u64)(m + i * SZ);
    u32 *t = k[i].tex;
    if (i == 0) {
      t[1] = 10u << 20; /* 8_8_8_8 UNORM */
      t[2] = 63u | (63u << 14);
      t[3] = 0xfacu | (8u << 20) | (9u << 28); /* x y z w; linear aligned; 2D */
      t[4] = (64u - 1) << 13;
    } else {
      memcpy(t, gt, 32);
      t[1] &= ~(0xfu << 26);
      t[2] = (t[2] & ~0x0fffffffu) | 255u | (255u << 14);
      t[4] = (t[4] & ~(0x3fffu << 13)) | ((256u - 1) << 13);
      t[3] &= ~(1u << 26); /* the memory type's top bit (read-only, as the game's has it) */
      if (i == 2)
        t[6] = t[7] = 0;
      if (i == 3)
        t[3] = (t[3] & ~(0x1fu << 20)) | (8u << 20);
    }
    t[0] = (u32)(a >> 8);
    t[1] = (t[1] & ~0xffu) | (u32)((a >> 40) & 0xff);
    k[i].w = k[i].h = 64, k[i].ramp = INSET_RAMP, k[i].debug = 1; /* no insets: all magenta */
  }
  u32 n = 0;
  u64 shader = (u64)g_i.mem;
  n = acb_acquire(cb, n, 1);
  cb[n++] = acb_pm4(0x76, 4), cb[n++] = 0x204, cb[n++] = 0, cb[n++] = 0, cb[n++] = 0;
  cb[n++] = acb_pm4(0x76, 4), cb[n++] = 0x207, cb[n++] = 8, cb[n++] = 8, cb[n++] = 1;
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x20c;
  cb[n++] = (u32)(shader >> 8), cb[n++] = (u32)(shader >> 40);
  u32 rsrc1 = ((K_INSET_GCN_VGPRS - 1) / 4) | (((K_INSET_GCN_SGPRS - 1) / 8) << 6) |
              (0xc0u << 12) | (1u << 21);
  u32 rsrc2 = (2u << 1) | (1u << 7) | (1u << 8) | (1u << 11);
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x212, cb[n++] = rsrc1, cb[n++] = rsrc2;
  for (int i = 0; i < NT; i++) {
    u64 ka = (u64)&k[i];
    cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x240, cb[n++] = (u32)ka, cb[n++] = (u32)(ka >> 32);
    cb[n++] = acb_pm4(0x15, 4), cb[n++] = 8, cb[n++] = 8, cb[n++] = 1, cb[n++] = 0x41;
    n = acb_barrier(cb, n, (u64)label, (u32)i + 1);
  }
  __asm__ volatile("mfence" ::: "memory");
  int rc = gen1_submit_acb(cb, n);
  for (int i = 0; rc == 0 && i < 500 && *label != NT; i++)
    sceKernelUsleep(100);
  LOG("inset: self-test: submit 0x%x, label %u (the game's T#: %08x %08x %08x %08x %08x %08x "
      "%08x %08x)", rc, *label, gt[0], gt[1], gt[2], gt[3], gt[4], gt[5], gt[6], gt[7]);
  for (int i = 0; i < NT; i++) {
    const u8 *p = m + i * SZ;
    u32 changed = 0, magenta = 0;
    for (u32 j = 0; j < SZ; j++)
      changed += p[j] != 0x11;
    for (u32 j = 0; j + 4 <= SZ; j += 4)
      magenta += p[j] == 0xff && p[j + 1] == 0 && p[j + 2] == 0xff && p[j + 3] == 0;
    LOG("inset: self-test %d (%s): %u bytes written, %u magenta texels (4096 expected)", i,
        what[i], changed, magenta);
  }
}
#endif

static void report(u64 now) {
  if (!g_i.report_at) {
    g_i.report_at = now + 10000000;
    return;
  }
  if (now < g_i.report_at)
    return;
  u64 gpu_hz = 0;
  if (g_i.cal_cpu && g_i.done_cpu > g_i.cal_cpu + 1000000 && g_i.done_gpu > g_i.cal_gpu)
    gpu_hz = (g_i.done_gpu - g_i.cal_gpu) * 1000000 / (g_i.done_cpu - g_i.cal_cpu);
  u64 avg = g_i.n_gpu ? g_i.s_gpu / g_i.n_gpu : 0;
  LOG_DETAIL("inset: %u frames given their alpha in the last 10 s; the GPU took %lu ticks a frame "
      "(longest %lu, %lu ticks a second: %lu us); without layer 1: %u with the pass still busy, "
      "%u with a texture it doesn't take",
      g_i.n, avg, g_i.gpu_max, gpu_hz, gpu_hz ? avg * 1000000 / gpu_hz : 0, g_i.skipped_busy,
      g_i.skipped_odd);
  g_i.n = g_i.n_gpu = g_i.skipped_busy = g_i.skipped_odd = 0;
  g_i.s_gpu = g_i.gpu_max = 0;
  g_i.report_at = now + 10000000;
}

/* the shader and the pass's slots, made once Hmd2's queue is known; 1 when ready */
static int ensure_mem(const struct vr_frame *f) {
  if (g_i.off)
    return 0;
  if (g_i.mem)
    return 1;
  if (!gen1_queue_ready()) /* Hmd2's queue known once it has submitted (gen1.c) */
    return 0;
  g_i.mem = vr2_dalloc(SHADER_SPACE + NSLOT * sizeof(struct slot), 0x10000);
  if (!g_i.mem) {
    LOG("inset: no memory for the alpha pass: off");
    g_i.off = 1;
    return 0;
  }
  memcpy(g_i.mem, k_inset_gcn, sizeof k_inset_gcn);
  for (u32 i = 0; i < NSLOT; i++)
    slot(i)->label = 0;
  LOG("inset: the alpha pass ready (%u bytes of shader, an edge of %u texels)",
      (u32)sizeof k_inset_gcn, (u32)INSET_RAMP);
#ifndef RELEASE
  self_test(f->tsharp[0]);
#else
  (void)f;
#endif
  return 1;
}

/* the debug menu's place for the insets to give way (inset_menu_frame, a frame behind) */
static float g_menu_tan[4];
static float g_menu_dx; /* each eye's copy moved in by it (tangent, vr2.c's stereo) */
static volatile u32 g_menu_on;

int inset_alpha_frame(const struct vr_frame *f, const s32 rect[2][4]) {
  s32 axis[2][2];
  float ppt[4];
  multiview_axis(axis, ppt, f->render_time);
  u64 now = sceKernelGetProcessTime();
  if (!ensure_mem(f))
    return 0;
  report(now);
  /* one texture for both eyes, 2D, 8_8_8_8 (an alpha of 8 bits), one mip level, no slice; the
   * insets inside it */
  const u32 *t = f->tsharp[0], *t1 = f->tsharp[1];
  u32 tw = (t[2] & 0x3fff) + 1, th = ((t[2] >> 14) & 0x3fff) + 1, dfmt = (t[1] >> 20) & 0x3f;
  int ok = dfmt == 10 && (t[3] >> 28) == 9 && (t[5] & 0x1fff) == 0 && ((t[3] >> 12) & 0xff) == 0 &&
           t[0] == t1[0] && (t[1] & 0xff) == (t1[1] & 0xff);
  for (int i = 0; ok && i < 2; i++)
    ok = rect[i][0] >= 0 && rect[i][1] >= 0 && rect[i][0] < rect[i][2] &&
         rect[i][1] < rect[i][3] && (u32)rect[i][2] <= tw && (u32)rect[i][3] <= th;
  if (!ok) {
    static u32 logged;
    if (logged++ < 4)
      LOG("inset: the texture (%ux%u format %u type %u, one for both eyes: %d) or the insets "
          "(%d,%d-%d,%d and %d,%d-%d,%d) not as the pass takes them: no layer 1", tw, th, dfmt,
          t[3] >> 28, t[0] == t1[0], rect[0][0], rect[0][1], rect[0][2], rect[0][3], rect[1][0],
          rect[1][1], rect[1][2], rect[1][3]);
    g_i.skipped_odd++;
    return 0;
  }
  /* the slot's previous frame done on the GPU */
  u32 next = g_i.seq + 1;
  struct slot *s = slot(next % NSLOT);
  if (next > NSLOT) {
    u32 before = next - NSLOT;
    if (s->label != before * 8 + 4) {
      g_i.skipped_busy++;
      if (g_i.skipped_busy > 30 && g_i.n == 0) {
        LOG("inset: the GPU hasn't finished the alpha pass for 30 frames: off");
        g_i.off = 1;
      }
      return 0;
    }
    if (g_i.accounted != before && s->ts[1] > s->ts[0]) {
      g_i.accounted = before;
      u64 d = s->ts[1] - s->ts[0];
      g_i.s_gpu += d, g_i.n_gpu++, g_i.gpu_max = d > g_i.gpu_max ? d : g_i.gpu_max;
      g_i.done_cpu = now, g_i.done_gpu = s->ts[1];
    }
  }
  u32 seq = ++g_i.seq;
  struct consts *k = &s->k;
  memset(k, 0, sizeof *k);
  memcpy(k->tex, t, 32);
  k->tex[1] &= ~(0xfu << 26); /* read and written as UNORM: the values as they are */
  /* The game's T# has word 3 bit 26 set, the top bit of its memory type: read-only. Stores
   * through it are dropped without a fault (the self-test, 2026-11-07: none landed, tiled or
   * linear, until this bit was cleared; then all did). The game's own T# is left as it is. */
  k->tex[3] &= ~(1u << 26);
  memcpy(k->rect, rect, sizeof k->rect);
  k->w = (s32)tw, k->h = (s32)th, k->ramp = INSET_RAMP, k->debug = g_opt_insetdebug > 5 ? 0 : (s32)g_opt_insetdebug;
  memcpy(k->axis, axis, sizeof k->axis);
  memcpy(k->ppt, ppt, sizeof k->ppt);
  multiview_centres((float(*)[2])k->ictan, f->render_time);
  if (g_opt_gazedot && fov_gaze(k->gaze))
    k->gazedot = 1;
  if (g_menu_on)
    k->menuon = 1, memcpy(k->menu, g_menu_tan, sizeof k->menu), k->menu_dx = g_menu_dx;
  /* With Hmd2's window test layer 1 shows nothing outside the insets' windows, and layer 0 reads
   * no alpha: the pass covers only the insets' rows (2880x600 of a 2880x2220 texture at
   * agent.multiview=600: it took 0.24-0.41 ms of GPU a frame over the whole, logs/mv99). The
   * whole texture without the window test, and for the debug drawings over the periphery. */
  u32 rows = th;
  if (vr2_window_test() && !g_opt_insetdebug && !k->gazedot) {
    s32 y0 = rect[0][1] < rect[1][1] ? rect[0][1] : rect[1][1];
    s32 y1 = rect[0][3] > rect[1][3] ? rect[0][3] : rect[1][3];
    if (y0 >= 0 && y1 > y0 && (u32)y1 <= th)
      k->oy = y0, rows = (u32)(y1 - y0);
  }
  u32 ndw = build(s, seq, tw, rows);
  __asm__ volatile("mfence" ::: "memory");
  int rc = gen1_submit_acb(s->cb, ndw);
  if (rc != 0) {
    s->label = seq * 8 + 4, s->ts[0] = s->ts[1] = 0;
    if (g_i.failed++ < 8)
      LOG("inset: frame %u: submit 0x%x", seq, rc);
    if (g_i.failed > 30) {
      LOG("inset: giving up");
      g_i.off = 1;
    }
    return 0;
  }
  g_i.n++;
  if (seq == 1) {
    /* the first: done on the GPU in time? (and the timestamps calibrate from here) */
    for (int i = 0; i < 500 && s->label != seq * 8 + 4; i++)
      sceKernelUsleep(100);
    int done = s->label == seq * 8 + 4;
    LOG("inset: the first frame's alpha %s (label 0x%x, GPU clock %lu -> %lu)",
        done ? "done" : "NOT done in 50 ms: off", s->label, s->ts[0], s->ts[1]);
    g_i.cal_cpu = sceKernelGetProcessTime(), g_i.cal_gpu = s->ts[1];
    if (!done) {
      g_i.off = 1;
      return 0;
    }
  }
  return 1;
}

/* The debug menu (menu.c) over both eyes, every frame it is open (vr2.c, after the pass): the
 * shader's stage 2 on Hmd2's queue, after the pass's own barrier. at: its top-left in each eye
 * (texels); tan_rect: its place in tangent from the eye's axis (x from, to; y from, to, up),
 * where the insets give way from the next frame on. Two slots, taken in turn: one still busy, the
 * frame goes without the menu. */
static struct slot *g_menu_slot;
static u32 g_menu_seq;
void inset_menu_frame(const struct vr_frame *f, const s32 at[2][2], const float tan_rect[4], float dx) {
  const u8 *pic = menu_picture();
  if (!pic) {
    g_menu_on = 0;
    return;
  }
  memcpy(g_menu_tan, tan_rect, sizeof g_menu_tan);
  g_menu_dx = dx;
  g_menu_on = 1;
  if (!ensure_mem(f))
    return;
  if (!g_menu_slot) {
    g_menu_slot = vr2_dalloc(2 * sizeof(struct slot), 0x10000);
    if (!g_menu_slot) {
      LOG("inset: no memory for the menu's pass");
      return;
    }
    g_menu_slot[0].label = g_menu_slot[1].label = 0;
  }
  u32 seq = g_menu_seq + 1;
  struct slot *s = &g_menu_slot[seq & 1];
  if (seq > 2 && s->label != (seq - 2) * 8 + 4)
    return; /* its last use still on the GPU */
  const u32 *t = f->tsharp[0];
  u32 tw = (t[2] & 0x3fff) + 1, th = ((t[2] >> 14) & 0x3fff) + 1;
  if (((t[1] >> 20) & 0x3f) != 10 || (t[3] >> 28) != 9)
    return;
  g_menu_seq = seq;
  struct consts *k = &s->k;
  memset(k, 0, sizeof *k);
  memcpy(k->tex, t, 32);
  k->tex[1] &= ~(0xfu << 26);
  k->tex[3] &= ~(1u << 26); /* (read-only memory type: stores dropped) */
  k->w = (s32)tw, k->h = (s32)th, k->stage = 2;
  u64 a = (u64)pic;
  u32 mt[8] = {(u32)(a >> 8), (u32)((a >> 40) & 0xff) | (10u << 20), (MENU_W - 1) | ((MENU_H - 1) << 14),
               0xfacu | (8u << 20) | (9u << 28), (MENU_W - 1) << 13, 0, 0, 0};
  memcpy(k->mtex, mt, 32);
  memcpy(k->mat, at, sizeof k->mat);
  k->mw = (s32)MENU_W, k->mh = (s32)MENU_H;
  u32 ndw = build(s, seq, 2 * MENU_W, MENU_H);
  __asm__ volatile("mfence" ::: "memory");
  if (gen1_submit_acb(s->cb, ndw) != 0)
    s->label = seq * 8 + 4;
}
