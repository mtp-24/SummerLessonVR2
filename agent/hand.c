/* PS VR2 hand tracking inside the PS4 game: libSceVrHand, which PS5VR runs in a PS5 app, here in
 * the game's process next to Hmd2 and VrTracker2.
 *  - The payload prepares the library like the other four, and it is loaded with them as PS VR2
 *    starts. It is started on a thread of its own (the headset never waits for it) once the
 *    headset has stayed worn with no system screen up (wait_clear_moment), once a session.
 *    Without it, or if anything below fails, the controller stays where it was placed before.
 *  - Its models (handCenter_00, handPose_00/_02) come from the system's copy through the payload
 *    (CMD_MODEL): its import of VrTracker2's "set VR model" export (9R6W33Au4B4, which asks the
 *    tracker daemon, which answers a game nothing) is answered here, as PS5VR does.
 *  - It reads the headset cameras through libSceHmd2, which needs the game's credentials raised
 *    (CMD_CAMERA) while it opens them; they go back to the game's own once its camera stream is
 *    up. The cameras' gate (gate_eval's section) holds the start while the kernel log says the
 *    system's own camera work is on or just over, which a start must not overlap.
 *  - Its GPU work: every shader it creates (sceAgcCreateShader) is translated to GCN on the spot
 *    (payload/rdna2gcn.c, the translator Hmd2's shader goes through) and the shader's register
 *    list is pointed at the translation; its compute buffers get the gen1 packet forms gen1.c
 *    gives Hmd2's. A shader that can't be translated never runs: its dispatches become NOPs, and
 *    so does every draw (none of its graphics shaders translate). Graphics buffers are never
 *    submitted.
 *  - It waits for its GPU work on an equeue event that a gen1 queue never raises (as Hmd2 does,
 *    gen1.c): the wait polls the label the work's end writes instead, then hands back the event
 *    AgcDriver would have.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

#include "../common/firmware.h"
#include "../payload/rdna2gcn.h"

/* 0: dry run for the first runs on hardware. VrHand's GPU work is converted and logged but never
 *    submitted, and its waits for it end at once: everything on the CPU side runs, nothing it
 *    computes is real, so no hand is ever seen. 1: submitted on its compute queue. */
#define HAND_SUBMIT 1
/* 0: the game's credentials go back to its own once VrHand has started (the cameras are open) */
#define HAND_KEEP_CAMERA 0

/* 1: the first command buffer is listed dispatch by dispatch (program, groups, initiator), and
 *    one more dispatch per submit runs (each must finish before the next is let in), so the log
 *    names the first one that never finishes. */
#define HAND_PROBE 0

#define HAND_WORK 0x3c00000u /* 60 MB, the library's minimum */
#define HAND_RESULT 0xdb8
#define HAND_PIPE 6  /* 5 is Summer Lesson's own compute queue (pipe 5 queue 0); queue id 0x20 + 8 * pipe + queue = 0x50 */
#define HAND_QUEUE 0 /* (Hmd2 has 0x28; a stuck queue of VrHand's own leaves the headset alone) */
#define HAND_30HZ 0  /* InitializeParam +0x30: 0 60 Hz (the most it takes), 1 30 Hz */
#define HAND_POLL 4000 /* us between looks at the results: a new one is taken up within this */
#define HAND_LOOKS (10000000 / HAND_POLL) /* in 10 s */
#define HAND_GIVE_UP 3 /* GPU waits in a row that time out: hand tracking stops */
#define LIB_VRHAND_INDEX 4 /* the payload's library list: libSceVrHand.sprx */

static int g_lib = -1;

/* ---------------------------------------------------------------- shaders */
#define MAX_SHADERS 160
#define POOL_SIZE 0x200000
#define FENCE_BYTES 0x1000 /* the start of the pool: one label per queue id */
#define REG_NUM_THREAD_X 0x207
#define REG_PGM_LO 0x20c
#define REG_PGM_HI 0x20d
#define REG_RSRC1 0x212
#define REG_RSRC2 0x213

static struct {
  u32 pgm_lo;     /* COMPUTE_PGM_LO of the translation */
  u16 threads[3]; /* the group as the shader was made for (a split wave32 group runs as twice
                   * as many threads in x) */
  u8 split;
} g_sh[MAX_SHADERS];
static volatile u32 g_nsh;
static u8 *g_pool; /* GPU-visible: the labels, then the translations */
static u32 g_pool_used;
static volatile u32 g_xl_lock;
static u32 g_tmp[0x8000];
static u32 g_failed;

/* the translation a COMPUTE_PGM_LO value runs, -1 if none */
static int translation_of(u32 pgm_lo) {
  u32 n = __atomic_load_n(&g_nsh, __ATOMIC_ACQUIRE);
  for (u32 i = 0; i < n; i++)
    if (g_sh[i].pgm_lo == pgm_lo)
      return (int)i;
  return -1;
}

/* Agc's shader object is its binary header, made absolute by sceAgcCreateShader ("1234", version
 * 0x18): +0x10 the code, +0x20 its register list ({register, value} pairs, +0x5c of them; PGM_LO/HI
 * filled in by Agc), +0x5a the stage (0 = compute), +0x70 bit 15 wave32. */
static void translate(u8 *s) {
  if (*(u32 *)s != 0x34333231u || *(u32 *)(s + 4) != 0x18) {
    if (g_failed++ < 80)
      LOG("hand: shader object %p not translated: header %x version %x", s, *(u32 *)s,
          *(u32 *)(s + 4));
    return;
  }
  u64 code = *(u64 *)(s + 0x10);
  u32 *regs = *(u32 **)(s + 0x20);
  u32 n = s[0x5c], stage = s[0x5a];
  if (stage != 0 || !regs || !code) {
    if (g_failed++ < 80)
      LOG("hand: shader %lx (stage %u) not translated: not compute", code, stage);
    return;
  }
  struct rdna2gcn_cs cs;
  memset(&cs, 0, sizeof cs);
  cs.wave32 = (*(u32 *)(s + 0x70) >> 15) & 1;
  for (u32 i = 0; i < n; i++) {
    u32 r = regs[2 * i];
    if (r >= REG_NUM_THREAD_X && r < REG_NUM_THREAD_X + 3)
      cs.threads[r - REG_NUM_THREAD_X] = regs[2 * i + 1] & 0xffff;
    else if (r == REG_RSRC2)
      cs.tidig = (regs[2 * i + 1] >> 11) & 3;
  }
  while (__atomic_exchange_n(&g_xl_lock, 1, __ATOMIC_ACQUIRE))
    sceKernelUsleep(100);
  struct rdna2gcn_info info;
  int rc = rdna2gcn_compute((const unsigned *)code, sizeof g_tmp / 4, g_tmp, sizeof g_tmp / 4, &cs,
                            &info);
  u32 need = (info.out_dwords * 4 + 0xff) & ~0xffu;
  if (rc == 0 && (!g_pool || g_pool_used + need > POOL_SIZE || g_nsh >= MAX_SHADERS)) {
    info.error = "no room for the translation";
    rc = -1;
  }
  if (rc != 0) {
    __atomic_store_n(&g_xl_lock, 0, __ATOMIC_RELEASE);
    if (g_failed++ < 80)
      LOG("hand: shader %lx not translated: %s at dword %u", code, info.error, info.error_at);
    return;
  }
  u8 *dst = g_pool + g_pool_used;
  memcpy(dst, g_tmp, info.out_dwords * 4);
  g_pool_used += need;
  u64 gcn = (u64)dst;
  u32 seen = 0;
  for (u32 i = 0; i < n; i++) {
    u32 r = regs[2 * i], *v = &regs[2 * i + 1];
    if (r == REG_PGM_LO) {
      seen |= (*v == (u32)(code >> 8)) ? 1 : 0x10;
      *v = (u32)(gcn >> 8);
    } else if (r == REG_PGM_HI) {
      *v = (u32)(gcn >> 40) & 0xff;
      seen |= 2;
    } else if (r == REG_RSRC1) {
      /* gfx7: the register counts of the translation; priority, float mode, DX10 clamp and IEEE
       * mode as given (bits 24 up are gfx10-only) */
      *v = (*v & 0x00fffc00u) | info.rsrc1_vgprs | (info.rsrc1_sgprs << 6);
      seen |= 4;
    } else if (r == REG_RSRC2) {
      *v &= 0x7fffffffu; /* same layout on gfx7 (LDS granularity 512 bytes on both) */
      if (info.hw_threads)
        *v &= ~(3u << 11); /* TIDIG_COMP_CNT 0: the code makes the ids from thread x */
      seen |= 8;
    } else if (info.hw_threads && r >= REG_NUM_THREAD_X && r < REG_NUM_THREAD_X + 3) {
      /* one wave64 per wave32 (rdna2gcn_compute): the group is a row of hw_threads */
      *v = r == REG_NUM_THREAD_X ? info.hw_threads : 1;
      seen |= 0x20;
    }
  }
  u32 k = g_nsh;
  g_sh[k].pgm_lo = (u32)(gcn >> 8);
  for (int a = 0; a < 3; a++)
    g_sh[k].threads[a] = (u16)(cs.threads[a] ? cs.threads[a] : 1);
  g_sh[k].split = info.hw_threads != 0;
  __atomic_fetch_add(&g_nsh, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&g_xl_lock, 0, __ATOMIC_RELEASE);
  LOG("hand: shader #%u %lx -> GCN %lx: %u -> %u dwords, %u VGPRs %u SGPRs, wave%u %ux%ux%u%s, "
      "regs %x", k, code, gcn, info.src_dwords, info.out_dwords, info.vgprs, info.sgprs,
      info.wave32 ? 32 : 64, cs.threads[0], cs.threads[1], cs.threads[2],
      info.hw_threads ? " (one wave64 per wave32)" : "", seen);
}

