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
 *    (CMD_CAMERA) while it opens them; they go back to the game's own once it has started.
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

/* The moment to start: the cameras are taken (the game's credentials raised) only once the
 * headset has stayed worn, with no system screen up, for HAND_SETTLE. Allison Snow's first two
 * starts (2026-11-04) took the console down within 2 s of the headset going on while PS VR2's
 * safety notice and play-area setup had the cameras; Hikari's starts that survived the same
 * screens began before the headset went on and took the camera images only once the setup had
 * finished (the kernel log's hmd2_leddet and hmd2_image opens). A system screen shows as the game's status
 * (isSystemUiOverlaid or isInBackgroundExecution, the real ones: hooks.c sets them for the game)
 * or as the controller held by the system (hooks.c note_intercepted). Watched through a fresh
 * restart's notice and setup (agent.handwatch, 2026-11-04): the controller held and the
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
/* the times in the lines below: the game's process time, for lining them up with the kernel
 * log's system screens (tools/testrun.sh's .ticks) */
#define SECS(t) (u32)((t) / 1000000), (u32)((t) / 1000 % 1000)
static int wait_clear_moment(void) {
  static const char *const why_text[] = {"the headset is worn, no system screen is up",
                                         "the headset is off", "a system screen is up",
                                         "a system screen has the controller",
                                         "the game is in the background (a system screen)"};
  const u32 cap = g_opt_handwatch ? 400 : 30;
  int last = -1, would = 0;
  u64 since = 0;
  u32 logged_status = 0, logged_why = 0, last_sum = 0;
  u8 last_st[16];
  memset(last_st, 0xff, sizeof last_st);
  while (g_shared.cmd != CMD_QUIT) {
    u8 st[512];
    int src;
    int why = hand_clear_why(st, &src);
    u64 now = sceKernelGetProcessTime();
    /* the raw status as it changes: eventNum, isSystemUiOverlaid, isInBackgroundExecution,
     * isCpuMode7CpuNormal, isGameLiveStreamingOnAir, isOutOfVrPlayArea, and whether anything
     * past them moved (a sum): which of it PS VR2's screens show in */
    u32 sum = 0;
    for (u32 i = sizeof last_st; i < sizeof st; i++) sum = sum * 31 + st[i];
    if (src != 0)
      st[0] = st[1] = st[2] = st[3] = 0xee;
    if ((memcmp(st, last_st, sizeof last_st) != 0 || sum != last_sum) && logged_status < cap) {
      LOG_DETAIL("hand: system status at %u.%03u s: rc 0x%x, events %d, overlaid %u, background %u, "
          "cpu7 %u, streaming %u, out of play area %u, bytes 9-15 %02x%02x%02x%02x%02x%02x%02x%s",
          SECS(now), src, *(s32 *)st, st[4], st[5], st[6], st[7], st[8], st[9], st[10], st[11],
          st[12], st[13], st[14], st[15], sum != last_sum && logged_status ? ", the rest changed" : "");
      memcpy(last_st, st, sizeof last_st);
      last_sum = sum;
      logged_status++;
    }
    if (why != last) {
      if (!why) /* the clear moment always: the reason for the start */
        LOG("hand: %s (%u.%03u s)%s", why_text[why], SECS(now),
            g_opt_handwatch ? ": agent.handwatch=1, watched only" : ": starting in 5 s");
      else if (logged_why++ < cap) /* what it waits for: detail */
        LOG_DETAIL("hand: %s (%u.%03u s)", why_text[why], SECS(now));
      last = why;
      since = now;
      would = 0;
    }
    if (!why && now - since >= HAND_SETTLE) {
      if (!g_opt_handwatch)
        return 0;
      if (!would) /* agent.handwatch=1: only watched */
        LOG("hand: would start now (%u.%03u s); agent.handwatch=1: not started", SECS(now));
      would = 1;
    }
    sceKernelUsleep(250000);
  }
  return -1;
}

static int start(void) {
  /* never from the reserve: with its work and pool there (step 2b, 2026-11-04 00:40) the game
   * stopped inside VrHand's start, credentials raised, the payload's loop stuck with it; Ghost-VR
   * found its first VrHand start took the console down from that segment (its hand.c game_dalloc) */
  void *work = vr2_dalloc(HAND_WORK, 0x10000);
  g_pool = vr2_dalloc(POOL_SIZE, 0x10000);
  if (!work || !g_pool) {
    LOG("hand: no direct memory for hand tracking (%u MB)", (HAND_WORK + POOL_SIZE) >> 20);
    return -1;
  }
  g_pool_used = FENCE_BYTES;
  LOG("hand: starting: the game's credentials raised for the cameras next");
  u64 cam = (u64)-1;
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
  /* Asked back whatever the first answer said: an answer later than the request's 3 s reads as
   * "not raised" here while the injector did raise them, and the game would keep them all session.
   * The injector only restores what it raised (open_cameras), so this is harmless otherwise. */
  if (!HAND_KEEP_CAMERA) {
    u64 back = (u64)-1;
    agent_call(CMD_CAMERA, 0, 0, 0, 0, 3000, 0, &back);
    LOG("hand: the game's own credentials back (%ld%s)", (s64)back,
        cam == 0 ? "" : "; they were not raised as far as the first answer said");
  }
  if (rc2 != 0) {
    if (rc == 0)
      sceVrHandFinalize();
    return -1;
  }
  LOG("hand: tracking started (%s)",
      HAND_SUBMIT ? "on the GPU" : "dry run: its GPU work is not submitted");
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
 * session of its own then: Hikari's runs xD and wm); a start that fails is not tried again. */
static void *hand_thread(void *arg) {
  (void)arg;
  ps5compat_mark_thread();
  const struct astro_firmware *fw = astro_firmware(astro_hdr.firmware);
  if (!fw || !fw->hand.name) {
    LOG("hand: no hand tracking on this firmware yet; the controller stays at its place");
    g_hand_settled = 1;
    return 0;
  }
  if (!lib_ready()) {
    LOG("hand: libSceVrHand not loaded; the controller stays at its place");
    g_hand_settled = 1;
    return 0;
  }
  /* the headset first: its output running for 2 s, then the clear moment */
  while (gen1_refreshes() < 240 && g_shared.cmd != CMD_QUIT)
    sceKernelUsleep(100000);
  if (wait_clear_moment() != 0)
    return 0;
  int rc = start();
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
