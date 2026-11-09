/* Measurement only: each frame's way from the game to the headset, to witness tearing and judder in
 * numbers (2026-11-03: tearing and doubled edges in the headset with frames passed once drawn, none
 * on the TV, none passed at the hand-over). Nothing here changes what is shown. A debug tool:
 * agent.trace=1 in cvars.txt switches it on.
 *
 * Per frame, by the game's frame count (viewport +0x360; vr2.c maps its frame numbers to it in
 * g_vr.shown_count):
 *   sub    the game's first GPU submit of the frame (its render thread, the count still the frame's)
 *   hand   its hand-over (sceHmdReprojectionStart), and how long after a headset flip it came
 *   drawn  its drawn label reached (polled every 200 us by the GPU-label thread)
 *   pass   passed to Hmd2 (vr2.c), its texture pair
 *   build  Hmd2's first build taking it, and how many took it
 *   shown  Hmd2's flips of the headset (gen1.c, the time each was submitted: done at the next of
 *          the headset's refreshes, so up to 8.3 ms later; the margins below count it in): first
 *          and last flip of the frame; it leaves at the next frame's first flip, +8.3 ms
 * (No system call is made here: asking the headset port's flip status from Hmd2's thread ended the
 * game at once, 2026-11-03 23:43, a call into libSceVideoOut through an empty pointer.)
 * Two overlaps, each one a tear:
 *   input  the frame still on the headset when the game's GPU starts the frame that reuses its
 *          textures (three frames on: the GPU in order, so from max(its first submit, the frame
 *          before it drawn))
 *   output Hmd2 setting up a flip into the display buffer it flipped last (on the headset, or
 *          about to be)
 * Every 10 s a summary; each input overlap's full record (12 at most per 10 s) and one sample.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

#define TN 64
struct tf {
  volatile u32 c; /* the count this slot holds; 0: empty */
  u32 pair, frame, flips, builds, phase, checked;
  s32 refresh;
  u64 sub, hand, drawn, pass, build, shown0, shownN, end;
};
static struct tf g_t[TN];

static u64 now(void) { return sceKernelGetProcessTime(); }

static struct tf *slot(u32 c, int make) {
  if (!c)
    return 0;
  struct tf *t = &g_t[c % TN];
  if (t->c != c) {
    if (!make)
      return 0;
    memset(t, 0, sizeof *t);
    t->c = c;
  }
  return t;
}

/* the 10 s summary */
static struct {
  u32 n, overlaps, out_shown, sampled, dumped;
  s64 margin_min;
  u64 s_shown, s_drawn, s_end, s_next;
  u32 n_shown, n_drawn, n_end, n_next;
} g_s = {.margin_min = 1ll << 40};

static void check_reuse(u32 c3);
void trace_submit(void) {
  if (!g_opt_trace)
    return;
  u32 c = (u32)hooks_frame_count();
  struct tf *t = slot(c, 1);
  if (t && !t->sub) {
    t->sub = now();
    check_reuse(c);
  }
}

void trace_handover(u32 c) {
  if (!g_opt_trace)
    return;
  struct tf *t = slot(c, 1);
  if (!t)
    return;
  u64 n = now(), lf = gen1_last_flip();
  t->hand = n;
  t->phase = lf && n > lf && n - lf < 100000 ? (u32)(n - lf) : 0;
  t->refresh = (s32)gen1_refreshes();
}

void trace_pass(u32 c, u32 frame, u32 pair) {
  if (!g_opt_trace)
    return;
  struct tf *t = slot(c, 0);
  if (t)
    t->pass = now(), t->frame = frame, t->pair = pair;
}

void trace_build(u32 frame) {
  if (!g_opt_trace)
    return;
  struct tf *t = slot((u32)g_vr.shown_count[frame % 64], 0);
  if (t && t->frame == frame) {
    if (!t->build)
      t->build = now();
    t->builds++;
  }
}

static void dump(const char *why, const struct tf *t, const struct tf *n3, s64 gpu3, s64 margin) {
  u64 h = t->hand;
  if (!h)
    return;
#define R(x) ((x) ? (s64)(x) - (s64)h : 0)
  LOG("trace %s: frame %u (pair %u, #%u): submit %ld, hand-over 0 (%u us after a flip), drawn %ld, "
      "passed %ld, built %ld (%u builds), flipped to the headset %ld to %ld (%u flips), gone %ld; frame %u "
      "(pair %u) on the GPU from %ld: margin %ld us",
      why, t->c, t->pair, t->frame, R(t->sub), t->phase, R(t->drawn), R(t->pass), R(t->build),
      t->builds, R(t->shown0), R(t->shownN), t->flips, R(t->end), n3 ? n3->c : 0, n3 ? n3->pair : 0,
      gpu3 ? gpu3 - (s64)h : 0, margin);
#undef R
}