static int create_shader(void **dst, void *header, const void *code) {
  int rc = sceAgcCreateShader(dst, header, code);
  if (rc == 0 && dst && *dst)
    translate((u8 *)*dst);
  return rc;
}

/* ---------------------------------------------------------------- command buffers */
struct cb_desc {
  u64 addr;
  u32 dwords;
  u8 flags;
};

#define NQ 0x80 /* queue ids: 0x20 + 8 * pipe (<= 6) + queue (<= 7) */
#define NOP(len) (0xc0001000u | (((len)-2) << 16))
#define GFX7_RELEASE_MEM 0xc0054900u /* 7 dwords: a gfx10 one (8) gets a type-2 filler */

/* per queue id: what the end of the last buffer submitted to it writes, which its wait polls */
static struct {
  volatile u64 *label;
  u64 want;
  u32 wide;
} g_end[NQ];
static u32 g_fence_seq;
static volatile u32 g_dead; /* its GPU work stopped finishing: nothing more goes to the GPU */

static struct {
  u32 buffers, dispatches, dropped, draws, gfx, waits, timeouts;
} g_stats;

#if HAND_PROBE
/* VrHand builds its command buffer once and submits it again and again (it is converted in
 * place), so the dispatch packets are kept here and let back in one more per submit. */
#define MAXD 512
static struct {
  u32 *cb;
  u32 at, len, pkt[6];
  int sh; /* the translation it runs */
} g_disp[MAXD];
static u32 g_ndisp, g_run_limit = 1, g_probe_done;

static int disp_slot(u32 *cb, u32 at, u32 len, int add) {
  for (u32 k = 0; k < g_ndisp; k++)
    if (g_disp[k].cb == cb && g_disp[k].at == at)
      return (int)k;
  if (!add || g_ndisp >= MAXD)
    return -1;
  g_disp[g_ndisp].cb = cb, g_disp[g_ndisp].at = at, g_disp[g_ndisp].len = len;
  memcpy(g_disp[g_ndisp].pkt, cb + at, len * 4);
  return (int)g_ndisp++;
}
static int held_back(u32 n) { return !g_probe_done ? n > g_run_limit : 0; }
#else
static int held_back(u32 n) { (void)n; return 0; }
#endif

/* One of VrHand's command buffers in gen1 form (gen1.c does the same for Hmd2's), in place; it
 * comes back converted on the next submit, so every step leaves a converted packet alone. q: the
 * compute queue id it goes to, -1 for graphics. The first few are listed in the log. */
static void convert(u32 *cb, u32 ndw, int q) {
  u32 i = 0, pgm = 0, dispatches = 0, dropped = 0, draws = 0;
  u32 ops[24], counts[24], nops = 0;
  static u32 listed, detailed;
  int detail = HAND_PROBE && q >= 0 && !detailed++;
  while (i < ndw) {
    u32 h = cb[i], type = h >> 30;
    if (type == 2) {
      i++;
      continue;
    }
    u32 len = ((h >> 16) & 0x3fff) + 2;
    if (type != 3 || i + len > ndw) {
      LOG("hand: buffer %p: no packet at dword %u of %u (%08x), rest left alone", cb, i, ndw, h);
      break;
    }
    u32 op = (h >> 8) & 0xff, *d = cb + i + 1;
    u32 k = 0;
    while (k < nops && ops[k] != op)
      k++;
    if (k < nops)
      counts[k]++;
    else if (nops < 24)
      ops[nops] = op, counts[nops++] = 1;
    int dispatch = (op == 0x15 && len == 5) || (op == 0x16 && len == 3);
#if HAND_PROBE
    if (op == 0x10 && disp_slot(cb, i, len, 0) >= 0)
      dispatch = 1; /* one the probe held back last time */
#endif
    if ((op == 0x79 && (d[0] & 0xffff) >= 0x340 && (d[0] & 0xffff) <= 0x347) ||
        (op == 0x37 && ((d[0] >> 8) & 0xf) == 0 && d[1] >= 0xc340 && d[1] <= 0xc347)) {
      cb[i] = NOP(len); /* profiler marker: privileged in gen1 */
    } else if (op == 0x76 && len >= 3) {
      /* SET_SH_REG: the program the next dispatch runs */
      u32 r0 = d[0] & 0xffff;
      if (r0 <= REG_PGM_LO && REG_PGM_LO < r0 + len - 2)
        pgm = d[1 + REG_PGM_LO - r0];
    } else if (dispatch) {
      /* DISPATCH_DIRECT / _INDIRECT, DISPATCH_INITIATOR last: only translated programs run; bits
       * 12-15 are RDNA's (wave32, tunnel) and gfx7's ATC */
#if HAND_PROBE
      int slot = disp_slot(cb, i, len, 1);
      if (slot >= 0)
        memcpy(cb + i, g_disp[slot].pkt, len * 4); /* the packet as VrHand wrote it */
      op = (cb[i] >> 8) & 0xff;
#endif
      dispatches++;
      int t = translation_of(pgm);
      u32 *init = &cb[i + len - 1];
#if HAND_PROBE
      if (slot >= 0)
        g_disp[slot].sh = t;
#endif
      if (t < 0 || held_back(dispatches)) {
        cb[i] = NOP(len);
        dropped++;
      } else {
        if (g_sh[t].split && op == 0x15 && (*init & (1u << 5))) {
          /* thread counts, in groups of the shader's own shape: its split group is a row of
           * hardware threads (partial groups run whole) */
          for (int a = 0; a < 3; a++)
            d[a] = (d[a] + g_sh[t].threads[a] - 1) / g_sh[t].threads[a];
          *init &= ~((1u << 5) | (1u << 1));
        }
        *init &= ~0xf000u;
      }
      if (detail)
        LOG("hand: dispatch %u: shader #%d, %u x %u x %u, initiator %x%s", dispatches, t,
            op == 0x15 ? d[0] : 0, op == 0x15 ? d[1] : 0, op == 0x15 ? d[2] : 0, *init,
            cb[i] == NOP(len) ? " (held back)" : "");
    } else if (pm4_is_draw(op)) {
      cb[i] = NOP(len);
      draws++;
    } else if (op == 0x49 && (len == 8 || (len == 7 && h == GFX7_RELEASE_MEM))) {
      if (len == 8) {
        /* RELEASE_MEM, gfx7 compute form (as gen1.c): no interrupt. One that only raised the
         * interrupt writes a label of ours instead, for the wait to poll. */
        u32 data_sel = d[1] >> 29, lo = d[2], hi = d[3], v0 = d[4], v1 = d[5];
        if (data_sel == 0 && q >= 0 && g_pool) {
          u64 a = (u64)(g_pool + 8 * (u32)q);
          data_sel = 1, lo = (u32)a, hi = (u32)(a >> 32), v0 = 0, v1 = 0;
        }
        cb[i] = GFX7_RELEASE_MEM;
        d[0] = (1u << 16) | (1u << 17) | (1u << 15) | (5u << 8) | 0x14;
        d[1] = data_sel << 29;
        d[2] = lo, d[3] = hi, d[4] = v0, d[5] = v1;
        d[6] = 0x80000000u;
      }
      u32 data_sel = d[1] >> 29;
      u64 addr = ((u64)d[3] << 32) | d[2];
      if (q >= 0 && g_pool && addr == (u64)(g_pool + 8 * (u32)q)) {
        /* ours: a new value every submit, or the wait would find the last one's */
        d[4] = __atomic_add_fetch(&g_fence_seq, 1, __ATOMIC_RELAXED);
        d[5] = 0;
      }
      if (q >= 0 && (data_sel == 1 || data_sel == 2)) {
        g_end[q].label = (volatile u64 *)addr;
        g_end[q].want = data_sel == 2 ? ((u64)d[5] << 32 | d[4]) : d[4];
        g_end[q].wide = data_sel == 2;
      }
    } else if (op == 0x58 && len == 8) {
      /* ACQUIRE_MEM, gfx7 compute form: TCL1, TC, K$, I$ over everything */
      cb[i] = 0xc0055800u;
      d[0] = (1u << 22) | (1u << 23) | (1u << 27) | (1u << 29);
      d[1] = 0xffffffffu, d[2] = 0xff, d[3] = 0, d[4] = 0, d[5] = 0x0a;
      d[6] = 0x80000000u;
    } else if (detail && op == 0x46) {
      LOG("hand: event write %x", d[0]);
    }
    i += len;
  }
  g_stats.buffers++;
  g_stats.dispatches += dispatches, g_stats.dropped += dropped, g_stats.draws += draws;
  if (listed++ < 6) {
    char line[24 * 16];
    u32 at = 0;
    line[0] = 0;
    for (u32 k = 0; k < nops && at + 16 <= sizeof line; k++)
      at += agent_snprintf(line + at, sizeof line - at, " %x:%u", ops[k], counts[k]);
    LOG("hand: %s buffer %p, %u dwords, queue 0x%x: %u dispatches (%u dropped), %u draws "
        "dropped; packets%s", q < 0 ? "graphics" : "compute", cb, ndw, q < 0 ? 0 : q, dispatches,
        dropped, draws, line);
  }
}

static void convert_desc(int q, u64 addr, u32 ndw) {
  if (addr && ndw && ndw < 0x100000)
    convert((u32 *)addr, ndw, q < NQ ? q : -1);
}

static void log_submit(const char *how, u32 q, int rc) {
  static u32 n;
  if (n++ < 3 || rc != 0)
    LOG("hand: %s on queue 0x%x -> 0x%x", how, q, rc);
}
static int submit_hb3(u32 q, const struct cb_desc *c, u64 arg) {
  convert_desc((int)q, c->addr, c->dwords);
  if (!HAND_SUBMIT || g_dead)
    return 0;
  int rc = sceAgcDriverSubmitAcbHb3 ? sceAgcDriverSubmitAcbHb3(q, c, arg) : -1;
  log_submit("submit", q, rc);
  return rc;
}
static int submit_acb(u32 q, const struct cb_desc *c) {
  convert_desc((int)q, c->addr, c->dwords);
  if (!HAND_SUBMIT || g_dead)
    return 0;
  int rc = sceAgcDriverSubmitAcb ? sceAgcDriverSubmitAcb(q, c) : -1;
  log_submit("SubmitAcb", q, rc);
  return rc;
}
static int submit_multi_acbs(u32 q, void *const *a, const u32 *n, u32 count) {
  for (u32 k = 0; k < count; k++)
    convert_desc((int)q, (u64)a[k], n[k]);
  if (!HAND_SUBMIT || g_dead)
    return 0;
  int rc = sceAgcDriverSubmitMultiAcbs ? sceAgcDriverSubmitMultiAcbs(q, a, n, count) : -1;
  log_submit("SubmitMultiAcbs", q, rc);
  return rc;
}
/* Graphics: nothing in it can run here; listed, never submitted, its waits end at once. */
static int submit_dcb(const struct cb_desc *c) {
  convert_desc(-1, c->addr, c->dwords);
  g_stats.gfx++;
  return 0;
}
static int submit_multi_dcbs(void *const *a, const u32 *n, u32 count) {
  for (u32 k = 0; k < count; k++)
    convert_desc(-1, (u64)a[k], n[k]);
  g_stats.gfx += count;
  return 0;
}

/* ---------------------------------------------------------------- GPU waits */
/* sceAgcDriverAddEqEvent(eq, id, udata) = kevent EV_ADD {id, EVFILT_GRAPHICS_CORE, data id};
 * VrHand's id is the queue id it submits to (0/1 for graphics). It adds the event, submits,
 * waits, deletes it, and takes sceAgcDriverGetEqContextId (data >> 16) == 1 as done. */
struct kevent_ps {
  u64 ident;
  s16 filter;
  u16 flags;
  u32 fflags;
  s64 data;
  void *udata;
};

#define MAX_EQS 16
static struct {
  void *eq;
  int id;
  void *udata;
} g_eqs[MAX_EQS];
static volatile u32 g_eq_lock;

static int add_eq_event(void *eq, int id, void *udata) {
  while (__atomic_exchange_n(&g_eq_lock, 1, __ATOMIC_ACQUIRE))
    sceKernelUsleep(10);
  int k = 0;
  while (k < MAX_EQS && g_eqs[k].eq && g_eqs[k].eq != eq)
    k++;
  if (k < MAX_EQS)
    g_eqs[k].eq = eq, g_eqs[k].id = id, g_eqs[k].udata = udata;
  __atomic_store_n(&g_eq_lock, 0, __ATOMIC_RELEASE);
  static u32 logged;
  if (logged++ < 8 || k == MAX_EQS)
    LOG("hand: GPU event 0x%x on equeue %p%s", id, eq, k == MAX_EQS ? " (not kept: table full)" : "");
  return sceAgcDriverAddEqEvent(eq, id, udata);
}

static int delete_eq_event(void *eq, int id) {
  while (__atomic_exchange_n(&g_eq_lock, 1, __ATOMIC_ACQUIRE))
    sceKernelUsleep(10);
  for (int k = 0; k < MAX_EQS && g_eqs[k].eq; k++)
    if (g_eqs[k].eq == eq && g_eqs[k].id == id) {
      int last = k;
      while (last + 1 < MAX_EQS && g_eqs[last + 1].eq)
        last++;
      g_eqs[k] = g_eqs[last]; /* the table stays packed */
      g_eqs[last].eq = 0;
      break;
    }
  __atomic_store_n(&g_eq_lock, 0, __ATOMIC_RELEASE);
  return sceAgcDriverDeleteEqEvent ? sceAgcDriverDeleteEqEvent(eq, id) : -1;
}

static int gpu_event_of(void *eq, int *id, void **udata) {
  int found = 0;
  while (__atomic_exchange_n(&g_eq_lock, 1, __ATOMIC_ACQUIRE))
    sceKernelUsleep(10);
  for (int k = 0; k < MAX_EQS && g_eqs[k].eq && !found; k++)
    if (g_eqs[k].eq == eq) {
      *id = g_eqs[k].id, *udata = g_eqs[k].udata;
      found = 1;
    }
  __atomic_store_n(&g_eq_lock, 0, __ATOMIC_RELEASE);
  return found;
}

static int wait_equeue(void *eq, void *ev, int num, int *out, u32 *timo) {
  int id;
  void *udata;
  if (!ev || num < 1 || !gpu_event_of(eq, &id, &udata))
    return sceKernelWaitEqueue(eq, ev, num, out, timo);
  /* whatever is really there first (a user event on the same queue) */
  u32 now_only = 0;
  int n = 0;
  if (sceKernelWaitEqueue(eq, ev, num, &n, &now_only) == 0 && n > 0) {
    if (out)
      *out = n;
    return 0;
  }
  g_stats.waits++;
  int done = 1;
  if (HAND_SUBMIT && !g_dead && id >= 0x20 && id < NQ) {
    volatile u64 *label = g_end[id].label;
    u64 want = g_end[id].want, deadline = sceKernelGetProcessTime();
    deadline += timo && *timo < 2000000 ? *timo : 2000000;
    done = 0;
    while (label) {
      u64 v = *label;
      if (g_end[id].wide ? v == want : (u32)v == (u32)want) {
        done = 1;
        break;
      }
      if (sceKernelGetProcessTime() >= deadline)
        break;
      sceKernelUsleep(50);
    }
    if (!label)
      done = 1; /* nothing to poll: the buffer's end wrote no label */
  } else {
    sceKernelUsleep(1000);
  }
#if HAND_PROBE
  if (HAND_SUBMIT && !g_probe_done && !g_dead) {
    u32 j = g_run_limit;
    int sh = j >= 1 && j <= g_ndisp ? g_disp[j - 1].sh : -2;
    if (done && j < g_ndisp) {
      LOG("hand: probe: dispatch %u (shader #%d) finishes", j, sh);
      g_run_limit = j + 1;
    } else if (done) {
      LOG("hand: probe: all %u dispatches finish", g_ndisp);
      g_probe_done = 1;
    } else {
      LOG("hand: probe: dispatch %u (shader #%d) never finishes", j, sh);
      g_probe_done = 1;
    }
  }
#endif
  static u32 in_a_row;
  if (!done) {
    if (g_stats.timeouts++ < 8)
      LOG("hand: GPU wait on queue 0x%x timed out (label %lx, want %lx)", id,
          *g_end[id].label, g_end[id].want);
    if (++in_a_row >= HAND_GIVE_UP && !g_dead) {
      g_dead = 1;
      LOG("hand: its GPU work stopped finishing; hand tracking stops");
    }
    if (out)
      *out = 0;
    return (int)0x8002003c;
  }
  in_a_row = 0;
  struct kevent_ps *e = ev;
  memset(e, 0, sizeof *e);
  e->ident = (u64)id;
  e->filter = -14;
  e->data = (1 << 16) | (id & 0xffff);
  e->udata = udata;
  if (out)
    *out = 1;
  return 0;
}

/* ---------------------------------------------------------------- the models */
/* VrTracker2's "set VR model" contract (from PS5VR): 0 and *out = bytes; 0x8a740006 bad
 * arguments; 0x8a740009 the buffer is too small. Model 2v+1 = handCenter_%02d, 2v+2 handPose. */
static int read_model(int id, void *buf, u64 size, u64 *out) {
  if (!buf || !out || id < 1 || id > 6 || !((0x46 >> id) & 1))
    return (int)0x8a740006;
  u64 got = (u64)-1;
  int rc = agent_call(CMD_MODEL, (u64)id, (u64)buf, size, 0, 8000, 3, &got);
  LOG("hand: model %d: %ld (buffer %lu bytes, request %d)", id, (s64)got, size, rc);
  if (rc != 0 || (s64)got == -1) {
    *out = 0;
    return 0;
  }
  if ((s64)got == -2) {
    *out = size;
    return (int)0x8a740009;
  }
  *out = got;
  return 0;
}

/* ---------------------------------------------------------------- the palms */
/* written by the hand thread, read by the game's tracker hook (vr.c) */
static struct {
  volatile u32 seq;
  float pos[2][3];
  u64 at[2];
} g_palms;