/* the frame reusing t's textures (three on) is starting on the GPU: is t still on the headset? */
static void check_reuse(u32 c3) {
  struct tf *t3 = slot(c3, 0), *t2 = slot(c3 - 1, 0), *t = slot(c3 - 3, 0);
  if (!t3 || !t || t->checked || !t->pass || !t3->sub)
    return; /* (checked again at the later of its first submit and the frame before it drawn;
             * a frame never passed to Hmd2 is not on the headset) */
  if (t2 && !t2->drawn)
    return;
  u64 start = t3->sub;
  if (t2 && t2->drawn > start)
    start = t2->drawn; /* the GPU runs in order: behind the frame before it */
  t->checked = 1;
  /* still on the headset, or not on it yet (passed, not shown): until now at least; a flip is done
   * up to a refresh after its submit, so the frame leaves up to 8.3 ms after the next one's */
  u64 end = (t->end ? t->end : now()) + 8333;
  s64 margin = (s64)start - (s64)end;
  g_s.n++;
  if (margin < g_s.margin_min)
    g_s.margin_min = margin;
  if (t->end && t3->sub)
    g_s.s_next += start - t->hand, g_s.n_next++;
  if (margin < 0) {
    g_s.overlaps++;
    if (g_s.dumped++ < 12)
      dump("INPUT OVERLAP", t, t3, (s64)start, margin);
  } else if (!g_s.sampled) {
    g_s.sampled = 1;
    dump("sample", t, t3, (s64)start, margin);
  }
}

void trace_drawn(u32 c) {
  if (!g_opt_trace)
    return;
  struct tf *t = slot(c, 0);
  if (t && !t->drawn) {
    t->drawn = now();
    if (t->hand)
      g_s.s_drawn += t->drawn - t->hand, g_s.n_drawn++;
  }
  check_reuse(c + 1); /* the frame after it may now start on the GPU */
}

/* Hmd2 flipped the headset (hooks_flip, from gen1.c's cpu_flip, just after the submit) */
void trace_flip(u32 frame) {
  if (!g_opt_trace)
    return;
  static u32 last_frame;
  u64 n = now();
  struct tf *t = slot((u32)g_vr.shown_count[frame % 64], 0);
  if (!t || t->frame != frame)
    return;
  if (frame != last_frame) {
    struct tf *p = slot((u32)g_vr.shown_count[last_frame % 64], 0);
    if (p && p->frame == last_frame && !p->end) {
      p->end = n;
      if (p->hand)
        g_s.s_end += p->end - p->hand, g_s.n_end++;
    }
    last_frame = frame;
  }
  if (!t->shown0) {
    t->shown0 = n;
    if (t->hand)
      g_s.s_shown += n - t->hand, g_s.n_shown++;
  }
  t->shownN = n;
  t->flips++;
}

/* Hmd2 sets up its flip of the headset (gen1.c set_flip, its build): into the display buffer it
 * flipped last? (from our own record: no call) */
void trace_set_flip(int index) {
  if (!g_opt_trace)
    return;
  static int last_index = -1;
  if (index == last_index)
    g_s.out_shown++;
  last_index = index;
}

void trace_report(void) {
  if (!g_opt_trace)
    return;
#define AVG(s, n) ((n) ? (s64)((s) / (n)) / 1000 : 0), ((n) ? (s64)((s) / (n)) / 100 % 10 : 0)
  LOG("trace: %u frames checked; after the hand-over: drawn %ld.%ld ms, on the headset %ld.%ld, off "
      "it %ld.%ld, the frame reusing its textures on the GPU %ld.%ld; the smallest margin %ld us, "
      "%u input overlaps (tears); Hmd2 set %u flips into the display buffer it flipped last",
      g_s.n, AVG(g_s.s_drawn, g_s.n_drawn), AVG(g_s.s_shown, g_s.n_shown), AVG(g_s.s_end, g_s.n_end),
      AVG(g_s.s_next, g_s.n_next), g_s.n ? g_s.margin_min : 0, g_s.overlaps, g_s.out_shown);
#undef AVG
  memset(&g_s, 0, sizeof g_s);
  g_s.margin_min = 1ll << 40;
}