static void publish_palm(int hand, const float p[3]) {
  /* a palm that isn't a number, or is more than 10 m off: not one (the controller would follow) */
  for (int i = 0; i < 3; i++)
    if (!__builtin_isfinite(p[i]) || p[i] > 10.0f || p[i] < -10.0f) {
      static u32 dropped;
      if (dropped++ < 8)
        LOG("hand: a %s palm that isn't one dropped (%d)", hand ? "right" : "left", (int)i);
      return;
    }
  __atomic_fetch_add(&g_palms.seq, 1, __ATOMIC_ACQ_REL);
  memcpy(g_palms.pos[hand], p, 12);
  g_palms.at[hand] = sceKernelGetProcessTime();
  __atomic_fetch_add(&g_palms.seq, 1, __ATOMIC_ACQ_REL);
}

int hand_palm(int hand, float pos[3]) {
  if (hand < 0 || hand > 1)
    return 0;
  for (int tries = 0; tries < 4; tries++) {
    u32 s = __atomic_load_n(&g_palms.seq, __ATOMIC_ACQUIRE);
    if (s & 1)
      continue;
    float p[3];
    memcpy(p, g_palms.pos[hand], 12);
    u64 at = g_palms.at[hand];
    if (__atomic_load_n(&g_palms.seq, __ATOMIC_ACQUIRE) != s)
      continue;
    if (!at || sceKernelGetProcessTime() - at > 150000) /* seen in the last 150 ms */
      return 0;
    memcpy(pos, p, 12);
    return 1;
  }
  return 0;
}

/* ---------------------------------------------------------------- the library */
/* Loaded as PS VR2 starts (vr2.c load_stack, after the four libraries and their overrides,
 * before its CMD_RESOLVE), so the library mount never goes up in mid-game; started later
 * (hand_thread). */
void hand_load(void) {
  const struct astro_firmware *fw = astro_firmware(astro_hdr.firmware);
  if (!g_opt_hands || !fw || !fw->hand.name)
    return;
  agent_call(CMD_STAGE, LIB_VRHAND_INDEX, 0, 0, 0, 3000, 0, 0);
  int res = 0;
  g_lib = sceKernelLoadStartModule("/summerlessonvr2/libSceVrHand.sprx", 0, 0, 0, 0, &res);
  LOG("hand: load libSceVrHand.sprx -> 0x%x (start 0x%x)", g_lib, res);
  agent_call(CMD_STAGE, LIB_VRHAND_INDEX, 1, 0, 0, 3000, 0, 0);
  /* its unbound imports -> the agent's stub; the injector also takes the library mount down */
  agent_call(CMD_STUB, g_lib >= 0 ? (u64)g_lib : 0, 0, 0, 0, 3000, 0, 0);
  if (g_lib < 0)
    return;
  ps5compat_install_hand(g_lib);
  vr2_override(g_lib, NID_sceAgcCreateShader, (void *)create_shader);
  vr2_override(g_lib, NID_sceAgcDriverSubmitAcbHb3, (void *)submit_hb3);
  vr2_override(g_lib, NID_sceAgcDriverSubmitAcb, (void *)submit_acb);
  vr2_override(g_lib, NID_sceAgcDriverSubmitMultiAcbs, (void *)submit_multi_acbs);
  vr2_override(g_lib, NID_sceAgcDriverSubmitDcb, (void *)submit_dcb);
  vr2_override(g_lib, NID_sceAgcDriverSubmitMultiDcbs, (void *)submit_multi_dcbs);
  vr2_override(g_lib, "AhGvpITrf4M", (void *)submit_dcb);        /* AgrSubmitDcb */
  vr2_override(g_lib, "+T8Xo6LtFJI", (void *)submit_multi_dcbs); /* AgrSubmitMultiDcbs */
  vr2_override(g_lib, NID_sceAgcDriverAddEqEvent, (void *)add_eq_event);
  vr2_override(g_lib, NID_sceAgcDriverDeleteEqEvent, (void *)delete_eq_event);
  vr2_override(g_lib, NID_sceKernelWaitEqueue, (void *)wait_equeue);
  vr2_override(g_lib, "9R6W33Au4B4", (void *)read_model);
}

static int lib_ready(void) {
  return g_lib >= 0 && sceVrHandInitialize && sceVrHandStart && sceVrHandStop && sceVrHandGetResult;
}

/* the times in the lines below: the game's process time, for lining them up with the kernel
 * log's system screens (tools/testrun.sh's .ticks) */
#define SECS(t) (u32)((t) / 1000000), (u32)((t) / 1000 % 1000)

/* ---------------------------------------------------------------- the cameras' gate */
/* The headset cameras as the kernel log tells it (common/camera.h: the injector polls the kernel
 * message buffer, kern.msgbuf, and writes g_shared.cam every 100 ms). Every console hang of the palm tracker's was a START
 * (the credentials raised, /dev/hmd2_image and /dev/hmd2_leddet opened, VrHand's camera thread
 * configuring them) that overlapped the system's own headset-camera work: the safety notice and
 * play-area setup (Allison Snow's first two starts), the post-mount bring-up of a game started
 * with the headset worn ("Don't need show SafetyNotice", the c-stock and game-switch hangs,
 * about 10 and 7 s after it), the tracking-lost screen's camera job; a RUNNING tracker survived all of
 * it (233 sessions). Every surviving start had opened the cameras 10.3 s or more after that
 * bring-up (6.6 s or more after SafetyNoticeFinished). The gate is clear only when the kernel
 * log is being read, the image stream has no system owner, the system's camera job is idle (the
 * two levels the log alone tells; the system's screens are the game's own status, hand_clear_why
 * and isOutOfVrPlayArea), the headset was not just put on (the
 * tracking-lost screen's camera job came 1 s after SetHmdMountStatus 1 in 15 of 31), the tracker
 * has not been relocalising for 3 s and has been stable since its last SLAM reset (a headset-off
 * replay may never log it: 10 s at most), and the last end of the system's camera work is 12 s old
 * (any VrCamera, system hmd2_image, notice, play-area or system-screen line stamps it): the
 * c-stock hang's start was about 10 s after its bring-up, the nearest surviving start 10.3 s
 * (the constants below, tools/hand/gate_check.py).
 * LIVE (step 2, after three watch-only console runs: the old rule would have started 5.5 s after
 * the system released the cameras in each start-up setup, the gate held until 12 s; it went not
 * clear 0.7 s before each setup's camera job): the start comes at the first moment the start
 * rule (worn, no system screen, 5 s) and the gate hold together (wait_clear_moment), and the gate
 * is asked again, from a fresh block, right before the credentials are raised (start()). Never
 * clear: never a start.
 * With agent.hands=0 the rule and the gate run watch only, as in step 1. A thread logs the
 * gate's verdicts and what it added to the start's wait, in both modes. */
/* The constants, as tools/hand/gate_check.py chose them over the old logs (every known hang held;
 * the 212 surviving starts delayed 1.5 s in the median, 3.6 s at p90, 11.4 s at most):
 *   floor 12: c-stock's start was 10.3 s after its bring-up (+-0.5 s), the nearest surviving start
 *   10.3 s too; 11 holds c-stock by 0.7 s, 15 costs the median start 4.5 s.
 *   relocal 3 s, the lines seen 0-3 s before a tracking-lost camera job: 10 s cost the median
 *   start 3 s more (the bring-up's own SLAM reset/latch lines 8.8 s before it).
 *   stable 10 s: 30 cost 10 sessions (no "tracking stable" after their SLAM reset) up to 28 s. */
#define GATE_BRINGUP_US 12000000 /* the end of the system's camera work this long ago */
#define GATE_RELOCAL_US 3000000  /* the tracker's last relocalising line this long ago */
#define GATE_MOUNT_US 5000000    /* the headset put on this long ago */
#define GATE_STABLE_US 10000000  /* "tracking stable" awaited this long after a SLAM reset */
#define GATE_MISSED_US 15000000  /* kernel log lines possibly missed this long ago */
#define GATE_MARK_US 3000000     /* the injector's newest marker line seen this long ago at most */
#define GATE_CONFIRM_US 300000   /* a clear verdict confirmed by a block written within this */
#define GATE_TELL_US 20000000    /* the system's state unknown this long into a hold: the player told */
#define GATE_FEED_US 3000000     /* the injector's last kern.msgbuf snapshot, and its last write */
#define GATE_FRESH_US 500000     /* a fresh block (the check before the credentials): its snapshot
                                  * this close to its write (the poll is 200 ms, the write 100) */
#define GATE_FRESH_WAIT_US 1000000 /* a fresh block awaited this long, else no start now */
#define GATE_LINES 80            /* verdict changes logged, then one line in 50 */
#define GATE_WAIT_LOG_US 30000000 /* while the gate holds a start the rule would make: a line this often */
#define GATE_GAZE_FIRST_US 60000000 /* the gate holding a start this long: eye tracking (fov.c) no longer waits for it */
#define HAND_CAMERA_UP_US 3000000 /* the credentials kept raised this long at most for the camera stream */

enum {
  GATE_CLEAR, GATE_NO_KLOG, GATE_STALE, GATE_NO_USERLAND, GATE_NO_MARK, GATE_MISSED, GATE_SYS_STREAM, GATE_XFD_BUSY,
  GATE_LEVELS, GATE_SCREEN, GATE_NO_STATUS, GATE_OUT_OF_AREA, GATE_MOUNT, GATE_RELOCAL, GATE_NOT_STABLE,
  GATE_BRINGUP, GATE_N
};
static const char *const k_gate_text[GATE_N] = {
    "clear",
    "no kernel log (the injector reads no kern.msgbuf, or is an older one)",
    "no fresh word from the kernel log within a second",
    "the kernel message buffer carries no system lines",
    "the injector's marker lines are not seen in the kernel log",
    "kernel log lines possibly missed less than 15 s ago",
    "the system has the image stream",
    "the system's camera job is running",
    "the system's camera state is not known (since the game's start, or lines were lost): its lines will say",
    "a system VR screen has the focus (the kernel log's focus lines)",
    "the game's system status could not be read (sceSystemServiceGetStatus)",
    "out of the play area",
    "the headset was just put on",
    "the tracker is relocalising",
    "tracking not stable since the tracker started over",
    "the system's camera or screen work was less than 12 s ago",
};

static volatile u64 g_gate_old_rule_at; /* the start rule's moment (process time), 0 before */
static volatile u32 g_hand_started;      /* the palm tracker runs: the gate's watch is over */

/* The two camera starts, the palm tracker's (the game's credentials raised) and eye tracking's
 * (fov.c), never beside each other: the one starting holds this. */
static volatile u32 g_camera_start_lock; /* 0 free, else the owner (CAMERA_START_*) */
static volatile u64 g_camera_start_given_at; /* the last give after a start that happened */
static volatile u32 g_camera_start_given_by; /* by whom */
#define CAMERA_START_RECENT_US 5000000
int camera_start_take(u32 who) {
  u32 zero = 0;
  return __atomic_compare_exchange_n(&g_camera_start_lock, &zero, who, 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE);
}
void camera_start_give(u32 who, int started) {
  u32 mine = who;
  if (__atomic_load_n(&g_camera_start_lock, __ATOMIC_ACQUIRE) != who)
    return; /* not the owner: nothing to give, nothing stamped */
  if (started) { /* (stamped before the give, so a taker after it sees it) */
    __atomic_store_n(&g_camera_start_given_by, who, __ATOMIC_RELEASE);
    __atomic_store_n(&g_camera_start_given_at, sceKernelGetProcessTime(), __ATOMIC_RELEASE);
  }
  __atomic_compare_exchange_n(&g_camera_start_lock, &mine, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
int camera_start_recent(u32 who) {
  u64 given = __atomic_load_n(&g_camera_start_given_at, __ATOMIC_ACQUIRE);
  u32 owner = __atomic_load_n(&g_camera_start_lock, __ATOMIC_ACQUIRE);
  return (owner && owner != who) ||
         (given && __atomic_load_n(&g_camera_start_given_by, __ATOMIC_ACQUIRE) != who &&
          sceKernelGetProcessTime() - given < CAMERA_START_RECENT_US);
}

/* A consistent copy of the injector's block: it writes seq odd, the body, seq even (three
 * copyins, each a kernel call complete before the next, payload/main.c publish_cam_state); a copy
 * is good when seq was even before and the same after it (the fence keeps the second read after
 * the copy: a seqlock's reader). */
static int read_cam_state(struct cam_state *out) {
  for (int tries = 0; tries < 8; tries++) {
    u64 s0 = __atomic_load_n(&g_shared.cam.seq, __ATOMIC_ACQUIRE);
    if (!(s0 & 1)) {
      memcpy(out, (const void *)&g_shared.cam, sizeof *out);
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (__atomic_load_n(&g_shared.cam.seq, __ATOMIC_ACQUIRE) == s0)
        return 1;
    }
    sceKernelUsleep(500);
  }
  return 0;
}

/* an age on the block's clock, 0 (fresh) when the clock runs behind the event */
static u64 age_of(u64 t, u64 at) { return t >= at ? t - at : 0; }

/* what a caller of gate_eval keeps between calls: the injector's instance, and when the block's
 * seq last moved, by our clock */
struct gate_ctx {
  u64 instance, last_seq, seq_at;
};

/* The verdict at `now` (process time) from a copy of the block already taken. The block is alive
 * when its seq moved within GATE_FEED_US (our clock, per caller) and the reader's last read is
 * within GATE_FEED_US of the block's own time; an injector instance not seen before (a newer one
 * adopted the game, its seq from 0) starts the count over. Its marker lines must be seen: the
 * newest written within GATE_MARK_US (the proof the buffer is read live, end to end). The
 * system's status (sceSystemServiceGetStatus) for isOutOfVrPlayArea. */
static int gate_judge(struct gate_ctx *g, const struct cam_state *c, u64 now) {
  if (c->version != CAM_STATE_VERSION || c->size != sizeof *c)
    return GATE_NO_KLOG;
  if (c->instance != g->instance)
    g->instance = c->instance, g->last_seq = c->seq, g->seq_at = 0; /* over again */
  else if (c->seq != g->last_seq)
    g->last_seq = c->seq, g->seq_at = now;
  if (!g->seq_at || age_of(now, g->seq_at) > GATE_FEED_US || !c->feed_at ||
      age_of(c->now, c->feed_at) > GATE_FEED_US)
    return GATE_NO_KLOG;
  u64 t = c->now; /* the block's own clock for its ages */
  if (!c->userland)
    return GATE_NO_USERLAND;
  /* the markers seen lately: the newest seen within GATE_MARK_US (they are written each second;
   * one seen and then no more is a dead reading) and no more than two behind the newest written */
  if (!c->mark_seen_at || age_of(t, c->mark_seen_at) > GATE_MARK_US ||
      c->mark_written_seq - c->mark_seen_seq >= 3)
    return GATE_NO_MARK;
  if (c->missed_at && age_of(t, c->missed_at) < GATE_MISSED_US)
    return GATE_MISSED;
  if (c->img_pri != CAM_PRI_NONE && c->img_pri != -1 && c->img_mode != 1)
    return GATE_SYS_STREAM;
  if (c->xfd == CAM_XFD_BUSY)
    return GATE_XFD_BUSY;
  /* the camera job and the stream not known (a new game, or lines lost since): only a line makes
   * them known, never time; the system's bring-up at every game's VR start writes them. (The
   * system's screens are the game's own status: hand_clear_why, and isOutOfVrPlayArea below) */
  if (c->xfd == CAM_XFD_NONE || !c->img_known)
    return GATE_LEVELS;
  if (c->screen_up) /* (the second signal beside the game's status, which the start rule reads) */
    return GATE_SCREEN;
  u8 st[512];
  memset(st, 0, sizeof st);
  if (!sceSystemServiceGetStatus || sceSystemServiceGetStatus(st) != 0)
    return GATE_NO_STATUS; /* (never seen: rc 0 in every console run; closed, not open, if it were) */
  if (st[8])
    return GATE_OUT_OF_AREA;
  if (c->mount == 1 && c->mount_at && age_of(t, c->mount_at) < GATE_MOUNT_US)
    return GATE_MOUNT;
  if (c->relocal_at && age_of(t, c->relocal_at) < GATE_RELOCAL_US)
    return GATE_RELOCAL;
  if (c->restart_at && !c->stable_after_reset && age_of(t, c->restart_at) < GATE_STABLE_US)
    return GATE_NOT_STABLE;
  if (c->bringup_at && age_of(t, c->bringup_at) < GATE_BRINGUP_US)
    return GATE_BRINGUP;
  return GATE_CLEAR;
}

/* The verdict from a copy taken now; a clear one is confirmed by a second copy the injector wrote
 * after the first (its seq advanced) within GATE_CONFIRM_US, judged again: a dead injector's last
 * block, however recent its seq looked to this caller, never says clear. */
static int gate_eval(struct gate_ctx *g, struct cam_state *c, u64 now) {
  if (!read_cam_state(c))
    return GATE_NO_KLOG;
  /* A caller that asks seldom (the hand thread, only once the start rule is clear) has no recent
   * sight of the seq moving: watch it move first, up to GATE_CONFIRM_US, so its verdict is the
   * block's and not "no kernel log" for want of a second look. A dead injector still reads
   * GATE_NO_KLOG (the seq never moves), only GATE_CONFIRM_US later. */
  if (c->instance != g->instance || !g->seq_at || age_of(now, g->seq_at) > GATE_FEED_US) {
    u64 s0 = c->seq, inst = c->instance, t0 = now;
    g->instance = inst, g->last_seq = s0, g->seq_at = 0;
    struct cam_state again;
    while (age_of(now, t0) < GATE_CONFIRM_US) {
      sceKernelUsleep(20000);
      now = sceKernelGetProcessTime();
      if (!read_cam_state(&again))
        continue; /* (a torn copy is never judged or logged: *c stays the last good one) */
      *c = again;
      if (c->instance == inst && c->seq != s0)
        break;
    }
  }
  int why = gate_judge(g, c, now);
  if (why != GATE_CLEAR)
    return why;
  u64 s0 = c->seq, inst = c->instance, t0 = now;
  for (;;) {
    sceKernelUsleep(20000);
    now = sceKernelGetProcessTime();
    if (read_cam_state(c) && c->instance == inst && c->seq != s0)
      return gate_judge(g, c, now);
    if (age_of(now, t0) >= GATE_CONFIRM_US)
      return GATE_NO_KLOG;
  }
}

/* The verdict from a FRESH block: one the injector published after this call began, with the
 * reader's snapshot within GATE_FRESH_US of it (a poll); waited for up to GATE_FRESH_WAIT_US,
 * else GATE_STALE. For the check right before the credentials: a block up to 3 s old can say
 * clear of a system camera job that has begun since. */
static int gate_eval_fresh(struct gate_ctx *g, struct cam_state *c, u64 *now) {
  u64 t0 = sceKernelGetProcessTime(), s0, inst;
  for (;;) { /* the baseline: an even seq (a write under way ends first), and the instance */
    *now = sceKernelGetProcessTime();
    if (read_cam_state(c)) {
      s0 = c->seq, inst = c->instance;
      break;
    }
    if (age_of(*now, t0) >= GATE_FRESH_WAIT_US)
      return GATE_STALE;
    sceKernelUsleep(1000);
  }
  for (;;) { /* a write begun after that: the same instance, seq two up or more */
    *now = sceKernelGetProcessTime();
    if (read_cam_state(c) && c->instance == inst && c->seq >= s0 + 2 &&
        c->version == CAM_STATE_VERSION && age_of(c->now, c->feed_at) <= GATE_FRESH_US)
      return gate_judge(g, c, *now); /* (this copy, once) */
    if (age_of(*now, t0) >= GATE_FRESH_WAIT_US)
      return GATE_STALE;
    sceKernelUsleep(20000);
  }
}

/* the state in a line: how old each last event is (seconds ago, by the block's clock; 0 never).
 * Kept short: with the longest reason it must fit LOG's 510 bytes */
static u32 gate_describe(char *out, u32 n, const struct cam_state *c) {
  u64 t = c->now;
#define AGE(at) (u32)((at) ? age_of(t, at) / 1000000 : 0), (u32)((at) ? age_of(t, at) / 1000 % 1000 : 0)
#define AGE_FMT "%u.%03u"
  u32 at = agent_snprintf(out, n, "ages in s: image %s mode %u pid %u pri %d " AGE_FMT ", camera job %s "
                          AGE_FMT ", screen %s, screen line " AGE_FMT ", mount %u "
                          AGE_FMT ", bring-up end " AGE_FMT ", relocal " AGE_FMT
                          ", SLAM reset " AGE_FMT ", stable " AGE_FMT ", msgbuf %u lines "
                          AGE_FMT ", %u polls, %u misses (last " AGE_FMT "), marker %u/%u "
                          AGE_FMT ", seq %lu",
                          c->img_known ? "known" : "unknown", c->img_mode, c->img_pid,
                          c->img_pri == CAM_PRI_NONE ? 0 : c->img_pri, AGE(c->img_at),
                          c->xfd == CAM_XFD_BUSY ? "busy" : c->xfd == CAM_XFD_IDLE ? "idle" : "unseen",
                          AGE(c->xfd_at), c->screen_up ? "focused" : "none", AGE(c->screen_at), c->mount,
                          AGE(c->mount_at), AGE(c->bringup_at), AGE(c->relocal_at), AGE(c->slam_at),
                          AGE(c->stable_at), c->lines, AGE(c->line_at), c->polls, c->misses,
                          AGE(c->missed_at), c->mark_seen_seq, c->mark_written_seq,
                          AGE(c->mark_seen_at), c->seq);
#undef AGE
#undef AGE_FMT
  return at;
}

static void gate_log(const char *when, int why, u64 now, const struct cam_state *c) {
  char st[400];
  st[0] = 0;
  if (why != GATE_NO_KLOG && why != GATE_STALE)
    gate_describe(st, sizeof st, c);
  LOG("hand-gate: %s (%u.%03u s): %s%s%s%s", when, SECS(now), why == GATE_CLEAR ? "clear" : "not clear: ",
      why == GATE_CLEAR ? "" : k_gate_text[why], st[0] ? "; " : "", st);
}

/* the verdict now, in the log (the hand thread's own context) */
static struct gate_ctx g_hand_gate;
static int hand_gate_report(const char *when) {
  struct cam_state c;
  u64 now = sceKernelGetProcessTime();
  int why = gate_eval(&g_hand_gate, &c, now);
  gate_log(when, why, now, &c);
  return why;
}

/* Watches the gate all session (every 250 ms): each change of its verdict goes to the log with
 * the reason and the state, and once the start rule has had its moment, when the gate was or
 * became clear: "old rule clear at X s, gate clear at Y s (extra Z s)". */
static void *hand_gate_thread(void *arg) {
  (void)arg;
  ps5compat_mark_thread();
  struct gate_ctx g = {0, 0};
  int last = -1;
  u32 changes = 0;
  u64 clear_since = 0; /* the current clear stretch began (process time), 0 while not clear */
  int reported = 0;
  LOG("hand-gate: watching the headset cameras' state (%s)",
      g_opt_hands ? "live: the start needs the start rule and the gate together"
                  : "watch only: agent.hands=0, no start");
  while (g_shared.cmd != CMD_QUIT) {
    if (g_hand_started) {
      LOG("hand-gate: the palm tracker has started: the gate is no longer watched");
      return 0;
    }
    struct cam_state c;
    u64 now = sceKernelGetProcessTime();
    int why = gate_eval(&g, &c, now);
    if (why == GATE_CLEAR) {
      if (!clear_since)
        clear_since = now;
    } else {
      clear_since = 0;
    }
    if (why != last) {
      changes++;
      if (changes <= GATE_LINES || changes % 50 == 0) {
        gate_log("verdict", why, now, &c);
        if (changes == GATE_LINES)
          LOG("hand-gate: %u verdict changes logged; one line in 50 from here", changes);
      }
      last = why;
    }
    u64 old = __atomic_load_n(&g_gate_old_rule_at, __ATOMIC_ACQUIRE);
    if (old && !reported && why == GATE_CLEAR) {
      u64 y = clear_since <= old ? old : clear_since;
      LOG("hand-gate: old rule clear at %u.%03u s, gate clear at %u.%03u s (extra %u.%03u s)",
          SECS(old), SECS(y), SECS(y - old));
      reported = 1;
    }
    sceKernelUsleep(250000);
  }
  return 0;
}

/* The moment to start: the cameras are taken (the game's credentials raised) only once the
 * headset has stayed worn, with no system screen up, for HAND_SETTLE. Allison Snow's first two
 * starts (2026-11-04) took the console down within 2 s of the headset going on while PS VR2's
 * safety notice and play-area setup had the cameras; Hikari's starts that survived the same
 * screens began before the headset went on and took the camera images only once the setup had
 * finished (the kernel log's hmd2_leddet and hmd2_image opens). A system screen shows as the game's status
 * (isSystemUiOverlaid or isInBackgroundExecution, the real ones: hooks.c sets them for the game)
 * or as the controller held by the system (hooks.c note_intercepted). Watched through a fresh
 * restart's notice and setup (the agent's watch-only mode then, 2026-11-04): the controller held and the
 * background flag from the notice to 3 s after SafetyNoticeFinished, overlaid only for the PS
 * menu. */
#define HAND_SETTLE 5000000 /* us */
/* why a camera start must wait now: 0 it needn't (worn, no system screen, the controller the
 * game's), 1 the headset off, 2 a system screen up, 3 the controller held by one, 4 the game in the
 * background; st (512 bytes) gets the system status, *src its call's result (fov.c's eye tracking
 * waits by the same rule) */
int hand_clear_why(u8 *st, int *src) {
  memset(st, 0, 512);
  hooks_pad_held(); /* the controller as of now too (a paused game may not read it) */
  u64 held = __atomic_load_n(&g_pad_intercepted_at, __ATOMIC_ACQUIRE); /* before now: never ahead */
  u64 now = sceKernelGetProcessTime();
  *src = sceSystemServiceGetStatus ? sceSystemServiceGetStatus(st) : -1;
  /* isSystemUiOverlaid, or isInBackgroundExecution: PS VR2's safety notice and play-area setup
   * set the latter (with the controller held) and never the former (Allison Snow, allison5) */
  int overlaid = *src == 0 && st[4], background = *src == 0 && st[5];
  return g_vr.hmd_off ? 1 : overlaid ? 2 : held && now - held < 1000000 ? 3 : background ? 4 : 0;
}
volatile u32 g_hand_settled; /* the palm tracker's start is over: started, failed or never to be */
/* what the start rule's wait keeps across a start the gate turned back (the rule's settle goes on) */
struct clear_state {
  int last;
  u64 since, gate_log_at;
  u64 hold_since;      /* the gate has held a start the rule would make since (0: not holding):
                        * set when the rule is clear and the gate not, over when the rule's own
                        * condition breaks (the log's "this hold", the player told after 20 s) */
  u32 logged_status, logged_why, last_sum;
  u32 turnbacks;       /* starts the gate turned back right before the credentials */
  u64 turnback_log_at; /* (logged: the first 3, then one in 30 s) */
  u32 told;            /* the player told once that hand tracking waits for the system's state */
  u8 last_st[16];
};
/* The palm tracker not started GATE_GAZE_FIRST_US after the start rule's FIRST clear moment of the
 * session (the gate holding, turn-backs, the rule's own breaks in between: whatever the path):
 * eye tracking, if any, no longer waits for it. (cs->hold_since measures the current hold, for
 * the log.) */
static void gate_hold_release(struct clear_state *cs, u64 now) {
  u64 first = __atomic_load_n(&g_gate_old_rule_at, __ATOMIC_ACQUIRE);
  if (first && !g_hand_settled && now - first >= GATE_GAZE_FIRST_US) {
    LOG("hand: the cameras' gate has held the start for %u s: eye tracking, if any, goes "
        "first; the palm tracker still waits", (u32)(GATE_GAZE_FIRST_US / 1000000));
    g_hand_settled = 1; /* (fov.c still never starts beside a start: camera_start_take) */
  }
}
static int wait_clear_moment(struct clear_state *cs) {
  static const char *const why_text[] = {"the headset is worn, no system screen is up",
                                         "the headset is off", "a system screen is up",
                                         "a system screen has the controller",
                                         "the game is in the background (a system screen)"};
  const u32 cap = 30;
  while (g_shared.cmd != CMD_QUIT) {
    u8 st[512];
    int src;
    int why = hand_clear_why(st, &src);
    u64 now = sceKernelGetProcessTime();
    /* the raw status as it changes: eventNum, isSystemUiOverlaid, isInBackgroundExecution,
     * isCpuMode7CpuNormal, isGameLiveStreamingOnAir, isOutOfVrPlayArea, and whether anything
     * past them moved (a sum): which of it PS VR2's screens show in */
    u32 sum = 0;
    for (u32 i = sizeof cs->last_st; i < sizeof st; i++) sum = sum * 31 + st[i];
    if (src != 0)
      st[0] = st[1] = st[2] = st[3] = 0xee;
    if ((memcmp(st, cs->last_st, sizeof cs->last_st) != 0 || sum != cs->last_sum) && cs->logged_status < cap) {
      LOG_DETAIL("hand: system status at %u.%03u s: rc 0x%x, events %d, overlaid %u, background %u, "
          "cpu7 %u, streaming %u, out of play area %u, bytes 9-15 %02x%02x%02x%02x%02x%02x%02x%s",
          SECS(now), src, *(s32 *)st, st[4], st[5], st[6], st[7], st[8], st[9], st[10], st[11],
          st[12], st[13], st[14], st[15], sum != cs->last_sum && cs->logged_status ? ", the rest changed" : "");
      memcpy(cs->last_st, st, sizeof cs->last_st);
      cs->last_sum = sum;
      cs->logged_status++;
    }
    if (why != cs->last) {
      if (!why) /* the clear moment always: the reason for the start */
        LOG("hand: %s (%u.%03u s): %s", why_text[why], SECS(now),
            g_opt_hands ? "starting in 5 s" : "the start rule's 5 s (watch only, agent.hands=0)");
      else if (cs->logged_why++ < cap) /* what it waits for: detail */
        LOG_DETAIL("hand: %s (%u.%03u s)", why_text[why], SECS(now));
      cs->last = why;
      cs->since = now;
      if (why)
        cs->hold_since = 0; /* the rule's own condition broke: a hold, if any, is over */
    }
    if (!why && now - cs->since >= HAND_SETTLE) {
      /* the start rule's moment (once: the gate thread's "extra" line counts from it) */
      if (!__atomic_load_n(&g_gate_old_rule_at, __ATOMIC_ACQUIRE))
        __atomic_store_n(&g_gate_old_rule_at, now, __ATOMIC_RELEASE);
      if (!g_opt_hands)
        return 0; /* watch only */
      /* live: the cameras' gate too, at the same moment; else keep waiting (the rule's 5 s go
       * on unless its own condition breaks), and never start on a timeout */
      struct cam_state c;
      int gw = gate_eval(&g_hand_gate, &c, now);
      if (gw == GATE_CLEAR) {
        cs->hold_since = 0; /* this hold is over */
        return 0;
      }
      if (!cs->hold_since)
        cs->hold_since = now;
      if (gw == GATE_LEVELS && !cs->told && now - cs->hold_since >= GATE_TELL_US) {
        /* the system's state unknown for 20 s with the start otherwise due (lines lost mid-game,
         * or a start-up without the bring-up's lines): the player told once. The headset off and
         * on makes the system log a camera job in a tenth of the cases, its bring-up in a few;
         * the game started again always does (the levels are known again by its bring-up) */
        cs->told = 1;
        LOG("hand: the system's state unknown for %u s with the start due: the player told",
            (u32)(GATE_TELL_US / 1000000));
        agent_call(CMD_NOTIFY,
                   (u64) "SummerLessonVR2: hand tracking is waiting for the system's camera state. "
                         "Take the headset off and put it back on; if the hands don't start, close "
                         "the game and start it again.",
                   0, 0, 0, 3000, 0, 0);
      }
      if (!cs->gate_log_at || now - cs->gate_log_at >= GATE_WAIT_LOG_US) {
        LOG("hand: the start rule is clear (%u.%03u s), the cameras' gate is not: %s; waiting "
            "(this hold %u s)", SECS(now), k_gate_text[gw], (u32)((now - cs->hold_since) / 1000000));
        cs->gate_log_at = now;
      }
    }
    gate_hold_release(cs, now); /* (every poll: a held rule, a broken one, all the same) */
    sceKernelUsleep(250000);
  }
  return -1;
}

/* -2: no start now (eye tracking starting this moment, or the gate not clear from a fresh block
 * right before the credentials), -3: the start rule itself broke right before them (its reason in
 * g_start_break_why): back to waiting, nothing taken but the memory. quiet: the turn-back's lines
 * left out (hand_thread rate-limits them). */
static int g_start_break_why;
static int start(int quiet) {
  /* the camera-start lock first (eye tracking's start holds it for a moment), then not within
   * 5 s of eye tracking's start either (as it keeps off ours) */
  if (!camera_start_take(CAMERA_START_PALM)) {
    if (!quiet)
      LOG("hand: no start now: eye tracking is starting; waiting again");
    return -2;
  }
  if (camera_start_recent(CAMERA_START_PALM)) {
    camera_start_give(CAMERA_START_PALM, 0);
    if (!quiet)
      LOG("hand: no start now: eye tracking started less than 5 s ago; waiting again");
    return -2;
  }
  /* its memory, now that the gate has said clear once (a session where it never does takes
   * none); never from the reserve: with its work and pool there (step 2b, 2026-11-04 00:40) the
   * game stopped inside VrHand's start, credentials raised, the payload's loop stuck with it;
   * Ghost-VR found its first VrHand start took the console down from that segment (its hand.c
   * game_dalloc). Direct memory has no free here: static, kept across a turned-back start */
  static void *work;
  if (!work)
    work = vr2_dalloc(HAND_WORK, 0x10000);
  if (work && !g_pool)
    g_pool = vr2_dalloc(POOL_SIZE, 0x10000);
  if (!work || !g_pool) {
    camera_start_give(CAMERA_START_PALM, 0);
    LOG("hand: no direct memory for hand tracking (%u MB)", (HAND_WORK + POOL_SIZE) >> 20);
    return -1;
  }
  g_pool_used = FENCE_BYTES;
  /* the gate from a fresh block, last, right before the credentials: the cameras' state may have
   * moved since the moment both conditions held (the start rule's 250 ms poll, the system's own
   * events) */
  struct cam_state c;
  u64 now;
  int why = gate_eval_fresh(&g_hand_gate, &c, &now);
  if (!quiet || why == GATE_CLEAR)
    gate_log("before CMD_CAMERA, from a fresh block", why, now, &c);
  if (why != GATE_CLEAR) {
    camera_start_give(CAMERA_START_PALM, 0);
    if (!quiet)
      LOG("hand: no start now: the cameras' gate %s before the credentials; waiting again",
          why == GATE_STALE ? "had no fresh word" : "closed");
    return -2;
  }
  u32 game_pid = c.game_pid;
  { /* the game's own status once more, the authority on the system's screens: not clear, no
     * credentials */
    u8 st[512];
    int src, w = hand_clear_why(st, &src);
    if (w != 0) {
      camera_start_give(CAMERA_START_PALM, 0);
      g_start_break_why = w; /* (hand_thread: the rule's settle starts over from this) */
      if (!quiet)
        LOG("hand: no start now: the start rule broke before the credentials (%d); waiting again", w);
      return -3;
    }
  }
  LOG("hand: starting: the game's credentials raised for the cameras next");
  u64 cam = (u64)-1, t_init = sceKernelGetProcessTime();
  u64 raise_block_now = read_cam_state(&c) ? c.now : 0; /* the newest block before the raise */
  int crc = agent_call(CMD_CAMERA, 1, 0, 0, 0, 3000, 0, &cam);
  LOG("hand: headset cameras: credentials %s (%ld, request %d)", cam == 0 ? "raised" : "not raised",
      (s64)cam, crc);
  u8 p[0x40];
  memset(p, 0, sizeof p);
  *(u32 *)(p + 0x00) = sizeof p;
  *(s32 *)(p + 0x08) = 700;  /* below the reprojection and the game */
  *(u64 *)(p + 0x10) = 0x30; /* CPUs 4 and 5, which a PS4 process may use */
  *(u32 *)(p + 0x18) = HAND_PIPE;
  *(u32 *)(p + 0x1c) = HAND_QUEUE;
  *(void **)(p + 0x20) = work;
  *(u32 *)(p + 0x28) = HAND_WORK;
  *(u32 *)(p + 0x2c) = 0x10000;
  *(u32 *)(p + 0x30) = g_opt_hands == 30 ? 1 : HAND_30HZ; /* agent.hands=30 */
  int rc = sceVrHandInitialize(p);
  LOG("hand: VrHandInitialize -> 0x%x (%u shaders translated)", rc, g_nsh);
  int rc2 = -1;
  if (rc == 0) {
    u8 sp[0x10];
    memset(sp, 0, sizeof sp);
    *(u32 *)sp = sizeof sp;
    rc2 = sceVrHandStart(sp);
    LOG("hand: VrHandStart -> 0x%x", rc2);
  }
  u64 t_ref = sceKernelGetProcessTime(); /* after the start call: what counts as the stream up */
  /* The credentials stay raised until the camera stream is up: VrHand's camera-manager thread
   * configures and starts the stream (the ioctls, the GPU mapping) after sceVrHandStart returns,
   * and the start's hangs were all in that window. The stream is up when the kernel announces
   * THIS game's stream (its "hmd2_image: pid <game>, pri -1, ts, mode 4" line, written by the
   * stream's configuration, seen through the camera block with the game's pid: the surest sign;
   * an earlier game's line is reset by the injector at a new game and refused by the pid here),
   * when VrHand's camera manager or image lock speaks after the start call (the lib: lines), or
   * when a hand's result is tracked (state 3: frames are flowing); 3 s at most.
   * Asked back whatever the first answer said: an answer later than the request's 3 s reads as
   * "not raised" here while the injector did raise them, and the game would keep them all session.
   * The injector only restores what it raised (open_cameras), so this is harmless otherwise. */
  if (!HAND_KEEP_CAMERA) {
    const char *up = rc2 != 0 ? "the start failed" : 0;
    while (!up) {
      u64 t = sceKernelGetProcessTime();
      /* the stream's line only as NEW text (never read after a loss) of a poll begun later than
       * any block seen before the credentials were raised (the poll's own time, no line offset,
       * past raise_block_now) */
      if (read_cam_state(&c) && c.img_known && c.img_new && c.img_pri == -1 && c.img_mode == 4 &&
          game_pid && c.img_pid == game_pid && raise_block_now && c.img_poll_at > raise_block_now) {
        up = "the kernel announced the game's stream";
        break;
      }
      if (g_hand_lib_camera_at >= t_ref) {
        up = "VrHand's camera manager spoke";
        break;
      }
      u8 gp[0x30], res[HAND_RESULT] __attribute__((aligned(16)));
      memset(gp, 0, sizeof gp);
      *(u32 *)gp = sizeof gp;
      *(u64 *)(gp + 0x10) = t;
      if (sceVrHandGetResult(gp, res) == 0 && *(s32 *)(res + 0xc) == 3) {
        up = "a hand tracked";
        break;
      }
      if (t - t_ref >= HAND_CAMERA_UP_US) {
        up = "the 3 s cap";
        break;
      }
      sceKernelUsleep(20000);
    }
    u64 back = (u64)-1;
    agent_call(CMD_CAMERA, 0, 0, 0, 0, 3000, 0, &back);
    LOG("hand: the game's own credentials back after %u ms: %s (%ld%s)",
        (u32)((sceKernelGetProcessTime() - t_init) / 1000), up, (s64)back,
        cam == 0 ? "" : "; they were not raised as far as the first answer said");
  }
  camera_start_give(CAMERA_START_PALM, 1); /* (a start happened, worked or not) */
  if (rc2 != 0) {
    if (rc == 0)
      sceVrHandFinalize();
    return -1;
  }
  LOG("hand: tracking started (%s)",
      HAND_SUBMIT ? "on the GPU" : "dry run: its GPU work is not submitted");
  g_hand_started = 1;
  return 0;
}

/* One hand per call (+0x18). The result: +0xc the state (3 = tracked), then 26 joints of 0x60
 * bytes in OpenXR order (palm first), position at +0 in VrTracker2 space (metres). */
static void track(void) {
  static u8 res[HAND_RESULT] __attribute__((aligned(16)));
  int last[2] = {-2, -2};
  u32 ticks = 0, seen[2] = {0, 0}, changes = 0;
  while (g_shared.cmd != CMD_QUIT && !g_dead) {
    for (int h = 0; h < 2; h++) {
      u8 gp[0x30];
      memset(gp, 0, sizeof gp);
      *(u32 *)gp = sizeof gp;
      *(u64 *)(gp + 0x10) = sceKernelGetProcessTime();
      *(u32 *)(gp + 0x18) = (u32)h;
      int rc = sceVrHandGetResult(gp, res);
      int state = rc == 0 ? *(s32 *)(res + 0xc) : -1;
      if (state != last[h] && changes++ < 40)
        LOG("hand: %s hand state %d (0x%x)", h ? "right" : "left", state, rc);
      last[h] = state;
      if (state == 3) {
        publish_palm(h, (const float *)(res + 0x10));
        seen[h]++;
      }
    }
    if (++ticks == HAND_LOOKS) { /* 10 s */
      LOG_DETAIL("hand: seen %u%% / %u%% of the last 10 s; GPU: %u buffers, %u dispatches (%u dropped), "
          "%u draws dropped, %u graphics buffers, %u waits (%u timed out)",
          seen[0] * 100 / HAND_LOOKS, seen[1] * 100 / HAND_LOOKS, g_stats.buffers,
          g_stats.dispatches, g_stats.dropped,
          g_stats.draws, g_stats.gfx, g_stats.waits, g_stats.timeouts);
      ticks = seen[0] = seen[1] = 0;
    }
    sceKernelUsleep(HAND_POLL);
  }
  LOG("hand: VrHandStop 0x%x, VrHandFinalize 0x%x", sceVrHandStop(), sceVrHandFinalize());
}

/* Started once, then kept through the headset coming off and on (the system starts no camera
 * session of its own then: Hikari's runs xD and wm); a start that fails is not tried again. One
 * the gate turns back right before the credentials is not a failed start: it waits again, the
 * start rule's settle kept. */
static void *hand_thread(void *arg) {
  (void)arg;
  ps5compat_mark_thread();
  const struct astro_firmware *fw = astro_firmware(astro_hdr.firmware);
  if (g_opt_hands && (!fw || !fw->hand.name)) {
    LOG("hand: no hand tracking on this firmware yet; the controller stays at its place");
    g_hand_settled = 1;
    return 0;
  }
  if (g_opt_hands && !lib_ready()) {
    LOG("hand: libSceVrHand not loaded; the controller stays at its place");
    g_hand_settled = 1;
    return 0;
  }
  if (!g_opt_hands)
    g_hand_settled = 1; /* agent.hands=0: the start rule and the gate run watch only, no start */
  /* the gate's watcher, now that a start (or its watch) is to come */
  {
    u8 attr[64], thr[16];
    scePthreadAttrInit(attr);
    scePthreadAttrSetstacksize(attr, 64 * 1024);
    int rc = scePthreadCreate(thr, attr, hand_gate_thread, 0, "SummerLessonVR2 hand gate");
    scePthreadAttrDestroy(attr);
    if (rc)
      LOG("hand-gate: thread -> 0x%x", rc);
  }
  /* the headset first: its output running for 2 s, then the clear moment */
  while (gen1_refreshes() < 240 && g_shared.cmd != CMD_QUIT)
    sceKernelUsleep(100000);
  struct clear_state cs;
  memset(&cs, 0, sizeof cs);
  cs.last = -1;
  memset(cs.last_st, 0xff, sizeof cs.last_st);
  int rc;
  for (;;) {
    /* live: the moment the start rule and the cameras' gate hold together; watch only: the
     * start rule's moment (wait_clear_moment sets g_gate_old_rule_at for the gate thread) */
    if (wait_clear_moment(&cs) != 0)
      return 0;
    hand_gate_report(g_opt_hands ? "at the start" : "at the start rule's moment");
    if (!g_opt_hands) {
      LOG("hand: no start (agent.hands=0): the start rule and the gate were watched only");
      return 0;
    }
    u64 now = sceKernelGetProcessTime();
    int quiet = cs.turnbacks >= 3 && now - cs.turnback_log_at < GATE_WAIT_LOG_US;
    rc = start(quiet);
    if (rc != -2 && rc != -3)
      break;
    if (rc == -3) /* the rule broke: its state as wait_clear_moment would have it, settle over */
      cs.last = g_start_break_why, cs.since = sceKernelGetProcessTime(), cs.hold_since = 0;
    cs.turnbacks++;
    if (!quiet)
      cs.turnback_log_at = now;
    gate_hold_release(&cs, sceKernelGetProcessTime()); /* (a hold through turn-backs counts too) */
    sceKernelUsleep(250000);
  }
  g_hand_settled = 1;
  if (rc != 0) {
    LOG("hand: no hand tracking; the controller stays at its place");
    return 0;
  }
  track();
  return 0;
}

void hand_start(void) {
  u8 attr[64], thr[16];
  scePthreadAttrInit(attr);
  scePthreadAttrSetstacksize(attr, 256 * 1024);
  int rc = scePthreadCreate(thr, attr, hand_thread, 0, "SummerLessonVR2 hands");
  scePthreadAttrDestroy(attr);
  if (rc) {
    LOG("hand: thread -> 0x%x", rc);
    g_hand_settled = 1; /* (no start to wait for) */
  }
}
