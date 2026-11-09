/* PS VR (PS4) emulation: the game's libSceHmd / libSceVrTracker / libSceCamera /
 * libSceHmdSetupDialog imports are pointed here. The headset, camera and tracker
 * report "connected"; frames the game hands to sceHmdReprojectionStart are kept
 * in g_vr for the PS VR2 output (vr2.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "agent.h"
#include "psvr1.h"
#include "vr2.h"

struct vr_state g_vr = {
    .user_id = -1,
    .ipd = 0.063f,
    /* PS VR2's own field of view (Hmd2 FOV of this headset; out, in, top, bottom), so the game
     * draws what PS VR2 shows: it asks once, at sceHmdOpen, before Hmd2 is up in the process.
     * A real PS VR reports 1.20743, 1.181346, 1.262872, 1.262872. */
    .title_fov = {1.8418f, 0.9472f, 1.3290f, 1.3290f},
    .force_modes = 1,
};

/* Original GOT values (the real library functions). */
#define H(name) static void *orig_##name;
#include "hooks.inc"
#undef H

/* ---------------------------------------------------------------- helpers */
static u64 now_us(void) { return sceKernelGetProcessTime(); }

static int once(volatile u32 *flag) {
  return __atomic_exchange_n(flag, 1, __ATOMIC_ACQ_REL) == 0;
}

#define CALL_ORIG(type, name, ...) (((type)orig_##name)(__VA_ARGS__))
#define LOG_FIRST(counter, n, ...)                                                               \
  do {                                                                                           \
    if (__atomic_fetch_add(&(counter), 1, __ATOMIC_RELAXED) < (n))                               \
      LOG(__VA_ARGS__);                                                                          \
  } while (0)

#define HMD_HANDLE 0x0f000000
static s32 g_main_port = -1; /* the game's main VideoOut port (bus 0), -1 until opened */
#define VBL_USER_ID 0x7a5b /* see "main port at 120 Hz" */

/* ---------------------------------------------------------------- libSceHmd */
static s32 h_sceHmdInitialize315(const void *param) {
  LOG("hmd: Initialize315(%p)", param);
  return 0;
}

static s32 h_sceHmdOpen(s32 user, s32 type, s32 index, void *param) {
  LOG("hmd: Open(user %d, type %d, index %d, %p) -> 0x%x", user, type, index, param, HMD_HANDLE);
  g_vr.user_id = user;
  return HMD_HANDLE;
}

static s32 h_sceHmdClose(s32 handle) {
  LOG("hmd: Close(0x%x)", handle);
  return 0;
}

static s32 h_sceHmdTerminate(void) {
  LOG("hmd: Terminate");
  return 0;
}

static s32 h_sceHmdGetDeviceInformation(struct hmd_device_information *info) {
  static u32 n;
  if (!info)
    return HMD_ERR_PARAMETER_NULL;
  agent_tick();
  s32 rc = orig_sceHmdGetDeviceInformation
               ? CALL_ORIG(s32(*)(void *), sceHmdGetDeviceInformation, info)
               : -1;
  s32 real_status = rc == 0 ? (s32)info->status : -1;
  s32 user = rc == 0 ? info->user_id : -1;
  if (g_vr.user_id < 0) {
    s32 u = -1;
    if (sceUserServiceGetInitialUser(&u) == 0 && u >= 0)
      g_vr.user_id = u;
  }
  if (user < 0)
    user = g_vr.user_id;
  memset(info, 0, sizeof *info);
  info->status = HMD_STATUS_READY;
  info->user_id = user >= 0 ? user : g_vr.user_id;
  info->panel_width = 1920; /* PS VR's: it only reaches the plugin's GetMonitorInfo (0x2f8eb00);
                             * the eye size is r.ScreenPercentage's (gpu_label_thread) */
  info->panel_height = 1080;
  info->latency_90hz = 13000;
  info->latency_120hz = 10000;
  info->hmu_mount = !g_vr.hmd_off;
  u32 k = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED);
  if (k < 4 || k % 600 == 0)
    LOG_DETAIL("hmd: GetDeviceInformation #%u (real rc 0x%x status %d) -> ready, user %d", k, rc,
        real_status, info->user_id);
  return 0;
}

/* Summer Lesson asks by handle once the headset is open (its periodic "still connected" check:
 * a status other than ready there is logged as a disconnection). */
static s32 h_sceHmdGetDeviceInformationByHandle(s32 handle, struct hmd_device_information *info) {
  static u32 n;
  s32 rc = h_sceHmdGetDeviceInformation(info);
  LOG_FIRST(n, 2, "hmd: GetDeviceInformationByHandle(0x%x) -> 0x%x (status %u)", handle, rc,
            info ? info->status : 0);
  return rc;
}

/* The order the game reads the structure in. A reading of Summer Lesson's uv maths (eboot
 * 0x341a9-0x34379) had it taking the first tangent as the inner one; run 1 measured the
 * opposite: given 0.9472 first, its left eye's uv offset was 0.1698 = 0.9472 * scale, so the
 * first tangent is the one at the left eye's left (outer) edge, as Astro Bot has it. 0: out, in. */
#define GAME_FOV_IN_FIRST 0
static s32 h_sceHmdGetFieldOfView(s32 handle, struct hmd_fov *fov) {
  if (!fov)
    return HMD_ERR_PARAMETER_NULL;
  *fov = g_vr.title_fov;
  if (GAME_FOV_IN_FIRST)
    fov->tan_out = g_vr.title_fov.tan_in, fov->tan_in = g_vr.title_fov.tan_out;
  LOG("hmd: GetFieldOfView(0x%x) -> %f %f %f %f (%s)", handle, fov->tan_out, fov->tan_in,
      fov->tan_top, fov->tan_bottom, GAME_FOV_IN_FIRST ? "inner first, as this game reads it"
                                                       : "outer first");
  return 0;
}

static s32 h_sceHmdGet2DEyeOffset(s32 handle, struct hmd_eye_offset *l, struct hmd_eye_offset *r) {
  if (!l || !r)
    return HMD_ERR_PARAMETER_NULL;
  memset(l, 0, sizeof *l);
  memset(r, 0, sizeof *r);
  l->x = -g_vr.ipd * 0.5f;
  r->x = g_vr.ipd * 0.5f;
  LOG("hmd: Get2DEyeOffset(0x%x) -> +-%f", handle, r->x);
  return 0;
}

/* ---------------------------------------------------------------- reprojection */
static void log_tsharp(const char *what, const u32 *t);
static s32 h_sceHmdReprojectionInitialize(const struct hmd_reprojection_resource *res, s32 type,
                                          void *option) {
  if (res)
    LOG("hmd: ReprojectionInitialize(type %d) onion %p garlic %p prio %d mask 0x%lx pipe %u "
        "queue %u",
        type, res->onion_buff, res->garlic_buff, res->thread_priority, res->cpu_affinity_mask,
        res->pipe_id, res->queue_id);
  if (res) {
    g_vr.reproj_prio = res->thread_priority;
    g_vr.reproj_affinity = res->cpu_affinity_mask;
  }
  g_vr.reproj_initialized = 1;
  return 0;
}

static s32 h_sceHmdReprojectionSetDisplayBuffers(s32 vo, s32 i0, s32 i1, void *option) {
  LOG("hmd: ReprojectionSetDisplayBuffers(vo 0x%x, %d, %d, %p)", vo, i0, i1, option);
  g_vr.display_vo = vo;
  g_vr.display_index[0] = i0;
  g_vr.display_index[1] = i1;
  return 0;
}

static s32 h_sceHmdReprojectionSetOutputMinColor(float r, float g, float b) {
  LOG("hmd: ReprojectionSetOutputMinColor(%f %f %f)", r, g, b);
  return 0;
}

static void *vblank_thread(void *arg);
static void raise_vblank(u32 k, int shown, int counted);
static int vbl_sleep(u64 until, int on_flip);

/* ---- the pace: a frame every two refreshes, as PS VR had it ----
 * This build changes nothing of the game's own timing (AstroBotVR2's 90 and 120 fps modes
 * patched that game's time step and frame timer; another game's are yet to be found in its
 * code). The vblank thread is woken as Hmd2 flips (gen1.c), so the game's refresh event follows
 * the flip within microseconds, and every 10 s the log says how Hmd2's flips went. */
volatile u32 g_refresh_hz;
static void *volatile g_wake_eq;
#define WAKE_FLIP 2
void hooks_wake(void) {
  void *eq = g_wake_eq;
  if (eq)
    sceKernelTriggerUserEvent(eq, WAKE_FLIP, 0);
}
volatile u32 g_late_pass, g_release_n;   /* vr2.c's late pass: nothing aims frames later here */
static volatile u32 g_shown, g_repeated; /* Hmd2's flips: a new frame, the same one again */

/* the flight recorder's counters (a snapshot a second, logged around a mark: rec_tick, below) */
static volatile u32 g_rec_shown, g_rec_again, g_rec_clockfb, g_rec_dmem_refused, g_rec_gpu_max;
static volatile u32 g_rec_ts_min = ~0u, g_rec_ts_max;
static volatile u32 g_pool_retries; /* the CPU pool's 224 MB retries refused (logged as a count) */
static u32 g_pool_granted; /* inset mode: the CPU pool's 224 MB given once (h_sceKernelAllocateDirectMemory) */
static u64 g_flex_min = ~0ull; /* the game's free flexible memory, its lowest this session */
extern volatile u32 g_rec_pose_max, g_rec_reseats; /* vr.c */
/* Frames never shown, counted exactly: a frame number missing from Hmd2's flips (vr2.c numbers the
 * frames it passes in order), and frames the hand-off itself skipped (hooks_note_unpassed). It was
 * "handed over minus shown" per 10 s window, which miscounted a frame handed over at a window's end
 * and shown in the next one (one "never shown" there, one more shown than handed over in the next:
 * none was lost), more often once frames wait for their drawing before the headset. */
static volatile u32 g_never, g_rec_never;
void hooks_note_unpassed(u32 n, u32 why) {
  __atomic_fetch_add(&g_never, n, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_rec_never, n, __ATOMIC_RELAXED);
  /* when and why (the first 60, then every 100th): one lost each ~1100 frames at 90 and 120 was
   * the engine's cap above the headset's refresh (docs/MULTIVIEW.md 28) */
  static volatile u32 said;
  u32 k = __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED);
  if (k < 60 || k % 100 == 0)
    LOG_DETAIL("hmd: %u frame(s) never shown (%s) at %lu us, frame %u handed over", n,
        why == 1 ? "the newest passed instead" : why == 2 ? "gone from the ring" : why == 3 ?
        "overwritten before passed" : "a flip skipped it", (u64)now_us(), g_vr.frame_w);
}
/* The frame's count: the game's end of frame (0x34b70) queues its GPU label write of this count
 * (0x34c0f), hands the frame over (0x34c97: we are in it), then counts on. A u32 (+0x360); 0 when
 * it cannot be told. vr_submit_frame stores it in the frame before the frame is published, so a
 * reader that sees the frame sees its count (stored after, the mirror could take a frame with the
 * count of four frames earlier, already drawn, and copy it unfinished). */
u64 hooks_frame_count(void) {
  if (!astro_hdr.viewport || !astro_hdr.game_base)
    return 0;
  return *(volatile u32 *)((u8 *)astro_hdr.game_base + astro_hdr.viewport + 0x360);
}
/* the game's GPU has drawn the frame of this count: its labels (viewport +0x2a8, +0x2b0, the two
 * buffers' "drawn" labels) have reached it; 1 too when it cannot be told (count 0) */
int hooks_frame_drawn(u64 count) {
  if (!count || !astro_hdr.viewport)
    return 1;
  u8 *vpp = (u8 *)astro_hdr.game_base + astro_hdr.viewport;
  volatile u64 *l0 = *(volatile u64 *volatile *)(vpp + 0x2a8), *l1 = *(volatile u64 *volatile *)(vpp + 0x2b0);
  u64 a = l0 ? *l0 : 0, b = l1 ? *l1 : 0;
  return (a > b ? a : b) >= count;
}
/* the game's newest "drawn" label (the larger of the two buffers'), 0 when it cannot be told */
u64 hooks_drawn_label(void) {
  if (!astro_hdr.viewport)
    return 0;
  u8 *vpp = (u8 *)astro_hdr.game_base + astro_hdr.viewport;
  volatile u64 *l0 = *(volatile u64 *volatile *)(vpp + 0x2a8), *l1 = *(volatile u64 *volatile *)(vpp + 0x2b0);
  u64 a = l0 ? *l0 : 0, b = l1 ? *l1 : 0;
  return a > b ? a : b;
}
/* Hmd2 flipped the headset (gen1.c): the vblank thread's wake, and what the flip shows */
/* How many refreshes each frame stayed on the headset (1, 2, 3, 4 or more), per 10 s: at 60 fps
 * every frame should stay two. Uneven (one, then three) is judder and doubled edges in motion, which
 * the totals above do not show (they come out the same). */
static volatile u32 g_held[5], g_held_run;
void hooks_flip(u32 flip_arg) {
  static u32 last;
  if (flip_arg == last)
    g_repeated++, g_rec_again++, g_held_run++;
  else {
    if (last) {
      u32 r = g_held_run;
      g_held[r >= 4 ? 4 : r]++;
    }
    g_held_run = 1;
    if (last && flip_arg > last + 1 && flip_arg - last < 64) /* passed, replaced before shown */
      hooks_note_unpassed(flip_arg - last - 1, 4);
    g_shown++, g_rec_shown++, last = flip_arg;
  }
  hooks_wake();
}

static s32 h_sceHmdReprojectionSetUserEventEnd(void *eq, s32 id) {
  LOG("hmd: ReprojectionSetUserEventEnd(eq %p, id %d)", eq, id);
  g_vr.end_eq = eq;
  g_vr.end_id = id;
  __atomic_store_n(&g_vr.end_set, 1, __ATOMIC_RELEASE);
  agent_tick();
  return 0;
}

/* Starts the agent's threads once there is something for them to do (the payload adopts an
 * agent already in a running game rather than putting a new one in). Called from frequent
 * hooks. */
void agent_tick(void) {
  static volatile u32 vblank_started;
  multiview_native_confirm(); /* (once, after the boot: the native rate from the agent's table) */
  /* Astro Bot gave the reprojection an end event (end_eq); Summer Lesson only registers its
   * vblank event (vbl_eq): either is the sign the game paces on PS VR's refreshes */
  if ((g_vr.end_eq || g_vr.vbl_eq) && !vblank_started && once(&vblank_started)) {
    u8 attr[64], thr[16];
    scePthreadAttrInit(attr);
    scePthreadAttrSetstacksize(attr, 256 * 1024);
    int rc = scePthreadCreate(thr, attr, vblank_thread, 0, "SummerLessonVR2 vblank");
    scePthreadAttrDestroy(attr);
    LOG("agent: vblank thread -> 0x%x (end eq %p id %d, vblank eq %p)", rc, g_vr.end_eq,
        g_vr.end_id, g_vr.vbl_eq);
  }
  /* PS VR2 once the game has been rendering VR frames for a while (not during its setup). */
  static volatile u32 vr2_once;
  if (g_vr.vr2_started != VR2_BUILD && g_vr.user_id >= 0 && g_vr.frames_in > 120 &&
      once(&vr2_once)) {
    g_vr.vr2_started = VR2_BUILD;
    vr2_start();
  }
}

static void log_tsharp(const char *what, const u32 *t) {
  /* GCN T#: base = dw0 | (dw1 & 0xff) << 32 (in 256-byte units), dfmt dw1[25:20], nfmt dw1[29:26],
   * width-1 dw2[13:0], height-1 dw2[27:14], tiling index dw3[24:20], type dw3[31:28],
   * pitch-1 dw4[26:13]. */
  u64 base = ((u64)t[0] | ((u64)(t[1] & 0xff) << 32)) << 8;
  LOG("  %s T# %08x %08x %08x %08x %08x %08x %08x %08x", what, t[0], t[1], t[2], t[3], t[4], t[5],
      t[6], t[7]);
  LOG("  %s base 0x%lx %ux%u dfmt %u nfmt %u tile %u type %u pitch %u", what, base,
      (t[2] & 0x3fff) + 1, ((t[2] >> 14) & 0x3fff) + 1, (t[1] >> 20) & 0x3f, (t[1] >> 26) & 0xf,
      (t[3] >> 20) & 0x1f, t[3] >> 28, ((t[4] >> 13) & 0x3fff) + 1);
}

/* Where the game's threads block, by call site (return address, from the eboot's base), each
 * 10 s (agent.waits: "where the game blocks", below) */
#define CENSUS_SITES 32
struct census {
  struct {
    u64 site;
    u32 n;
    u64 us;
  } e[CENSUS_SITES];
  volatile u32 lock;
};
static struct census g_blocks; /* the wait profiling, below */
static void census_add(struct census *c, u64 site, u64 us) {
#ifdef RELEASE
  (void)c, (void)site, (void)us;
  return; /* not reported in a release build: not counted */
#endif
  if (__atomic_exchange_n(&c->lock, 1, __ATOMIC_ACQUIRE))
    return; /* busy: this one not counted */
  for (int i = 0; i < CENSUS_SITES; i++)
    if (c->e[i].site == site || !c->e[i].site) {
      c->e[i].site = site, c->e[i].n++, c->e[i].us += us;
      break;
    }
  __atomic_store_n(&c->lock, 0, __ATOMIC_RELEASE);
}
static void census_report(struct census *c, const char *what) {
  char line[440];
  int o = 0;
  while (__atomic_exchange_n(&c->lock, 1, __ATOMIC_ACQUIRE))
    ;
  for (int i = 0; i < CENSUS_SITES && c->e[i].site; i++) {
    if (c->e[i].us >= 50000 && o < (int)sizeof line - 48)
      o += agent_snprintf(line + o, sizeof line - o, " 0x%lx:%ux/%lums", c->e[i].site, c->e[i].n,
                          c->e[i].us / 1000);
    c->e[i].site = 0, c->e[i].n = 0, c->e[i].us = 0;
  }
  __atomic_store_n(&c->lock, 0, __ATOMIC_RELEASE);
  line[o] = 0;
  LOG("%s: over 50 ms in 10 s (site:count/total):%s", what, o ? line : " none");
}
/* the game's sleeps (sceKernelUsleep, which Unreal's FPlatformProcess::Sleep at 0x2927d0
 * tail-jumps to): agent.rtime's samples (rtime.c) */
static s32 h_sceKernelUsleep(u32 us) {
  rtime_note(RT_WAIT, 0, 0);
  return CALL_ORIG(s32(*)(u32), sceKernelUsleep, us);
}

/* the game's GPU time a frame: its first command buffer after the last GPU-done event (its
 * graphics EOP, ident 0x40, on its own queue) to the next GPU-done event, the most and the mean
 * of each 10 s */
static volatile u64 g_first_sub, g_gpu_sum;
static volatile u32 g_gpu_max, g_gpu_n;
static void frame_submitted(void) {
  if (!g_first_sub)
    g_first_sub = now_us();
}

static void gpu_done(void);

/* The game's GPU time from its own labels: its RHI viewport (astro_hdr.viewport, a static of the
 * game's) keeps two pointers to u64 labels (+0x2a8, one a buffer) that its GPU sets to the frame's
 * count (+0x360) when the frame is drawn; the render thread waits for the other buffer's before
 * each frame (eboot 0x36fa0, 0x34a70). Polled every 200 us: a label's change is a frame done, timed
 * from the first command buffer after the change before. */
char g_cvars_text[4096]; /* cvars.txt, then the game's own cvars-<game>.txt (the payload) */
u32 g_cvars_len;
u32 g_opt_tvhmd, g_opt_hands = 60, g_opt_handwatch, g_opt_hmdrate, g_opt_cap, g_opt_mirrorboth, g_opt_recentre, g_opt_tvblank,
    g_opt_fingerprint, g_opt_trace, g_opt_waits, g_opt_rtime; /* debug tools: off unless cvars.txt says 1 */
u32 g_opt_newest = 1, g_opt_predict;

/* the text, once; the agent.* lines are taken on the spot (the rest when the game's console
 * manager is up: apply_cvars) */
void cvars_fetch(void) {
  static volatile u32 started, done;
  if (!once(&started)) { /* another thread fetches it: its result, up to 5 s */
    for (int i = 0; i < 500 && !__atomic_load_n(&done, __ATOMIC_ACQUIRE); i++)
      sceKernelUsleep(10000);
    return;
  }
  agent_call(CMD_CVARS, (u64)g_cvars_text, sizeof g_cvars_text, 0, 0, 3000, 0, 0);
  g_cvars_text[sizeof g_cvars_text - 1] = 0;
  g_cvars_len = (u32)strlen(g_cvars_text);
  if (!g_cvars_len) {
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    return;
  }
  LOG("cvars: %u bytes of cvars.txt", g_cvars_len);
  for (const char *p = g_cvars_text; *p;) {
    const char *line = p;
    while (*p && *p != '\n') p++;
    u32 n = (u32)(p - line);
    if (*p) p++;
    if (n < 7 || memcmp(line, "agent.", 6) != 0)
      continue;
    const char *eq = line + 6;
    while (eq < line + n && *eq != '=') eq++;
    if (eq >= line + n)
      continue;
    u32 v = 0;
    for (const char *d = eq + 1; d < line + n && *d >= '0' && *d <= '9'; d++) v = v * 10 + (u32)(*d - '0');
    u32 klen = (u32)(eq - line - 6);
    char key[32];
    if (klen >= sizeof key)
      klen = sizeof key - 1;
    memcpy(key, line + 6, klen);
    key[klen] = 0;
#ifdef RELEASE
    /* a release build takes the player's options only: the debug and test ones are ignored */
    static const char *const k_debug[] = {"handwatch", "hmdrate", "cap", "fingerprint", "trace",
                                          "waits", "rtime", "tvhmd", "insetviews", "capture", "capafter", "capdx", "capdy",
                                          "insetstate", "insetdebug", "inseteye", "probe",
                                          "insetpp", "insettaa", "taadump", "insettlv", "insetea", "insetocc", "insetlod", "insetpos", "insetppt",
                                          "insetsize", "insetwidth", "gazedot", "insetgaze",
                                          "insetonly", "insetwin", "insetblink", "gazehold",
                                          "vignette", "insetaam", "gazefilter", "gazemincut",
                                          "gazebeta"};
    int debug = 0;
    for (u32 i = 0; i < sizeof k_debug / sizeof *k_debug; i++)
      debug |= strlen(k_debug[i]) == klen && memcmp(key, k_debug[i], klen) == 0;
    if (debug) {
      LOG("cvars: agent.%s is a debug option: not in a release build", key);
      continue;
    }
#endif
    if (klen == 6 && memcmp(key, "gcwait", 6) == 0)
      g_opt_gcwait = v = v > 3600 ? 3600 : v;
    else if (klen == 5 && memcmp(key, "hands", 5) == 0)
      g_opt_hands = v;
    else if (klen == 9 && memcmp(key, "handwatch", 9) == 0)
      g_opt_handwatch = v;
    else if (klen == 7 && memcmp(key, "hmdrate", 7) == 0)
      g_opt_hmdrate = v;
    else if (klen == 3 && memcmp(key, "cap", 3) == 0)
      g_opt_cap = v;
    else if (klen == 10 && memcmp(key, "mirrorboth", 10) == 0)
      g_opt_mirrorboth = v;
    else if (klen == 8 && memcmp(key, "recentre", 8) == 0)
      g_opt_recentre = v;
    else if (klen == 11 && memcmp(key, "fingerprint", 11) == 0)
      g_opt_fingerprint = v;
    else if (klen == 5 && memcmp(key, "trace", 5) == 0)
      g_opt_trace = v;
    else if (klen == 5 && memcmp(key, "waits", 5) == 0)
      g_opt_waits = v;
    else if (klen == 5 && memcmp(key, "rtime", 5) == 0)
      g_opt_rtime = v = v != 0;
    else if (klen == 5 && memcmp(key, "tvhmd", 5) == 0)
      g_opt_tvhmd = v = v != 0;
    else if (klen == 9 && memcmp(key, "multiview", 9) == 0)
      continue; /* the payload's, at the start (multiview.c logs it) */
    else if (klen == 8 && memcmp(key, "insetaam", 8) == 0)
      g_opt_insetaam = v = v > 2 ? 1 : v;
    else if (klen == 10 && memcmp(key, "gazefilter", 10) == 0)
      g_opt_gazefilter = v = v != 0;
    else if (klen == 10 && memcmp(key, "gazemincut", 10) == 0)
      g_opt_gazemincut = v = v > 1000 ? 1000 : v;
    else if (klen == 8 && memcmp(key, "gazebeta", 8) == 0)
      g_opt_gazebeta = v = v > 100 ? 100 : v;
    else if (klen == 8 && memcmp(key, "vignette", 8) == 0)
      g_opt_vignette = v = v > 2 ? 0 : v;
    else if (klen == 8 && memcmp(key, "gazehold", 8) == 0)
      g_opt_gazehold = v = v > 300 ? 300 : v;
    else if (klen == 7 && memcmp(key, "insetpp", 7) == 0)
      g_opt_insetpp = v = v & 7;
    else if (klen == 8 && memcmp(key, "insettaa", 8) == 0)
      g_opt_insettaa = v = v != 0;
    else if (klen == 9 && memcmp(key, "insetsnap", 9) == 0)
      g_opt_insetsnap = v = v > 2 ? 2 : v;
    else if (klen == 3 && memcmp(key, "taa", 3) == 0)
      g_opt_taa = v = v > AGENT_TAA_N ? 0 : v;
#ifndef RELEASE
    else if (klen == 7 && memcmp(key, "taadump", 7) == 0)
      g_opt_taadump = v = v != 0;
#endif
    else if (klen == 10 && memcmp(key, "insetblink", 10) == 0)
      g_opt_insetblink = v = v > 600 ? 600 : v;
    else if (klen == 8 && memcmp(key, "insetwin", 8) == 0)
      g_opt_insetwin = v = v > 3 ? 0 : v;
    else if (klen == 9 && memcmp(key, "insetonly", 9) == 0)
      g_opt_insetonly = v = v > 2 ? 0 : v;
    else if (klen == 9 && memcmp(key, "insetgaze", 9) == 0)
      g_opt_insetgaze = v = v > 11 ? 0 : v;
    else if (klen == 7 && memcmp(key, "gazedot", 7) == 0)
      g_opt_gazedot = v = v != 0;
    else if (klen == 8 && memcmp(key, "insetppt", 8) == 0)
      g_opt_insetppt = v = v > 4000 ? 4000 : v;
    else if (klen == 9 && memcmp(key, "insetsize", 9) == 0)
      g_opt_insetsize = v = v > 4096 ? 0 : v;
    else if (klen == 10 && memcmp(key, "insetwidth", 10) == 0)
      g_opt_insetwidth = v = v > 4096 ? 0 : v;
    else if (klen == 8 && memcmp(key, "insetpos", 8) == 0)
      g_opt_insetpos = v = v != 0;
    else if (klen == 8 && memcmp(key, "insettlv", 8) == 0)
      g_opt_insettlv = v = v != 0;
    else if (klen == 7 && memcmp(key, "insetea", 7) == 0)
      g_opt_insetea = v = v != 0;
    else if (klen == 8 && memcmp(key, "insetocc", 8) == 0)
      g_opt_insetocc = v = v != 0;
    else if (klen == 8 && memcmp(key, "insetlod", 8) == 0)
      g_opt_insetlod = v = v != 0;
    else if (klen == 5 && memcmp(key, "probe", 5) == 0)
      g_opt_probe = v = v > 3 ? 1 : v; /* (2: also the insets' edge bands, 3: the eyes' flicker, mirror.c) */
    else if (klen == 8 && memcmp(key, "inseteye", 8) == 0)
      g_opt_inseteye = v = v != 0;
    else if (klen == 10 && memcmp(key, "insetdebug", 10) == 0)
      g_opt_insetdebug = v = v > 5 ? 0 : v;
    else if (klen == 10 && memcmp(key, "insetstate", 10) == 0)
      g_opt_insetstate = v = v != 0;
    else if (klen == 7 && memcmp(key, "capture", 7) == 0)
      continue; /* followed live (cvars_live_thread): a capture each new value */
    else if (klen == 8 && memcmp(key, "capafter", 8) == 0)
      continue; /* (followed live too) */
    else if (klen == 5 && (memcmp(key, "capdx", 5) == 0 || memcmp(key, "capdy", 5) == 0))
      continue; /* (followed live too) */
    else if (klen == 10 && memcmp(key, "insetviews", 10) == 0)
      g_opt_insetviews = v = v > 2 ? 2 : v;
    else if (klen == 7 && memcmp(key, "tvblank", 7) == 0)
      g_opt_tvblank = v;
    else if (klen == 6 && memcmp(key, "newest", 6) == 0)
      g_opt_newest = v = v != 0;
    else if (klen == 7 && memcmp(key, "predict", 7) == 0)
      g_opt_predict = v = v > 60 ? 60 : v;
    else if (fov_option(key, klen, eq + 1, (u32)(line + n - eq - 1)))
      continue; /* fov.c's, logged there */
    else {
      LOG("cvars: agent option '%s' unknown (known: hands, recentre, mirrorboth, tvblank, newest, "
          "predict, multiview; in a debug build also the debug tools, the inset options and gaze)",
          key);
      continue;
    }
    LOG("cvars: agent.%s = %u", key, v);
  }
  __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
}

/* ---- the game's console variables (cvars.txt: a line "name" logs one, "name=value" sets it) ----
 * Unreal's IConsoleManager singleton is the static astro_hdr.console_mgr (Hikari 1.00's
 * 0x46b4350, made by 0x117cc0 when null; the addresses here and below are Hikari's, the payload's
 * k_builds has each game's); FindConsoleVariable is its vtable slot +0x88 (as the Morpheus plugin
 * calls it at 0x2f8e3a1 for r.ScreenPercentage, then Set at the variable's +0x60 with
 * ECVF_SetByCode).
 * The variable's +0x38 is AsVariableInt, +0x40 AsVariableFloat (CalculateRenderTargetSize
 * 0x2f8fb91): each gives its TConsoleVariableData, [0] the game thread's value, [1] the render
 * thread's. A value is written into both (no Set: its OnChanged would queue render commands
 * from this thread); so a variable only read through its data follows, one with a sink (the
 * sg.* groups) does not: give the r.* ones. Names are UTF-16 (TCHAR) in this build. */
static int cv_atoi(const char *s, int *ok) {
  int neg = 0, v = 0;
  *ok = 0;
  if (*s == '-') neg = 1, s++;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'), *ok = 1;
  if (*s && *s != '\r' && *s != '\n') *ok = 0;
  return neg ? -v : v;
}
static float cv_atof(const char *s, int *ok) {
  int neg = 0;
  double v = 0, f = 0.1;
  *ok = 0;
  if (*s == '-') neg = 1, s++;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'), *ok = 1;
  if (*s == '.') {
    s++;
    while (*s >= '0' && *s <= '9') v += (*s++ - '0') * f, f *= 0.1, *ok = 1;
  }
  if (*s && *s != '\r' && *s != '\n') *ok = 0;
  return (float)(neg ? -v : v);
}
/* The values the agent set (inset mode's defaults, cvars.txt's, the menu's), kept: the game sets
 * some again itself (its quality groups, sg.*, write r.BloomQuality, r.ShadowQuality,
 * r.AmbientOcclusionLevels, r.PostProcessAAQuality through their Set when it applies its
 * settings, after the agent's: the menu showed bloom 1 and the game's shadows and AA, worn
 * 2026-11-07), so cvars_keep puts them back. */
static struct {
  void *var;
  int is_int, from_file; /* from_file: cvars.txt's (dropped when it changes, then set again) */
  union {
    int i;
    float f;
  } v;
  char name[48];
} g_kept[24];
static u32 g_kept_n;
static volatile u32 g_kept_lock; /* the menu (the pad thread) and cvars.txt (the cvars thread) */
static void cvar_keep_note(void *var, const char *name, int is_int, int iv, float fv, int from_file) {
  if (strlen(name) == 18 && memcmp(name, "r.ScreenPercentage", 18) == 0)
    return; /* (the Morpheus plugin sets it itself every frame: not fought over) */
  while (__atomic_exchange_n(&g_kept_lock, 1, __ATOMIC_ACQUIRE))
    ;
  u32 i = 0, free_i = ~0u;
  for (; i < g_kept_n && g_kept[i].var != var; i++)
    if (!g_kept[i].var && free_i == ~0u)
      free_i = i;
  if (i == g_kept_n) { /* a new one: a slot cvars.txt let go of, else the next */
    if (free_i != ~0u)
      i = free_i;
    else if (g_kept_n < sizeof g_kept / sizeof *g_kept)
      __atomic_store_n(&g_kept_n, g_kept_n + 1, __ATOMIC_RELEASE);
    else {
      __atomic_store_n(&g_kept_lock, 0, __ATOMIC_RELEASE);
      return;
    }
    __atomic_store_n(&g_kept[i].var, (void *)0, __ATOMIC_RELEASE);
  }
  g_kept[i].from_file = from_file;
  g_kept[i].is_int = is_int;
  if (is_int)
    g_kept[i].v.i = iv;
  else
    g_kept[i].v.f = fv;
  u32 k = 0;
  for (; name[k] && k < sizeof g_kept[i].name - 1; k++)
    g_kept[i].name[k] = name[k];
  g_kept[i].name[k] = 0;
  __atomic_store_n(&g_kept[i].var, var, __ATOMIC_RELEASE);
  __atomic_store_n(&g_kept_lock, 0, __ATOMIC_RELEASE);
}
/* cvars.txt changed: its values let go of (the file's lines set them again; a line taken out
 * leaves the game its own), the menu's and inset mode's kept */
static void cvar_keep_drop_file(void) {
  while (__atomic_exchange_n(&g_kept_lock, 1, __ATOMIC_ACQUIRE))
    ;
  for (u32 i = 0; i < g_kept_n; i++)
    if (g_kept[i].from_file)
      __atomic_store_n(&g_kept[i].var, (void *)0, __ATOMIC_RELEASE);
  __atomic_store_n(&g_kept_lock, 0, __ATOMIC_RELEASE);
}
/* agent.gcwait=N: the engine's seconds (game time) between its garbage collections,
 * GTimeBetweenPurgingPendingKillObjects (gc.TimeBetweenPurgingPendingKillObjects, a reference
 * variable: cvars.txt's way, the variable's data, doesn't reach it), 60 in all four games, at each
 * build's place (the float its registration, FAutoConsoleVariableRef's, points at). 0 (the
 * default on Hikari 1.00): the game's own (it sets 57-75 from its settings, again at scene changes;
 * going back to it from an agent's value writes the last value the game wrote over the agent's, else 60).
 * Without the line: 600 on the newer engine (Chisato, Allison, Hikari 1.02), the game's own on 1.00.
 * Written over any time of 0-3600 s; kept every 0.5 s (the cvars thread). The once-a-minute hitch: one collection, a frame of ~40 ms. */
volatile u32 g_opt_gcwait = GC_WAIT_DEFAULT; /* (GC_WAIT_DEFAULT: none set, the build's default) */
static const u32 k_gc_wait[BUILD_N] = {0x467e140, 0x4722140, 0x4722140, 0x466e140};
static void gc_wait_keep(void) {
  static float ours, game, logged; /* the agent's value, the game's last own one, the last logged */
  static u32 said, game_said;
  u32 b = astro_hdr.build;
  if (!b || b > BUILD_N || !astro_hdr.game_base)
    return;
  volatile float *p = (volatile float *)((u8 *)astro_hdr.game_base + k_gc_wait[b - 1]);
  float now = *p;
  int game_wrote = ours == 0.0f || now != ours; /* (not the agent's own value) */
  if (ours != 0.0f && now != ours && now > 0.0f && now <= 3600.0f)
    game = now; /* (the game's own for sure: written over the agent's; a value read before the agent
                 * ever wrote could be one an earlier agent in this game left, so not taken) */
  u32 w = g_opt_gcwait == GC_WAIT_DEFAULT ? (build_newer_engine() ? 600 : 0) : g_opt_gcwait;
  if (!w) { /* the game's own: the last it wrote over the agent's given back once (60, the eboot's,
            * if it wrote none yet); the game sets its own again at its next scene change anyway */
    if (ours != 0.0f && now == ours) {
      float g = game != 0.0f ? game : 60.0f;
      *p = g;
      LOG("gc: the game's own collections again (every %.1f s)", (double)g);
    } else if (ours != 0.0f)
      LOG("gc: the game's own collections again (it had set %.1f)", (double)now);
    ours = logged = 0.0f;
    return;
  }
  float want = (float)w;
  if (now == want) {
    ours = want;
    return;
  }
  /* (a time in seconds, as the game sets it from its settings once it has loaded them: 57-75 s seen,
   * a different value each start; anything else is not this variable: left alone) */
  if (!(now > 0.0f && now <= 3600.0f)) {
    if (!said++)
      LOG("gc: the collections' interval reads %f, not a time: left alone", (double)now);
    return;
  }
  if (ours != 0.0f && game_wrote && game_said++ < 4)
    LOG("gc: the game set the collections' interval to %.1f: back to the agent's", (double)now);
  *p = want, ours = want;
  if (want != logged) /* (once per value: the game's resets at each scene change not repeated) */
    LOG("gc: a garbage collection every %u s of game time (agent.gcwait%s; it was %.1f)", (u32)want,
        g_opt_gcwait == GC_WAIT_DEFAULT ? ", the newer engine's default" : "", (double)now);
  logged = want;
}

/* the cvars thread, every 0.5 s: a kept value the game changed, set back */
static void cvars_keep(void) {
  static u32 said;
  for (u32 i = 0; i < __atomic_load_n(&g_kept_n, __ATOMIC_ACQUIRE); i++) {
    void *var = __atomic_load_n(&g_kept[i].var, __ATOMIC_ACQUIRE);
    if (!var)
      continue;
    u8 *vt = *(u8 **)var;
    if (g_kept[i].is_int) {
      int *di = (*(int *(**)(void *))(vt + 0x38))(var);
      if (!di || (di[0] == g_kept[i].v.i && di[1] == g_kept[i].v.i))
        continue;
      if (said++ < 40)
        LOG("cvars: %s set back to %d (the game had made it %d)", g_kept[i].name, g_kept[i].v.i, di[0]);
      di[0] = di[1] = g_kept[i].v.i;
    } else {
      float *df = (*(float *(**)(void *))(vt + 0x40))(var);
      if (!df || (df[0] == g_kept[i].v.f && df[1] == g_kept[i].v.f))
        continue;
      if (said++ < 40)
        LOG("cvars: %s set back to %f (the game had made it %f)", g_kept[i].name,
            (double)g_kept[i].v.f, (double)df[0]);
      df[0] = df[1] = g_kept[i].v.f;
    }
  }
}

/* a value no longer kept (agent.insetpp's bloom and AO: the game's own again; it sets them itself
 * at its next scene change) */
static void cvar_unkeep(const char *name) {
  u32 len = (u32)strlen(name);
  while (__atomic_exchange_n(&g_kept_lock, 1, __ATOMIC_ACQUIRE))
    ;
  for (u32 i = 0; i < g_kept_n; i++)
    if (g_kept[i].var && strlen(g_kept[i].name) == len && memcmp(g_kept[i].name, name, len) == 0) {
      __atomic_store_n(&g_kept[i].var, (void *)0, __ATOMIC_RELEASE);
      LOG("cvars: %s no longer kept (agent.insetpp: the eye's own in the insets)", name);
    }
  __atomic_store_n(&g_kept_lock, 0, __ATOMIC_RELEASE);
}

/* the r.* lines of a text applied to the game's console variables (cvars.txt's, and inset
 * mode's defaults before them: apply_cvars) */
static void apply_cvar_lines(void *mgr, void *(*find)(void *, const u16 *), char *text, int from_file) {
  char *p = text;
  while (*p) {
    char *line = p;
    while (*p && *p != '\n') p++;
    if (*p) *p++ = 0;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#' || *line == '\r') continue;
    if (memcmp(line, "agent.", 6) == 0) continue; /* the agent's own, taken at the fetch */
    char *val = line;
    while (*val && *val != '=' && *val != ' ' && *val != '\r') val++;
    int set = *val == '=';
    *val = 0;
    if (set) val++;
    u16 name[96];
    u32 i = 0;
    for (; line[i] && i < 95; i++) name[i] = (u8)line[i];
    name[i] = 0;
    void *var = find(mgr, name);
    if (!var) {
      LOG("cvars: %s: no such variable", line);
      continue;
    }
    u8 *vt = *(u8 **)var;
    int *di = (*(int *(**)(void *))(vt + 0x38))(var);
    float *df = (*(float *(**)(void *))(vt + 0x40))(var);
    if (di)
      LOG("cvars: %s = %d (render thread %d)%s", line, di[0], di[1], set ? "" : " (read)");
    else if (df)
      LOG("cvars: %s = %f (render thread %f)%s", line, df[0], df[1], set ? "" : " (read)");
    else
      LOG("cvars: %s: neither int nor float (a string or a command): not touched", line);
    if (!set || (!di && !df))
      continue;
    int ok = 0;
    if (di) {
      int v = cv_atoi(val, &ok);
      if (ok)
        di[0] = di[1] = v, cvar_keep_note(var, line, 1, v, 0.0f, from_file);
    } else {
      float v = cv_atof(val, &ok);
      if (ok)
        df[0] = df[1] = v, cvar_keep_note(var, line, 0, 0, v, from_file);
    }
    if (ok)
      LOG("cvars: %s -> %s", line, val);
    else
      LOG("cvars: %s: '%s' is not a number: not set", line, val);
  }
}

static void apply_cvars(void) {
  if (!astro_hdr.viewport || !astro_hdr.console_mgr)
    return;
  cvars_fetch();
  if (!g_cvars_len && !g_multiview_rows)
    return;
  void *mgr = *(void **)((u8 *)astro_hdr.game_base + astro_hdr.console_mgr);
  if (!mgr) {
    LOG("cvars: the console manager is not made yet");
    return;
  }
  void *(*find)(void *, const u16 *) = *(void *(**)(void *, const u16 *))(*(u8 **)mgr + 0x88);
  /* Inset mode (agent.multiview: multiview.c's defaults): bloom and ambient occlusion off (each
   * view makes its own from what it sees, so the inset's glow and shading stopped at its edge:
   * a cutout of the glow round a light, worn 2026-11-07), shadows and AA quality at their
   * highest; cvars.txt's own lines after these, so it can set any of them otherwise. */
  /* (once: on a later change of cvars.txt the preset stays as the menu may have left it) */
  /* With agent.insetpp (3 in inset mode) the insets take the eye's own bloom and AO instead
   * (insetpp.c, docs/MULTIVIEW.md 22): those two are left at the game's values. */
  static int preset_done;
  if (g_multiview_rows && !preset_done) {
    char pre[160];
    u32 ipp = g_opt_insetpp, o = 0;
    pre[0] = 0;
    if (!(ipp & 1))
      o += (u32)agent_snprintf(pre + o, sizeof pre - o, "r.BloomQuality=0\n");
    else
      cvar_unkeep("r.BloomQuality");
    /* AO: 3 levels with the eye's in the insets (the game's 2, worn 2026-11-08: 1 showed its
     * dither in the shadows, 3 the smoothest; the extra levels are the coarse, cheap ones) */
    const struct mv_rate_preset *rp = multiview_rate_preset(); /* (per frame rate) */
    o += (u32)agent_snprintf(pre + o, sizeof pre - o, "r.AmbientOcclusionLevels=%d\n", ipp & 2 ? rp->ao : 0);
    /* (the newer engine, Chisato, Allison and Hikari 1.02: AA quality 3 at most, 4 and above
     * shake the picture: the user, worn, in Chisato and Allison; Chisato's game sets 3 itself) */
    int aa = build_newer_engine() && rp->aa > 3 ? 3 : rp->aa;
    agent_snprintf(pre + o, sizeof pre - o, "r.ShadowQuality=%d\nr.PostProcessAAQuality=%d\n",
                   rp->shadow, aa);
    apply_cvar_lines(mgr, find, pre, 0);
    preset_done = 1;
#ifndef RELEASE
    if (build_newer_engine()) { /* (Chisato's and Allison's newer TAA: its settings as the game has them, for the AA 4+ shimmer) */
      float w = -1.0f, fs = -1.0f;
      int cr = -1, smp = -1;
      hooks_cvar_float("r.TemporalAACurrentFrameWeight", &w, 0);
      hooks_cvar_float("r.TemporalAAFilterSize", &fs, 0);
      hooks_cvar_int("r.TemporalAACatmullRom", &cr, 0);
      hooks_cvar_int("r.TemporalAASamples", &smp, 0);
      LOG("cvars: the game's TAA: r.TemporalAACurrentFrameWeight %.4f, r.TemporalAAFilterSize %.3f, "
          "r.TemporalAACatmullRom %d, r.TemporalAASamples %d", (double)w, (double)fs, cr, smp);
    }
#endif
  }
  cvar_keep_drop_file();
  if (g_cvars_len)
    apply_cvar_lines(mgr, find, g_cvars_text, 1);
}

/* One of the game's integer variables by name: its value into *get, and *set into it when set is
 * not null (both threads' copies, as apply_cvars writes them); 1 if it is an integer variable
 * (menu.c, the debug menu) */
int hooks_cvar_int(const char *name, int *get, const int *set) {
  if (!astro_hdr.viewport || !astro_hdr.console_mgr)
    return 0;
  void *mgr = *(void **)((u8 *)astro_hdr.game_base + astro_hdr.console_mgr);
  if (!mgr)
    return 0;
  void *(*find)(void *, const u16 *) = *(void *(**)(void *, const u16 *))(*(u8 **)mgr + 0x88);
  u16 wname[96];
  u32 i = 0;
  for (; name[i] && i < 95; i++)
    wname[i] = (u8)name[i];
  wname[i] = 0;
  void *var = find(mgr, wname);
  if (!var)
    return 0;
  u8 *vt = *(u8 **)var;
  int *di = (*(int *(**)(void *))(vt + 0x38))(var);
  if (!di)
    return 0;
  if (set)
    di[0] = di[1] = *set, cvar_keep_note(var, name, 1, *set, 0.0f, 0);
  if (get)
    *get = di[0];
  return 1;
}

/* One of the game's float variables by name, as hooks_cvar_int (menu.c) */
int hooks_cvar_float(const char *name, float *get, const float *set) {
  if (!astro_hdr.viewport || !astro_hdr.console_mgr)
    return 0;
  void *mgr = *(void **)((u8 *)astro_hdr.game_base + astro_hdr.console_mgr);
  if (!mgr)
    return 0;
  void *(*find)(void *, const u16 *) = *(void *(**)(void *, const u16 *))(*(u8 **)mgr + 0x88);
  u16 wname[96];
  u32 i = 0;
  for (; name[i] && i < 95; i++)
    wname[i] = (u8)name[i];
  wname[i] = 0;
  void *var = find(mgr, wname);
  if (!var)
    return 0;
  u8 *vt = *(u8 **)var;
  float *df = (*(float *(**)(void *))(vt + 0x40))(var);
  if (!df)
    return 0;
  if (set)
    df[0] = df[1] = *set, cvar_keep_note(var, name, 0, 0, *set, 0);
  if (get)
    *get = df[0];
  return 1;
}

/* ---- the flight recorder ----
 * A snapshot every second of what decides how the headset feels, kept for the last 16 s. The
 * player presses L3+R3 (both sticks in) the moment something feels wrong: the controller buzzes,
 * and the log gets "mark #n" with the 10 s before and the 5 s after, one line a second. Debug
 * builds only (a player would get a buzz they can't explain). */
#ifndef RELEASE
struct rec_snap {
  u32 t_s, handed, shown, again, never, gpu_max_us, ts_min, ts_max, pose_max_udeg, reseats, clockfb,
      dmem_refused, worn, hands;
};
static struct rec_snap g_rec_ring[16];
static u32 g_rec_n, g_rec_follow, g_rec_marks;
static volatile u32 g_mark_req;
static volatile s32 g_mark_pad = -1;
static volatile u64 g_mark_buzz_until;
static void rec_log(const char *what, const struct rec_snap *r) {
  LOG("%s %us: %u frames handed, %u shown, %u twice, %u never; GPU %u.%u ms at most; time step "
      "%u-%u us; head step %u.%03u deg at most; %u re-seats, %u clock fallbacks, %u memory refused; "
      "worn %u", what, r->t_s, r->handed, r->shown, r->again,
      r->never, r->gpu_max_us / 1000, r->gpu_max_us / 100 % 10,
      r->ts_min == ~0u ? 0 : r->ts_min, r->ts_max, r->pose_max_udeg / 1000000,
      r->pose_max_udeg / 1000 % 1000, r->reseats, r->clockfb, r->dmem_refused, r->worn);
}
static void rec_tick(void) {
  static u64 next, start;
  static u32 last_handed, last_reseats;
  u64 t = now_us();
  if (g_mark_buzz_until && t >= g_mark_buzz_until) {
    u8 off[2] = {0, 0};
    if (scePadSetVibration && g_mark_pad >= 0)
      scePadSetVibration(g_mark_pad, off);
    g_mark_buzz_until = 0;
  }
  if (g_mark_req) { /* the 10 s before, now; the 5 after as they come */
    g_mark_req = 0;
    g_rec_marks++;
    LOG("mark #%u at %us (L3+R3): the 10 s before, then the 5 after", g_rec_marks,
        start ? (u32)((t - start) / 1000000) : 0);
    u32 have = g_rec_n < 10 ? g_rec_n : 10;
    for (u32 i = have; i > 0; i--)
      rec_log("  before", &g_rec_ring[(g_rec_n - i) % 16]);
    g_rec_follow = 5;
  }
  if (!start)
    start = t, next = t + 1000000;
  if (t < next)
    return;
  next += 1000000;
  struct rec_snap *r = &g_rec_ring[g_rec_n % 16];
  u32 h = g_vr.frames_in, rs = g_rec_reseats;
  r->t_s = (u32)((t - start) / 1000000);
  r->handed = h - last_handed, last_handed = h;
  r->shown = __atomic_exchange_n(&g_rec_shown, 0, __ATOMIC_RELAXED);
  r->again = __atomic_exchange_n(&g_rec_again, 0, __ATOMIC_RELAXED);
  r->never = __atomic_exchange_n(&g_rec_never, 0, __ATOMIC_RELAXED);
  r->gpu_max_us = __atomic_exchange_n(&g_rec_gpu_max, 0, __ATOMIC_RELAXED);
  r->ts_min = __atomic_exchange_n(&g_rec_ts_min, ~0u, __ATOMIC_RELAXED);
  r->ts_max = __atomic_exchange_n(&g_rec_ts_max, 0, __ATOMIC_RELAXED);
  r->pose_max_udeg = __atomic_exchange_n(&g_rec_pose_max, 0, __ATOMIC_RELAXED);
  r->reseats = rs - last_reseats, last_reseats = rs;
  r->clockfb = __atomic_exchange_n(&g_rec_clockfb, 0, __ATOMIC_RELAXED);
  r->dmem_refused = __atomic_exchange_n(&g_rec_dmem_refused, 0, __ATOMIC_RELAXED);
  r->worn = !g_vr.hmd_off;
  g_rec_n++;
  if (g_rec_follow) {
    g_rec_follow--;
    rec_log("  after", r);
  }
}
/* the controller hook: L3+R3 pressed together (rising) asks for a mark; a short buzz says so */
static void rec_pad(s32 handle, const u8 *data) {
  static u32 was;
  u32 b = *(const u32 *)data, both = (b & 0x6) == 0x6; /* SCE_PAD_BUTTON_L3 0x2, R3 0x4 */
  if (both && !was) {
    g_mark_req = 1;
    g_mark_pad = handle;
    u8 on[2] = {0, 200}; /* large motor, small motor */
    if (scePadSetVibration)
      scePadSetVibration(handle, on);
    g_mark_buzz_until = now_us() + 200000;
  }
  was = both;
}
#endif

/* cvars.txt read again every 2 s while the game runs: a change is applied at once, so settings
 * can be compared worn without a restart. Only what is safe to change mid-game: the game's own
 * variables (as at the start) and agent.newest and agent.predict (read every frame / every pose);
 * the other agent options set things up once at the start and are left as they were. */
static char g_cvars_seen[sizeof g_cvars_text];
/* The 10 s report (health_report), made on this thread: the hand-over (the game's render thread)
 * only says it is due. Made in the hand-over it held the game's next frame up once a report:
 * one frame on the headset for one refresh in nearly every 10 s window, at every setting and in
 * stock (2026-11-07, logs/mv80-mv85). */
static volatile u32 g_health_due; /* the frames handed over in the window; 0: none due */
static void health_report(u32 handed);
static void health_report_due(void) {
  u32 d = __atomic_exchange_n(&g_health_due, 0, __ATOMIC_ACQ_REL);
  if (!d)
    return;
  u64 t0 = sceKernelGetProcessTime();
  LOG("hmd: the game drew %u frames in the last 10 s (%u.%u a second)", d, d / 10, d % 10);
  health_report(d);
  if (g_multiview_rows)
    insetpp_report();
  static u32 said;
  if (said++ < 3)
    LOG("hmd: the 10 s report took %lu us (on the agent's cvars thread)",
        (u64)(sceKernelGetProcessTime() - t0));
}
/* The frame rate for the next start (the debug menu's "Frame rate, next start", 60/90/120;
 * docs/MULTIVIEW.md 28): it can't change while the game runs (the eyes, the insets' rows and the
 * plugin's frame sequence are fixed at its start), so a choice is saved to framerate.txt by the
 * payload (CMD_FRAMERATE) for the next one. Here, on the cvars thread, every 2 s: the game's
 * threads never wait on the payload. Starts at this start's rate; a payload without the command
 * (before it) never answers: said once, not tried again until the choice changes. */
volatile u32 g_opt_nextfps;
static u32 g_fps_saved;
/* what framerate.txt asks this start (the payload's want_fps; read as the cvars thread starts, the
 * native mode not yet confirmed: frame_pace still says 60 then) */
static u32 framerate_now(void) {
  return astro_hdr.want_fps == 90 || astro_hdr.want_fps == 120 ? astro_hdr.want_fps : 60;
}
static void framerate_save(void) {
  u32 now = framerate_now(), saved = g_fps_saved;
  u32 want = g_opt_nextfps;
  if (want == saved || (want != 60 && want != 90 && want != 120))
    return;
  g_fps_saved = want;
  int rc = agent_call(CMD_FRAMERATE, want, 0, 0, 0, 2000, 0, 0);
  LOG("framerate: %u fps from the next start (this one %u)%s", want, now,
      rc == 0 ? ": framerate.txt saved" : ": the payload did not save it (an older payload?)");
}

#ifndef RELEASE
/* a line's number from p to end (its digits; 0 without one) */
static u32 line_u32(const char *p, const char *end) {
  u32 v = 0;
  for (; p < end && *p >= '0' && *p <= '9' && v < 100000000u; p++)
    v = v * 10 + (u32)(*p - '0');
  return v;
}
#endif
static void *cvars_live_thread(void *arg) {
  (void)arg;
  static char text[sizeof g_cvars_text];
  /* the file as it is now: what changes after it is applied (the text read at the start is cut up
   * by apply_cvars, no use to compare with) */
  g_fps_saved = g_opt_nextfps = framerate_now(); /* (the menu's line starts at this start's rate) */
  if (agent_call(CMD_CVARS, (u64)g_cvars_seen, sizeof g_cvars_seen, 0, 0, 3000, 0, 0) != 0)
    memset(g_cvars_seen, 0, sizeof g_cvars_seen);
  g_cvars_seen[sizeof g_cvars_seen - 1] = 0;
  while (g_shared.cmd != CMD_QUIT) {
    for (int i = 0; i < 4 && g_shared.cmd != CMD_QUIT; i++) { /* (the report due within 0.5 s) */
      sceKernelUsleep(500000);
      health_report_due();
      cvars_keep();
      gc_wait_keep();
    }
    framerate_save();
    memset(text, 0, sizeof text);
    if (agent_call(CMD_CVARS, (u64)text, sizeof text, 0, 0, 3000, 0, 0) != 0)
      continue;
    text[sizeof text - 1] = 0;
    if (!memcmp(text, g_cvars_seen, sizeof text))
      continue;
    memcpy(g_cvars_seen, text, sizeof text);
    LOG("cvars: cvars.txt changed: applied now (the game's variables, agent.newest, agent.predict"
#ifndef RELEASE
        ", agent.tvhmd, agent.probe, agent.capture and the inset and gaze window options"
#endif
        ")");
    u32 newest = 1, predict = 0;
    for (const char *p = text; *p;) {
      const char *line = p;
      while (*p && *p != '\n') p++;
      u32 n = (u32)(p - line);
      if (*p) p++;
      u32 v = 0;
      if (n >= 14 && memcmp(line, "agent.newest=", 13) == 0) {
        newest = line[13] != '0';
      } else if (n >= 15 && memcmp(line, "agent.predict=", 14) == 0) {
        for (const char *d = line + 14; d < line + n && *d >= '0' && *d <= '9'; d++)
          v = v * 10 + (u32)(*d - '0');
        predict = v > 60 ? 60 : v;
      }
    }
    if (newest != g_opt_newest || predict != g_opt_predict)
      LOG("cvars: agent.newest %u -> %u, agent.predict %u -> %u ms", g_opt_newest, newest,
          g_opt_predict, predict);
    g_opt_newest = newest, g_opt_predict = predict;
#ifndef RELEASE
    { /* agent.tvhmd, agent.insetviews and the rest (debug tools) follow the file too: their
       * defaults when their line is gone */
      /* (inset mode, agent.multiview: its defaults where a line is missing, as multiview_boot
       * set them) */
      u32 mvon = g_multiview_rows != 0;
      u32 tvhmd = 0, views = mvon ? 2 : 0, state = mvon, idebug = 0, ieye = 1, probe = 0, itlv = 1,
          iea = 1, iocc = 1, ilod = 1, ipos = 1, ippt = 1200, isize = 0, idot = 0, igaze = mvon, ionly = 0,
          iwin = mvon ? 3 : 0, iblink = 0, ipp = mvon ? 7 : 0, ihold = 50,
          iwidth = mvon ? 2 * g_multiview_rows : 0, ivig = 0, iaam = mvon ? 2 : 1, gfilt = 1, gmin = 10,
          gbeta = 1, itaa = 0, ours = mvon ? taa_default() : 0, isnap = 2, mb = 0, capafter = 0;
      s32 capdx = 0, capdy = 0;
      u32 gcw = 0, gcw_seen = 0;
      for (const char *p = text; *p;) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        u32 n = (u32)(p - line);
        if (*p) p++;
        if (n >= 13 && memcmp(line, "agent.tvhmd=", 12) == 0)
          tvhmd = line[12] != '0';
        else if (n >= 18 && memcmp(line, "agent.insetviews=", 17) == 0)
          views = line[17] >= '0' && line[17] <= '2' ? (u32)(line[17] - '0') : 0;
        else if (n >= 18 && memcmp(line, "agent.insetstate=", 17) == 0)
          state = line[17] != '0';
        else if (n >= 16 && memcmp(line, "agent.inseteye=", 15) == 0)
          ieye = line[15] != '0';
        else if (n >= 18 && memcmp(line, "agent.insetdebug=", 17) == 0)
          idebug = line[17] >= '0' && line[17] <= '5' ? (u32)(line[17] - '0') : 0;
        else if (n >= 16 && memcmp(line, "agent.insetaam=", 15) == 0)
          iaam = line[15] == '0' ? 0 : line[15] == '2' ? 2 : 1;
        else if (n >= 16 && memcmp(line, "agent.insettaa=", 15) == 0)
          itaa = line[15] != '0';
        else if (n >= 17 && memcmp(line, "agent.insetsnap=", 16) == 0)
          isnap = line[16] >= '0' && line[16] <= '2' ? (u32)(line[16] - '0') : 2;
        else if (n >= 11 && memcmp(line, "agent.taa=", 10) == 0)
          ours = line_u32(line + 10, line + n), ours = ours > AGENT_TAA_N ? 0 : ours; /* (as cvars_fetch) */
        else if (n >= 18 && memcmp(line, "agent.mirrorboth=", 17) == 0)
          mb = line[17] >= '0' && line[17] <= '2' ? (u32)(line[17] - '0') : 0;
        else if (n >= 18 && memcmp(line, "agent.gazefilter=", 17) == 0)
          gfilt = line[17] != '0';
        else if (n >= 18 && memcmp(line, "agent.gazemincut=", 17) == 0) {
          gmin = 0;
          for (const char *d = line + 17; d < line + n && *d >= '0' && *d <= '9'; d++)
            gmin = gmin * 10 + (u32)(*d - '0');
          gmin = gmin > 1000 ? 1000 : gmin;
        } else if (n >= 16 && memcmp(line, "agent.gazebeta=", 15) == 0) {
          gbeta = 0;
          for (const char *d = line + 15; d < line + n && *d >= '0' && *d <= '9'; d++)
            gbeta = gbeta * 10 + (u32)(*d - '0');
          gbeta = gbeta > 100 ? 100 : gbeta;
        } else if (n >= 16 && memcmp(line, "agent.vignette=", 15) == 0)
          ivig = line[15] == '1' ? 1 : line[15] == '2' ? 2 : 0;
        else if (n >= 16 && memcmp(line, "agent.gazehold=", 15) == 0) {
          ihold = 0;
          for (const char *d = line + 15; d < line + n && *d >= '0' && *d <= '9'; d++)
            ihold = ihold * 10 + (u32)(*d - '0');
          ihold = ihold > 300 ? 300 : ihold;
        } else if (n >= 15 && memcmp(line, "agent.insetpp=", 14) == 0) {
          ipp = line[14] >= '0' && line[14] <= '7' ? (u32)(line[14] - '0') : 7;
        } else if (n >= 18 && memcmp(line, "agent.insetblink=", 17) == 0) {
          iblink = 0;
          for (const char *d = line + 17; d < line + n && *d >= '0' && *d <= '9'; d++)
            iblink = iblink * 10 + (u32)(*d - '0');
          iblink = iblink > 600 ? 600 : iblink;
        } else if (n >= 16 && memcmp(line, "agent.insetwin=", 15) == 0) {
          iwin = line[15] >= '0' && line[15] <= '3' ? (u32)(line[15] - '0') : 0;
        }
        else if (n >= 17 && memcmp(line, "agent.insetonly=", 16) == 0)
          ionly = line[16] >= '0' && line[16] <= '2' ? (u32)(line[16] - '0') : 0;
        else if (n >= 17 && memcmp(line, "agent.insetgaze=", 16) == 0)
          igaze = line_u32(line + 16, line + n), igaze = igaze > 11 ? 0 : igaze; /* (as cvars_fetch) */
        else if (n >= 15 && memcmp(line, "agent.gazedot=", 14) == 0)
          idot = line[14] != '0';
        else if (n >= 16 && memcmp(line, "agent.insetppt=", 15) == 0) {
          ippt = 0;
          for (const char *d = line + 15; d < line + n && *d >= '0' && *d <= '9'; d++)
            ippt = ippt * 10 + (u32)(*d - '0');
          ippt = ippt > 4000 ? 4000 : ippt;
        } else if (n >= 18 && memcmp(line, "agent.insetwidth=", 17) == 0) {
          iwidth = 0;
          for (const char *d = line + 17; d < line + n && *d >= '0' && *d <= '9'; d++)
            iwidth = iwidth * 10 + (u32)(*d - '0');
          iwidth = iwidth > 4096 ? 0 : iwidth;
        } else if (n >= 17 && memcmp(line, "agent.insetsize=", 16) == 0) {
          isize = 0;
          for (const char *d = line + 16; d < line + n && *d >= '0' && *d <= '9'; d++)
            isize = isize * 10 + (u32)(*d - '0');
          isize = isize > 4096 ? 0 : isize;
        } else if (n >= 16 && memcmp(line, "agent.insetpos=", 15) == 0)
          ipos = line[15] != '0';
        else if (n >= 16 && memcmp(line, "agent.insettlv=", 15) == 0)
          itlv = line[15] != '0';
        else if (n >= 15 && memcmp(line, "agent.insetea=", 14) == 0)
          iea = line[14] != '0';
        else if (n >= 16 && memcmp(line, "agent.insetocc=", 15) == 0)
          iocc = line[15] != '0';
        else if (n >= 16 && memcmp(line, "agent.insetlod=", 15) == 0)
          ilod = line[15] != '0';
        else if (n >= 13 && memcmp(line, "agent.gcwait=", 13) == 0) /* (empty: 0, as cvars_fetch) */
          gcw = line_u32(line + 13, line + n), gcw = gcw > 3600 ? 3600 : gcw, gcw_seen = 1;
        else if (n >= 13 && memcmp(line, "agent.probe=", 12) == 0)
          probe = line[12] == '2' ? 2 : line[12] == '3' ? 3 : line[12] != '0';
        else if (n >= 15 && memcmp(line, "agent.capture=", 14) == 0) {
          u32 c = 0;
          for (const char *d = line + 14; d < line + n && *d >= '0' && *d <= '9'; d++)
            c = c * 10 + (u32)(*d - '0');
          if (c != g_opt_capture)
            LOG("cvars: agent.capture %u -> %u", g_opt_capture, c);
          g_opt_capture = c;
        } else if (n >= 13 && (memcmp(line, "agent.capdx=", 12) == 0 || memcmp(line, "agent.capdy=", 12) == 0)) {
          s32 c = line[12] == '-' ? -(s32)line_u32(line + 13, line + n) : (s32)line_u32(line + 12, line + n);
          c = c < -1000 ? -1000 : c > 1000 ? 1000 : c;
          if (line[10] == 'x')
            capdx = c;
          else
            capdy = c;
        } else if (n >= 16 && memcmp(line, "agent.capafter=", 15) == 0)
          capafter = line_u32(line + 15, line + n), capafter = capafter > 2000 ? 2000 : capafter; /* (1000: a burst after a look back, 2000: in a row) */
      }
      if (views != g_opt_insetviews)
        LOG("cvars: agent.insetviews %u -> %u", g_opt_insetviews, views);
      g_opt_insetviews = views;
      if (state != g_opt_insetstate)
        LOG("cvars: agent.insetstate %u -> %u", g_opt_insetstate, state);
      g_opt_insetstate = state;
      if (idebug != g_opt_insetdebug)
        LOG("cvars: agent.insetdebug %u -> %u", g_opt_insetdebug, idebug);
      g_opt_insetdebug = idebug;
      if (ieye != g_opt_inseteye)
        LOG("cvars: agent.inseteye %u -> %u", g_opt_inseteye, ieye);
      g_opt_inseteye = ieye;
      if (!gcw_seen)
        gcw = GC_WAIT_DEFAULT;
      if (gcw != g_opt_gcwait) {
        LOG("cvars: agent.gcwait %d -> %d (-1: the build's default)", (int)g_opt_gcwait, (int)gcw);
        g_opt_gcwait = gcw;
      }
      if (probe != g_opt_probe)
        LOG("cvars: agent.probe %u -> %u", g_opt_probe, probe);
      g_opt_probe = probe;
      if (itlv != g_opt_insettlv)
        LOG("cvars: agent.insettlv %u -> %u", g_opt_insettlv, itlv);
      g_opt_insettlv = itlv;
      if (iea != g_opt_insetea)
        LOG("cvars: agent.insetea %u -> %u", g_opt_insetea, iea);
      g_opt_insetea = iea;
      if (iocc != g_opt_insetocc)
        LOG("cvars: agent.insetocc %u -> %u", g_opt_insetocc, iocc);
      g_opt_insetocc = iocc;
      if (ilod != g_opt_insetlod)
        LOG("cvars: agent.insetlod %u -> %u", g_opt_insetlod, ilod);
      g_opt_insetlod = ilod;
      if (ipos != g_opt_insetpos)
        LOG("cvars: agent.insetpos %u -> %u", g_opt_insetpos, ipos);
      g_opt_insetpos = ipos;
      if (ippt != g_opt_insetppt || isize != g_opt_insetsize)
        LOG("cvars: agent.insetppt %u -> %u, agent.insetsize %u -> %u", g_opt_insetppt, ippt,
            g_opt_insetsize, isize);
      g_opt_insetppt = ippt, g_opt_insetsize = isize;
      if (iwidth != g_opt_insetwidth)
        LOG("cvars: agent.insetwidth %u -> %u", g_opt_insetwidth, iwidth);
      g_opt_insetwidth = iwidth;
      if (idot != g_opt_gazedot)
        LOG("cvars: agent.gazedot %u -> %u", g_opt_gazedot, idot);
      g_opt_gazedot = idot;
      if (igaze != g_opt_insetgaze)
        LOG("cvars: agent.insetgaze %u -> %u", g_opt_insetgaze, igaze);
      g_opt_insetgaze = igaze;
      if (ionly != g_opt_insetonly)
        LOG("cvars: agent.insetonly %u -> %u", g_opt_insetonly, ionly);
      g_opt_insetonly = ionly;
      if (iwin != g_opt_insetwin)
        LOG("cvars: agent.insetwin %u -> %u (registration stays as at the launch)", g_opt_insetwin, iwin);
      g_opt_insetwin = iwin;
      if (iblink != g_opt_insetblink)
        LOG("cvars: agent.insetblink %u -> %u", g_opt_insetblink, iblink);
      g_opt_insetblink = iblink;
      if (ipp != g_opt_insetpp)
        LOG("cvars: agent.insetpp %u -> %u", g_opt_insetpp, ipp);
      g_opt_insetpp = ipp;
      if (ihold != g_opt_gazehold)
        LOG("cvars: agent.gazehold %u -> %u", g_opt_gazehold, ihold);
      g_opt_gazehold = ihold;
      if (ivig != g_opt_vignette)
        LOG("cvars: agent.vignette %u -> %u", g_opt_vignette, ivig);
      g_opt_vignette = ivig;
      if (iaam != g_opt_insetaam || gfilt != g_opt_gazefilter || gmin != g_opt_gazemincut ||
          gbeta != g_opt_gazebeta)
        LOG("cvars: agent.insetaam %u -> %u, agent.gazefilter %u -> %u, gazemincut %u -> %u, "
            "gazebeta %u -> %u", g_opt_insetaam, iaam, g_opt_gazefilter, gfilt, g_opt_gazemincut,
            gmin, g_opt_gazebeta, gbeta);
      g_opt_insetaam = iaam, g_opt_gazefilter = gfilt, g_opt_gazemincut = gmin, g_opt_gazebeta = gbeta;
      if (itaa != g_opt_insettaa)
        LOG("cvars: agent.insettaa %u -> %u", g_opt_insettaa, itaa);
      g_opt_insettaa = itaa;
      if (ours != g_opt_taa)
        LOG("cvars: agent.taa %u -> %u", g_opt_taa, ours);
      g_opt_taa = ours;
      if (isnap != g_opt_insetsnap)
        LOG("cvars: agent.insetsnap %u -> %u", g_opt_insetsnap, isnap);
      g_opt_insetsnap = isnap;
      if (capdx != g_opt_capdx || capdy != g_opt_capdy)
        LOG("cvars: agent.capdx/capdy %d %d -> %d %d", g_opt_capdx, g_opt_capdy, capdx, capdy);
      g_opt_capdx = capdx, g_opt_capdy = capdy;
      if (capafter != g_opt_capafter)
        LOG("cvars: agent.capafter %u -> %u", g_opt_capafter, capafter);
      g_opt_capafter = capafter;
      if (mb != g_opt_mirrorboth) /* (live: the probe needs 1, the 1:1 captures 2; 0 without the line) */
        LOG("cvars: agent.mirrorboth %u -> %u", g_opt_mirrorboth, mb);
      g_opt_mirrorboth = mb;
      if (tvhmd != g_opt_tvhmd)
        LOG("cvars: agent.tvhmd %u -> %u", g_opt_tvhmd, tvhmd);
      g_opt_tvhmd = tvhmd;
    }
#endif
    memcpy(g_cvars_text, text, sizeof text);
    g_cvars_len = (u32)strlen(g_cvars_text);
    apply_cvars(); /* (its fetch is done once: it applies this text) */
  }
  return 0;
}

static void *gpu_label_thread(void *arg) {
  (void)arg;
  u8 *vp = (u8 *)astro_hdr.game_base + astro_hdr.viewport;
  u64 last[2] = {0, 0};
  LOG("gpu: timing the game's frames from its labels (viewport at %p)", vp);
  { /* the engine's frame-rate settings (GEngine, astro_hdr.engine: Hikari's 0x4b186f0;
     * UpdateTimeAndHandleMaxTickRate reads flags +0x620 (0x08 bForceDisableFrameRateSmoothing,
     * 0x10 bSmoothFrameRate, 0x20 bUseFixedFrameRate), FixedFrameRate +0x624,
     * SmoothedFrameRateRange +0x628..+0x634, the running average delta +0xa50) */
    u8 *eng = astro_hdr.engine ? *(u8 **)((u8 *)astro_hdr.game_base + astro_hdr.engine) : 0;
    if (eng)
      LOG("gpu: engine %p: flags 0x%x, fixed frame rate %f, smoothing range %u:%f %u:%f, average "
          "delta %f ms", eng, *(u32 *)(eng + 0x620), *(float *)(eng + 0x624), eng[0x628],
          *(float *)(eng + 0x62c), eng[0x630], *(float *)(eng + 0x634),
          *(float *)(eng + 0xa50) * 1000.0f);
    /* t.MaxFPS (registered at 0x1986728, its value through the pointer astro_hdr.max_fps:
     * Hikari's 0x4b18cd0): read last by GetMaxTickRate, a cap when above 0 */
    float *mf = astro_hdr.max_fps ? *(float **)((u8 *)astro_hdr.game_base + astro_hdr.max_fps) : 0;
    if (mf)
      LOG("gpu: t.MaxFPS %f", *mf);
    if (astro_hdr.neo) /* Hikari's 0x4b8ef08, its question at +8 */
      LOG("gpu: the game's own Neo-mode answer %u (asked %u)",
          *(u32 *)((u8 *)astro_hdr.game_base + astro_hdr.neo),
          *(u8 *)((u8 *)astro_hdr.game_base + astro_hdr.neo + 8));
    /* the engine object's own class: its vtable, and the functions in the slots the main loop
     * calls (+0x228 GetMaxTickRate, +0x240, +0x248 IsAllowedFramerateSmoothing); a subclass's
     * override would explain measurements 7-8 (UGameEngine's own at 0x1975740 patched, no change) */
    if (eng) {
      u64 *vt = *(u64 **)eng;
      LOG("gpu: engine vtable %p (eboot +0x%lx): +0x228 -> +0x%lx, +0x240 -> +0x%lx, +0x248 -> +0x%lx",
          vt, (u64)vt - astro_hdr.game_base, vt[0x228 / 8] - astro_hdr.game_base,
          vt[0x240 / 8] - astro_hdr.game_base, vt[0x248 / 8] - astro_hdr.game_base);
      /* The cap at 60 (measurements 3-10): UGameEngine::GetMaxTickRate (0x15aea50, the live
       * slot +0x228) returns, with r.VSync on, SmoothedFrameRateRange's upper bound (the engine
       * object's +0x634, 60.0, bound type +0x630: 2 would be open) whenever UEngine's own
       * (0x1975740) gives 0, which it does with smoothing force-disabled (flags 0x1b); the main
       * loop then sleeps the rest of 1/60 every frame (0x196f2bb). In the native 120 mode the
       * bound is made 125: still a cap, above the refresh. */
      cvars_fetch();
      /* (opening the bound, type 2 at +0x630, hung the console on 2026-11-01: never) */
      if (g_opt_cap > 1) {
        /* agent.cap=N: N frames a second, or N thousandths of one from 1000 on (119880); at 60
         * too (59940: the headset's half rate, against the game's own 60.0) */
        float capv = g_opt_cap >= 1000 ? (float)g_opt_cap / 1000.0f : (float)g_opt_cap;
        *(volatile float *)(eng + 0x634) = capv;
        LOG("gpu: the engine's frame-rate cap -> %u.%03u (agent.cap)", (u32)capv,
            (u32)((capv - (float)(u32)capv) * 1000.0f + 0.5f));
      } else if (astro_hdr.frame_pace == 1 && eng[0x630] != 2 && *(float *)(eng + 0x634) < 125.0f) {
        /* the refresh itself (2026-11-01): at 125 the limiter let ticks start after 8.0 ms and the
         * game's time step jittered (sd 0.14-0.36 ms at 120, 0.45-0.81 at 90); at the refresh it is
         * as regular as at 60 (sd 0.08 ms). The headset's refresh, not the round number: PS VR2
         * runs 119.88 and 89.91 Hz (1000/1001 of them, the vblank clock's 1001000 / hz); at 120.0
         * the game made a frame more than the headset showed every ~1100 frames, two handed over
         * in one refresh and one never shown (2026-11-08, logs/inset-90-120 n90-b: each 11-12 s
         * at 90, ~9.4 s at 120, the hand-off thread on time, docs/MULTIVIEW.md 28) */
        float capv = (astro_hdr.refresh_hz == 90 ? 90000.0f : 120000.0f) / 1001.0f;
        *(volatile float *)(eng + 0x634) = capv;
        LOG("gpu: the engine's frame-rate cap (SmoothedFrameRateRange's upper bound, VSync) 60 -> "
            "%u.%03u, the headset's refresh", (u32)capv, (u32)((capv - (float)(u32)capv) * 1000.0f + 0.5f));
      } else if (astro_hdr.frame_pace == 2 && eng[0x630] != 2 && *(float *)(eng + 0x634) > 59.99f &&
                 *(float *)(eng + 0x634) < 60.01f) {
        /* At 60 (a frame every two refreshes), the headset's half rate: PS VR2 refreshes at 119.88
         * Hz (1000/1001 of 120), so 59.94, not the game's 60.0. At 60.0 the game made a frame a
         * little faster than the headset took them: one or two frames each 10 s were shown for
         * one refresh instead of two (a short frame, a judder); at 59.94 none in 15 of 16
         * windows, the game's time step 16.684 ms (sd 68-123 us against 124-131), the GPU as
         * before (logs/cap1-5994 against cap2-60, the cafe's held view, 2026-11-08; found at 90
         * and 120 by the inset-90-120 session, docs/MULTIVIEW.md 28) */
        float capv = 60000.0f / 1001.0f;
        *(volatile float *)(eng + 0x634) = capv;
        LOG("gpu: the engine's frame-rate cap 60 -> 59.940, the headset's half rate");
      }
    }
    /* The eye size (resolution.txt): the plugin's CalculateRenderTargetSize (0x2f8fb20) makes
     * the eye texture ceil(1920 x SP/100) by ceil(1080 x SP/100) from the live r.ScreenPercentage
     * (its TConsoleVariableData cached at astro_hdr.sp_cache, Hikari's 0x4bc5978, on its first
     * call: [0] the game thread's value, [1] the render thread's), the panel the HMD reports
     * playing no part; UpdatePostProcessSettings (0x2f8ff60) gives the views the same
     * percentage, and NeedReAllocateViewportRenderTarget (0x2f8fc40) compares the texture to that
     * size every frame, so a new value makes new textures (vr2.c takes a pair of a new size as a
     * new pair). */
    u32 sp_bits = astro_hdr.screen_pct, stock_bits;
    if (multiview_stock_pct(&stock_bits)) { /* (inset mode refused: the stock game's own size) */
      LOG("gpu: inset mode didn't run: the eyes at resolution.txt's size, not inset mode's");
      sp_bits = stock_bits;
    }
    if (sp_bits && astro_hdr.viewport && astro_hdr.sp_cache) {
      float **cv = (float **)((u8 *)astro_hdr.game_base + astro_hdr.sp_cache);
      float v;
      memcpy(&v, &sp_bits, sizeof v);
      /* The side-by-side texture is 1920 x SP/100 wide. PS VR2's compositor takes it up to 4096
       * (2048 eyes; 2200 had every frame refused, 0x8a720016), and the game's memory runs out at
       * 2400 (a 106 MB render-target allocation refused, the game quits): 213.33 at the most.
       * Textures over 3840 are registered at one eye's width (vr2.c pairs_register). */
      if (v > 213.34f) {
        LOG("gpu: r.ScreenPercentage %f over 213.33 (eyes 2048, PS VR2's 4096-wide limit): 213.33", v);
        v = 213.3333f;
      }
      if (*cv) {
        LOG("gpu: r.ScreenPercentage %f (render thread %f) -> %f: eyes %ux%u", (*cv)[0], (*cv)[1],
            v, (u32)(960.0f * v / 100.0f + 0.999f), (u32)(1080.0f * v / 100.0f + 0.999f));
        (*cv)[0] = v;
        (*cv)[1] = v;
      } else {
        LOG("gpu: r.ScreenPercentage not cached by the plugin yet: the game's own eye size");
      }
    }
    apply_cvars();
    u8 attr[64], thr[16];
    scePthreadAttrInit(attr);
    scePthreadAttrSetstacksize(attr, 64 * 1024);
    if (scePthreadCreate(thr, attr, cvars_live_thread, 0, "SummerLessonVR2 cvars") != 0)
      LOG("cvars: no live reload (thread)");
    scePthreadAttrDestroy(attr);
  }
  while (g_shared.cmd != CMD_QUIT) {
    for (int i = 0; i < 2; i++) {
      volatile u64 *l = *(volatile u64 *volatile *)(vp + 0x2a8 + 8 * i);
      if (!l)
        continue;
      u64 v = *l;
      if (v != last[i]) {
        if (last[i])
          gpu_done();
        last[i] = v;
        trace_drawn((u32)v);
      }
    }
    if (astro_hdr.delta_time) { /* the game's per-frame time step (FApp::DeltaTime, a double at
       * astro_hdr.delta_time: Hikari's 0x46690d0): its spread per 10 s, in us; each game frame
       * writes it once */
      static double last_dt;
      static u64 n, sum, sq, mn = ~0ull, mx, off, t_last;
      double dt = *(volatile double *)((u8 *)astro_hdr.game_base + astro_hdr.delta_time);
      if (dt != last_dt) {
        last_dt = dt;
        u64 us = (u64)(dt * 1000000.0 + 0.5);
        if (us < g_rec_ts_min) g_rec_ts_min = (u32)us;
        if (us > g_rec_ts_max) g_rec_ts_max = (u32)us;
        n++, sum += us, sq += us * us;
        if (us < mn) mn = us;
        if (us > mx) mx = us;
        u64 per = astro_hdr.frame_pace == 1 ? (g_refresh_hz == 90 ? 11122 : 8342) : 16683;
        if (us + 500 < per || us > per + 500)
          off++;
      }
      u64 tn = now_us();
      if (!t_last)
        t_last = tn;
      if (tn - t_last > 10000000 && n) {
        u64 mean = sum / n, var = sq / n - mean * mean;
        u64 sd = 0;
        while ((sd + 1) * (sd + 1) <= var) sd++;
        LOG_DETAIL("timestep: %lu game frames, delta %lu us on average (sd %lu, %lu to %lu); %lu more than 0.5 ms "
            "off the refresh period", n, mean, sd, mn, mx, off);
        n = sum = sq = mx = off = 0, mn = ~0ull, t_last = tn;
      }
    }
#ifndef RELEASE
    rec_tick();
#endif
    sceKernelUsleep(200);
  }
  return 0;
}

void hooks_gpu_timing_start(void) {
  static volatile u32 started;
  multiview_native_confirm(); /* (before the engine's cap reads the frame pace) */
  if (!astro_hdr.viewport || !astro_hdr.game_base || !once(&started))
    return;
  u8 attr[64], thr[16];
  scePthreadAttrInit(attr);
  scePthreadAttrSetstacksize(attr, 64 * 1024);
  int rc = scePthreadCreate(thr, attr, gpu_label_thread, 0, "SummerLessonVR2 gpu");
  scePthreadAttrDestroy(attr);
  if (rc)
    LOG("gpu: thread -> 0x%x", rc);
}
static void gpu_done(void) {
  u64 fs = g_first_sub, t = now_us();
  g_first_sub = 0;
  if (fs && t > fs && t - fs < 100000) {
    u32 us = (u32)(t - fs);
    g_gpu_sum += us, g_gpu_n++;
    if (us > g_gpu_max)
      g_gpu_max = us;
    if (us > g_rec_gpu_max)
      g_rec_gpu_max = us;
  }
}
static s32 h_sceGnmSubmitCommandBuffers(u32 n, void **d, u32 *ds, void **c, u32 *cs) {
  frame_submitted();
  trace_submit();
  rtime_note(RT_SUBMIT, n, ds);
  return CALL_ORIG(s32(*)(u32, void **, u32 *, void **, u32 *), sceGnmSubmitCommandBuffers, n, d,
                   ds, c, cs);
}
static s32 h_sceGnmSubmitAndFlipCommandBuffers(u32 n, void **d, u32 *ds, void **c, u32 *cs,
                                               u32 vo, u32 buf, u32 mode, s64 arg) {
  frame_submitted();
  trace_submit();
  rtime_note(RT_SUBMIT, n, ds);
  return CALL_ORIG(s32(*)(u32, void **, u32 *, void **, u32 *, u32, u32, u32, s64),
                   sceGnmSubmitAndFlipCommandBuffers, n, d, ds, c, cs, vo, buf, mode, arg);
}

/* the health of the pace, every 10 s: what Hmd2's flips showed of what the game handed over. At a
 * frame every two refreshes: ~600 handed over, ~600 shown, ~599 shown again, 0 never shown; at a
 * frame a refresh (framerate.txt 120): ~1199 handed over and shown, 0 again, 0 never shown. */
/* each frame the game hands over: how many refreshes since the one before (1, 2, 3 or more), and
 * how long after the last refresh event it came (the frame's work after its wait), for the
 * native 120 Hz mode (measurement 2 drew exactly 60 a second) */
static volatile u32 g_int[4], g_after_n, g_after_max;
static volatile u64 g_after_sum;
/* the head pose's prediction: how far ahead of the asking the game predicts (GetResult's
 * prediction_time - now: its own formula is GetTime + base + frame time x 2.5, 0x2591f38) and how
 * far ahead of the hand-over the frame's pose then still is (ReprojectionStart's ts->timestamp -
 * now); the display is a refresh or so after the hand-over */
static volatile s64 g_lead_ask_sum, g_lead_hand_sum, g_lead_ask_min, g_lead_ask_max, g_lead_hand_min,
    g_lead_hand_max;
static volatile u32 g_lead_ask_n, g_lead_hand_n;
static void lead_note(volatile s64 *sum, volatile s64 *mn, volatile s64 *mx, volatile u32 *n, s64 v) {
  if (!*n || v < *mn)
    *mn = v;
  if (!*n || v > *mx)
    *mx = v;
  *sum += v, (*n)++;
}
static void frame_timing(void) {
  static u64 last_count;
  u64 c = g_vr.vbl_count, t = now_us(), v = g_vr.vbl_time;
  if (last_count) {
    u64 d = c - last_count;
    g_int[d >= 3 ? 3 : d]++;
  }
  last_count = c;
  if (v && t > v && t - v < 100000) {
    u32 a = (u32)(t - v);
    g_after_sum += a, g_after_n++;
    if (a > g_after_max)
      g_after_max = a;
  }
}

static void health_report(u32 handed) {
  LOG_DETAIL("hmd: the 10 s report at %lu us", (u64)now_us());
  if (g_opt_waits)
    census_report(&g_blocks, "block: the game's condition, nanosleep and event-flag waits");
  LOG_DETAIL("hmd: frames %u in the same refresh, %u a refresh after the last, %u two after, %u three "
      "or more after; handed over %u.%u ms after their refresh on average, %u.%u at the most",
      g_int[0], g_int[1], g_int[2], g_int[3], g_after_n ? (u32)(g_after_sum / g_after_n / 1000) : 0,
      g_after_n ? (u32)(g_after_sum / g_after_n / 100 % 10) : 0, g_after_max / 1000,
      g_after_max / 100 % 10);
  g_int[0] = g_int[1] = g_int[2] = g_int[3] = 0, g_after_n = g_after_max = 0, g_after_sum = 0;
  if (g_lead_ask_n || g_lead_hand_n) {
    LOG_DETAIL("pose: predicted %ld.%ld ms ahead when the game asked (%ld.%ld to %ld.%ld), %ld.%ld ms ahead "
        "at the hand-over (%ld.%ld to %ld.%ld)",
        g_lead_ask_n ? g_lead_ask_sum / g_lead_ask_n / 1000 : 0,
        g_lead_ask_n ? (g_lead_ask_sum / g_lead_ask_n / 100) % 10 : 0, g_lead_ask_min / 1000,
        (g_lead_ask_min / 100) % 10, g_lead_ask_max / 1000, (g_lead_ask_max / 100) % 10,
        g_lead_hand_n ? g_lead_hand_sum / g_lead_hand_n / 1000 : 0,
        g_lead_hand_n ? (g_lead_hand_sum / g_lead_hand_n / 100) % 10 : 0, g_lead_hand_min / 1000,
        (g_lead_hand_min / 100) % 10, g_lead_hand_max / 1000, (g_lead_hand_max / 100) % 10);
    g_lead_ask_sum = g_lead_hand_sum = 0, g_lead_ask_n = g_lead_hand_n = 0;
  }
  vr_jitter_report();
  {
    static u32 said;
    u32 r = g_pool_retries;
    size_t flex = 0;
    if (sceKernelAvailableFlexibleMemorySize && sceKernelAvailableFlexibleMemorySize(&flex) == 0 &&
        flex < g_flex_min)
      g_flex_min = flex;
    if (r > said) {
      LOG_DETAIL("dmem: the game's CPU pool retried its 224 MB %u times in 10 s, refused (it copes)", r - said);
      said = r;
    }
    static u32 n;
    if (n++ % 6 == 0) /* (a minute) */
      LOG("dmem: flexible memory free %lu MB (the least this session %lu MB)%s", (u64)flex >> 20,
          g_flex_min >> 20, g_pool_granted ? "; the CPU pool's 224 MB given" : "");
  }
  trace_report();
  fp_report();
  if (g_held[1] || g_held[2] || g_held[3] || g_held[4]) {
    LOG_DETAIL("hmd: frames on the headset for 1 refresh %u, 2 %u, 3 %u, 4 or more %u", g_held[1],
        g_held[2], g_held[3], g_held[4]);
    g_held[1] = g_held[2] = g_held[3] = g_held[4] = 0;
  }
  u32 shown = g_shown, again = g_repeated, n = g_gpu_n;
  u64 sum = g_gpu_sum;
  u32 max = g_gpu_max;
  g_shown = g_repeated = 0;
  g_gpu_sum = 0, g_gpu_n = 0, g_gpu_max = 0;
  LOG("hmd: %u frames handed over, %u shown, %u shown again, %u never shown; the game's GPU "
      "%u.%u ms a frame on average, %u.%u at the most (%u frames)", handed, shown, again,
      __atomic_exchange_n(&g_never, 0, __ATOMIC_RELAXED), n ? (u32)(sum / n / 1000) : 0,
      n ? (u32)(sum / n / 100 % 10) : 0, max / 1000, max / 100 % 10, n);
}

static s32 h_sceHmdReprojectionStart(const struct hmd_reprojection_param *p,
                                     const struct hmd_tracker_state *ts, s64 flip_arg,
                                     s32 option) {
  rtime_note(RT_HANDOVER, 0, 0);
  if (!p || !ts || !p->texture[0] || !p->texture[1])
    return HMD_ERR_PARAMETER_NULL;
  u32 n = __atomic_fetch_add(&g_vr.frames_in, 1, __ATOMIC_RELAXED);
  { /* how many frames the game draws: 60 a second at full speed (resolution.txt) */
    static u64 since;
    static u32 n0;
    u64 now = sceKernelGetProcessTime();
    if (!since)
      since = now, n0 = n;
    else if (now - since >= 10000000) {
      u32 d = n - n0;
      __atomic_store_n(&g_health_due, d ? d : 1, __ATOMIC_RELEASE); /* (reported off this thread) */
      since = now, n0 = n;
    }
  }
  if (n < 3 || (n % 3600) == 0) {
    LOG("hmd: ReprojectionStart #%u flip_arg %ld option %d flags 0x%x u40 0x%x u48 0x%lx u50 0x%x "
        "label %p",
        n, flip_arg, option, p->flags, p->unknown_40, p->unknown_48, p->unknown_50,
        p->frame_label);
    log_tsharp("L", p->texture[0]);
    log_tsharp("R", p->texture[1]);
    if (p->sampler) {
      const u32 *s = p->sampler;
      LOG("  S# %08x %08x %08x %08x", s[0], s[1], s[2], s[3]);
    }
    LOG("  uv L %f %f %f %f  R %f %f %f %f", p->uv[0].scale_x, p->uv[0].scale_y,
        p->uv[0].offset_x, p->uv[0].offset_y, p->uv[1].scale_x, p->uv[1].scale_y,
        p->uv[1].offset_x, p->uv[1].offset_y);
    LOG("  pose %f %f %f  q %f %f %f %f  ts %lu frame %u", ts->position[0], ts->position[1],
        ts->position[2], ts->orientation[0], ts->orientation[1], ts->orientation[2],
        ts->orientation[3], ts->timestamp, ts->user_frame_number);
  }
  frame_timing();
  if (ts->timestamp) {
    lead_note(&g_lead_hand_sum, &g_lead_hand_min, &g_lead_hand_max, &g_lead_hand_n,
              (s64)ts->timestamp - (s64)now_us());
  }
  hooks_gpu_timing_start();
  trace_handover((u32)hooks_frame_count());
  vr_submit_frame(p, ts, flip_arg); /* with the frame's count (hooks_frame_count), before it is out */
  vr2_frame_ready(); /* the hand-off thread (vr2.c) passes it to Hmd2 at once */
  /* The PS VR reprojection zeroes the label of every frame older than the newest once it is
   * done with it (libSceHmd sub_fff0); the game waits for that before reusing an eye buffer. */
  static u64 *last_label;
  if (last_label && last_label != p->frame_label)
    *(volatile u64 *)last_label = 0;
  last_label = p->frame_label;
  agent_tick();
  return 0;
}

static s32 h_sceHmdReprojectionStop(void) {
  LOG("hmd: ReprojectionStop");
  return 0;
}

/* The game's 2D picture in the headset (Summer Lesson's PS4 RHI uses it for screens drawn flat:
 * its title and loading screens). The parameter, as libSceHmd checks it: the texture (a GCN T#),
 * its sampler, the uv (scale and offset, 16 bytes), the frame label, a u32 at +0x28, and zeroes
 * from +0x30. Shown as a flat screen fixed before the seat (vr.c, vr2.c), the same picture to both
 * eyes; counted as a frame, so PS VR2 is brought up on these screens too. */
static s32 h_sceHmdReprojectionStart2dVr(const void *p, s64 flip_arg, s32 option) {
  static u32 n;
  const u8 *b = p;
  const u32 *t = b ? *(const u32 *const *)b : 0;
  if (!t)
    return HMD_ERR_PARAMETER_NULL;
  u32 k = __atomic_fetch_add(&g_vr.frames_in, 1, __ATOMIC_RELAXED);
  if (k < 2 || n < 6 || (k % 3600) == 0) {
    LOG("hmd: ReprojectionStart2dVr #%u (%p, flip_arg %ld, option %d) u28 0x%x label %p", k, p,
        flip_arg, option, *(const u32 *)(b + 0x28), *(void *const *)(b + 0x20));
    log_tsharp("2D", t);
    const float *uv = (const float *)(b + 0x10);
    LOG("  uv %f %f %f %f", uv[0], uv[1], uv[2], uv[3]);
    n++;
  }
  vr_submit_flat(t, *(const u32 *const *)(b + 8), (const struct hmd_eye_uv *)(b + 0x10),
                 *(u64 *const *)(b + 0x20), flip_arg);
  vr2_frame_ready();
  /* as the real library: the label of the frame before is zeroed once this one is in (the game
   * reuses a texture only once its label is zero) */
  static u64 *last_label;
  u64 *label = *(u64 *const *)(b + 0x20);
  if (last_label && last_label != label)
    *(volatile u64 *)last_label = 0;
  last_label = label;
  agent_tick();
  return 0;
}

static s32 h_sceHmdReprojectionUnsetDisplayBuffers(void) {
  LOG("hmd: ReprojectionUnsetDisplayBuffers");
  return 0;
}

/* 120 Hz: the PS VR reprojection's end-of-refresh event, which the game paces on. Once PS VR2
 * shows the game, it is raised on each refresh of the headset (Hmd2 flips it every one); before
 * that, and if a refresh is late, on a clock at the headset's 119.88 Hz (89.91 in its 90 Hz mode:
 * framerate.txt). A clock of its own
 * would drift from the headset's: every few seconds a frame would come too many or too few. */
static void *vblank_thread(void *arg) {
  (void)arg;
  /* Made from the game's main thread, it would run where that does (core 0 above all) and wake
   * every fraction of a millisecond there: onto the cores and priority of the agent's other
   * threads and the game's own PS VR thread (Hmd::ReproThread: 4 to 6, 256). */
  {
    void *self = scePthreadSelf();
    int a = scePthreadSetaffinity ? scePthreadSetaffinity(self, 0x70) : -1;
    int pr = scePthreadSetprio ? scePthreadSetprio(self, 256) : -1;
    LOG("vblank: on cores 0x70 (0x%x), priority 256 (0x%x)", a, pr);
    void *eq = 0;
    int c = sceKernelCreateEqueue ? sceKernelCreateEqueue(&eq, "SummerLessonVR2 vblank") : -1;
    int e = c == 0 ? sceKernelAddUserEventEdge(eq, WAKE_FLIP) : -1;
    if (c == 0 && e == 0)
      g_wake_eq = eq;
    LOG("vblank: woken by Hmd2's flips (0x%x, 0x%x)", c, e);
  }
  u64 next_ns = now_us() * 1000, last_sync = 0;
  u32 seen = gen1_refreshes(), locked = 0, switches = 0;
  while (g_shared.cmd != 1) {
    /* a refresh in us: the headset's 120 Hz (119.88), or 90 (89.91) in its 90 Hz mode */
    u32 hz = g_refresh_hz ? g_refresh_hz : astro_hdr.refresh_hz == 90 ? 90 : 120;
    u32 period = 1001000 / hz;
    /* a refresh is due a period after the last: sleep most of that, then watch for it */
    u32 doze = period - period / 6;
    /* the clock read once: read again, it could have passed the moment (the sleep then wrapping
     * to 71 minutes, no refresh events, the game frozen waiting for one) */
    u64 now0 = now_us();
    if (locked && last_sync + doze > now0)
      vbl_sleep(last_sync + doze, 1);
    u64 t0 = now_us();
    int synced = 0;
    for (u64 t = t0; t - t0 < (locked ? 6000 : 1000); t = now_us()) {
      u32 r = gen1_refreshes();
      if (r != seen) {
        seen = r, synced = 1;
        break;
      }
      vbl_sleep(t + (locked ? 200 : 500), 1);
    }
    if (synced != (int)locked) {
      locked = synced;
      if (!locked)
        g_rec_clockfb++;
      if (switches++ < 16)
        LOG("vblank: %s (%u Hz)", locked ? "on the headset's refreshes" : "on the clock", hz);
    }
    if (synced) {
      last_sync = now_us();
      next_ns = last_sync * 1000;
    } else {
      next_ns += 1001000000ull / hz; /* ns */
      u64 t = now_us(), next = next_ns / 1000;
      if (next > t)
        vbl_sleep(next, 0);
      else if (t - next > 100000)
        next_ns = t * 1000;
    }
    u32 k = __atomic_fetch_add(&g_vr.vblanks, 1, __ATOMIC_RELAXED);
    if (k % 7200 == 7199)
      LOG_DETAIL("status: %u refreshes, %u frames from the game, %u tracker reads", k + 1,
          g_vr.frames_in, g_vr.tracker_reads);
    raise_vblank(k, 1, 1);
  }
  return 0;
}

/* the vblank thread's sleep until `until` (us), ended early by Hmd2's flip where `on_flip`
 * (returns 1 then) */
static int vbl_sleep(u64 until, int on_flip) {
  for (;;) {
    u64 t = now_us();
    if (t >= until)
      return 0;
    void *eq = g_wake_eq;
    if (!eq) {
      sceKernelUsleep((u32)(until - t));
      return 0;
    }
    u8 ev[2][32];
    int out = 0;
    u32 timo = (u32)(until - t);
    int rc = sceKernelWaitEqueue(eq, ev, 2, &out, &timo);
    static u32 spurious; /* returns with nothing, in a row */
    if (out <= 0) {
      if (until - t > 200 && now_us() - t < 50 && ++spurious >= 3) {
        /* neither a flip nor the timeout: the wait doesn't work here, sleep as before */
        LOG("vblank: the wait returned 0x%x at once: sleeping instead", rc);
        g_wake_eq = 0;
      }
      continue;
    }
    spurious = 0;
    int flipped = 0;
    for (int i = 0; i < out && i < 2; i++)
      flipped |= *(u64 *)ev[i] == WAKE_FLIP;
    if (flipped && on_flip)
      return 1;
  }
}

/* a refresh of the game's PS VR; `shown`: one of the headset's, where the newest frame is shown;
 * `counted`: a refresh in the flip status */
static void raise_vblank(u32 k, int shown, int counted) {
  if (g_vr.vbl_eq) {
    /* As the PS VR reprojection at half rate: a frame is shown for two refreshes, then the
     * newest one the game handed over; at 90 fps for one of the headset's. */
    static u32 shown_for;
    u32 w = g_vr.frame_w;
    u32 pace = astro_hdr.frame_pace == 1 ? 1 : 2; /* 1: the game's native 120 (framerate.txt) */
    if (shown && ++shown_for >= pace && w &&
        g_vr.frame[w & 3].flip_arg != g_vr.vbl_flip_arg) {
      g_vr.vbl_flip_arg = g_vr.frame[w & 3].flip_arg;
      shown_for = 0;
    }
    if (counted) {
      u32 lo, hi;
      __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
      g_vr.vbl_tsc = ((u64)hi << 32) | lo;
      g_vr.vbl_time = now_us();
      __atomic_fetch_add(&g_vr.vbl_count, 1, __ATOMIC_RELAXED); /* also from the game's threads */
    }
    sceKernelTriggerUserEvent(g_vr.vbl_eq, VBL_USER_ID, 0);
  }
  if (g_vr.end_eq) {
    int rc = sceKernelTriggerUserEvent(g_vr.end_eq, g_vr.end_id, 0);
    if (rc != 0) {
      /* The real library adds the user event to the game's queue itself. */
      int a = sceKernelAddUserEvent(g_vr.end_eq, g_vr.end_id);
      int rc2 = sceKernelTriggerUserEvent(g_vr.end_eq, g_vr.end_id, 0);
      static u32 n;
      LOG_FIRST(n, 4, "vblank: trigger -> 0x%x, AddUserEvent -> 0x%x, trigger -> 0x%x", rc, a, rc2);
    } else if (k < 2 && shown) {
      LOG("vblank: trigger ok");
    }
  }
}

/* ---------------------------------------------------------------- main port at 120 Hz */
/* The game paces its renderer on vblank events of its main VideoOut port (on the queue it gave
 * the PS VR reprojection) and on that port's flip status: with PS VR the reprojection flipped
 * it at 120 Hz with each frame's flip_arg. Here it is the 60 Hz TV and nothing flips it, which
 * holds the game to 30 fps. Its vblank event there is replaced by a user event the vblank
 * thread raises every 120 Hz refresh, handed to the game as a VideoOut event, and the port's
 * flip status reports those refreshes. */
#define EVFILT_USER_ (-11)
#define EVFILT_VIDEO_OUT_ (-13)

static s32 h_sceVideoOutAddVblankEvent(void *eq, s32 vo, void *udata) {
  /* Astro Bot added it on the queue it gave the PS VR reprojection's end event; Summer Lesson
   * (Unreal's PS4 RHI) on a queue of its own, "TrackerVblankQueue", and waits for two of these
   * events a frame in its 120 Hz VR mode. The first vblank event the game registers, on the port
   * it opened for its renderer, is the one: it becomes ours. */
  if (!g_vr.vbl_eq && eq &&
      (g_main_port < 0 || vo == g_main_port || (g_vr.end_eq && eq == g_vr.end_eq))) {
    g_vr.vbl_eq = eq;
    g_vr.main_vo = vo;
    s32 rc = sceKernelAddUserEventEdge(eq, VBL_USER_ID); /* auto-clearing */
    LOG("videoout: AddVblankEvent(eq %p, port 0x%x, udata %p)%s -> 120 Hz user event (0x%x)", eq,
        vo, udata, g_vr.end_eq && eq == g_vr.end_eq ? " on the VR queue" : "", rc);
    agent_tick();
    return 0;
  }
  LOG("videoout: AddVblankEvent(eq %p, port 0x%x, udata %p): another port, passed through", eq, vo,
      udata);
  return CALL_ORIG(s32(*)(void *, s32, void *), sceVideoOutAddVblankEvent, eq, vo, udata);
}

/* the game reads the count of its vblank event: the data field as libSceVideoOut packs it (the
 * count in bits 12-15, the event's data above) */
static s32 h_sceVideoOutGetEventCount(const u8 *ev) {
  s32 rc = CALL_ORIG(s32(*)(const u8 *), sceVideoOutGetEventCount, ev);
  static u32 n;
  LOG_FIRST(n, 2, "videoout: GetEventCount(ident %lu, data 0x%lx) -> %d", ev ? *(const u64 *)ev : 0,
            ev ? *(const u64 *)(ev + 16) : 0, rc);
  return rc;
}

static s32 h_sceVideoOutSetFlipRate(s32 h, s32 rate) {
  s32 rc = CALL_ORIG(s32(*)(s32, s32), sceVideoOutSetFlipRate, h, rate);
  LOG("videoout: SetFlipRate(0x%x, %d) -> 0x%x", h, rate, rc);
  return rc;
}

static s32 h_sceVideoOutUnregisterBuffers(s32 h, s32 set) {
  s32 rc = CALL_ORIG(s32(*)(s32, s32), sceVideoOutUnregisterBuffers, h, set);
  LOG("videoout: UnregisterBuffers(0x%x, set %d) -> 0x%x", h, set, rc);
  return rc;
}

/* the game waits for the TV's vblank only on its way out of VR mode (flips pending) */
static s32 h_sceVideoOutWaitVblank(s32 h) {
  static u32 n;
  LOG_FIRST(n, 4, "videoout: WaitVblank(0x%x)", h);
  return CALL_ORIG(s32(*)(s32), sceVideoOutWaitVblank, h);
}

/* The game's waits on its VR queue: our user event becomes the VideoOut vblank event it expects.
 * The first waits also say what else the game waits on there (its timers, its GPU events): the
 * map of its frame loop, for later. */
static s32 h_sceKernelWaitEqueue(void *eq, u8 *ev, s32 num, s32 *out, u32 *timo) {
  rtime_note(RT_WAIT, 0, 0);
  int vbl = eq && eq == g_vr.vbl_eq;
  s32 rc = CALL_ORIG(s32(*)(void *, u8 *, s32, s32 *, u32 *), sceKernelWaitEqueue, eq, ev, num, out,
                     timo);
  if (rc == 0 && vbl && ev && out)
    for (s32 i = 0; i < *out; i++) {
      u8 *e = ev + (u64)i * 32; /* struct kevent: ident, filter, flags, fflags, data, udata */
      u64 ident = *(u64 *)e;
      s16 filter = *(s16 *)(e + 8);
      if (ident == VBL_USER_ID && filter == EVFILT_USER_) {
        *(u64 *)e = 1; /* SCE_VIDEO_OUT_EVENT_VBLANK */
        *(s16 *)(e + 8) = EVFILT_VIDEO_OUT_;
        /* data as libSceVideoOut packs it: the event's data (the refresh count) above bit 16,
         * the number of events in bits 12-15 */
        *(u64 *)(e + 16) = (g_vr.vbl_count << 16) | (1u << 12);
      } else {
        static u32 n;
        LOG_FIRST(n, 12, "vrqueue: the game's VR queue gave event ident 0x%lx filter %d data 0x%lx",
                  ident, filter, *(u64 *)(e + 16));
      }
    }
  return rc;
}

/* The real flip state of a port, not the pace the game is told (below): the flips submitted and not
 * yet shown, and the buffer on screen. For the TV mirror, which must not write into a buffer the TV
 * still shows or is about to show. -1 when it cannot be read. */
int hooks_flip_state(s32 vo, s32 *pending, s32 *shown) {
  u64 st[8] = {0};
  if (!orig_sceVideoOutGetFlipStatus ||
      CALL_ORIG(s32(*)(s32, u64 *), sceVideoOutGetFlipStatus, vo, st) != 0)
    return -1;
  *pending = (s32)(st[6] >> 32); /* flipPendingNum */
  *shown = (s32)(u32)st[7];      /* currentBuffer */
  return 0;
}

/* SceVideoOutFlipStatus: count, processTime, tsc, flipArg, submitTsc, reserved, gcQueueNum,
 * flipPendingNum, currentBuffer */
static s32 h_sceVideoOutGetFlipStatus(s32 vo, u64 *st) {
  s32 rc = CALL_ORIG(s32(*)(s32, u64 *), sceVideoOutGetFlipStatus, vo, st);
  /* the pace is ours (the count, its time and tsc, the flip argument); the rest (the flips
   * pending on the real port, the buffer shown) stays real: Summer Lesson flips the port blank
   * and waits for that flip to be done before it changes the port's buffers, and it registers
   * its vblank event before it enters VR mode */
  if (g_vr.vbl_eq && vo == g_vr.main_vo && st) {
    if (rc != 0)
      memset(st, 0, 0x40);
    st[0] = g_vr.vbl_count;
    st[1] = g_vr.vbl_time;
    st[2] = g_vr.vbl_tsc;
    st[3] = (u64)g_vr.vbl_flip_arg;
    st[4] = g_vr.vbl_tsc;
    return 0;
  }
  return rc;
}

/* ---------------------------------------------------------------- pad */
/* The game copies only acceleration and angular velocity from each sample; libScePad's own
 * fused orientation (+0x0c, tilt-corrected) becomes the tracked controller's orientation. */
/* While a system screen has the controller (PS VR2's safety notice, its play-area setup, the PS
 * menu), libScePad marks the game's readings intercepted (buttons bit 31): when that was last
 * seen, from the real reading before a test script replaces it. hand.c waits for it to clear. */
#define PAD_INTERCEPTED 0x80000000u
volatile u64 g_pad_intercepted_at;
static void note_intercepted(const u8 *data) {
  static u32 was; /* the game's pad thread and the hand thread both read it: one of them logs */
  u32 now = (*(const u32 *)data & PAD_INTERCEPTED) != 0;
  if (now)
    __atomic_store_n(&g_pad_intercepted_at, now_us(), __ATOMIC_RELEASE);
  if (__atomic_exchange_n(&was, now, __ATOMIC_RELAXED) != now) {
    static u32 n;
    if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < (g_opt_handwatch ? 200u : 16u))
      LOG_DETAIL("pad: %s (%u.%03u s)", now ? "a system screen has the controller" : "the controller is the game's again",
          (u32)(now_us() / 1000000), (u32)(now_us() / 1000 % 1000));
  }
}

/* The game's controller read now, beside the game (scePadReadState takes no samples from it),
 * for when the game stops reading while a system screen is up. */
static volatile s32 g_pad_player = -1;
int hooks_pad_held(void) {
  s32 h = __atomic_load_n(&g_pad_player, __ATOMIC_ACQUIRE);
  u8 d[0x100] __attribute__((aligned(16))); /* ScePadData: 0x78 bytes */
  if (h < 0 || !orig_scePadReadState || CALL_ORIG(s32(*)(s32, u8 *), scePadReadState, h, d) != 0)
    return -1;
  note_intercepted(d);
  return (*(const u32 *)d & PAD_INTERCEPTED) != 0;
}

/* The game's controller: the test mode's recording or script (pads.c), then the view gestures
 * and the orientation for the tracked controller (vr.c). */
static s32 h_scePadRead(s32 handle, u8 *data, s32 num) {
  rtime_note(RT_PAD, 0, 0);
  static s32 player = -1; /* the first controller the game reads is the one it tracks */
  s32 n = CALL_ORIG(s32(*)(s32, u8 *, s32), scePadRead, handle, data, num);
  for (s32 i = 0; data && (handle == player || player < 0) && i < n; i++)
    note_intercepted(data + (u64)i * 0x78);
  if (handle == player || (player < 0 && n <= 0)) /* the player's, or none answering yet */
    n = pads_test_read(handle, data, num, n);     /* test.txt: recorded, or the script's */
  if (player < 0 && n > 0) {
    player = handle;
    s32 none = -1; /* hooks_pad_held's handle, unless scePadReadState gave it one first */
    __atomic_compare_exchange_n(&g_pad_player, &none, handle, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  }
  for (s32 i = 0; data && handle == player && i < n; i++)
    vr_pad_read(data + (u64)i * 0x78);
  return n;
}

/* Summer Lesson reads its controller with scePadReadState (one sample): the same path. */
static s32 h_scePadReadState(s32 handle, u8 *data) {
  static s32 player = -1;
  s32 rc = CALL_ORIG(s32(*)(s32, u8 *), scePadReadState, handle, data);
  if (data && rc == 0 && (handle == player || player < 0))
    note_intercepted(data);
  if (handle == player || player < 0) {
    s32 n = pads_test_read(handle, data, 1, rc == 0 ? 1 : 0); /* test.txt: recorded, or the script's */
    if (n > 0)
      rc = 0;
  }
  if (player < 0 && rc == 0) {
    player = handle;
    __atomic_store_n(&g_pad_player, handle, __ATOMIC_RELEASE);
    /* The tracked controller turns with libScePad's fused orientation (+0x0c), which the pad only
     * computes with its motion sensors on: Astro Bot switched them on, Summer Lesson never does
     * (it imports neither call; play test 1: "controller tracking seems to not be working"). */
    s32 m = scePadSetMotionSensorState ? scePadSetMotionSensorState(handle, 1) : -1;
    s32 t = scePadSetTiltCorrectionState ? scePadSetTiltCorrectionState(handle, 1) : -1;
    LOG("pad: controller 0x%x: motion sensors on (0x%x), tilt correction on (0x%x)", handle, m, t);
  }
  if (data && handle == player && rc == 0) {
    static u64 next;
    u64 t = now_us();
    if (t >= next) { /* the orientation the game's controller is turned by, every 2 s */
      next = t + 2000000;
      const float *q = (const float *)(data + 0x0c);
      LOG_DETAIL("pad: orientation %f %f %f %f, turning %f %f %f, connected %u", q[0], q[1], q[2], q[3],
          ((const float *)(data + 0x28))[0], ((const float *)(data + 0x28))[1],
          ((const float *)(data + 0x28))[2], data[0x4c]);
    }
#ifndef RELEASE
    menu_pad(data); /* the debug menu: its buttons, and nothing to the game while it is open */
#endif
    vr_pad_read(data);
#ifndef RELEASE
    rec_pad(handle, data);
#endif
  }
  return rc;
}

/* ---------------------------------------------------------------- system service */
/* With PS VR2 in VR mode the system reports this PS4 title as running in the background
 * (status byte 5), and the game pauses on that (its VR loop then hands over a black eye
 * texture). The system-UI-overlay flag (byte 4) still pauses it, so the PS menu works. */
static s32 h_sceSystemServiceGetStatus(u8 *st) {
  s32 rc = CALL_ORIG(s32(*)(u8 *), sceSystemServiceGetStatus, st);
  if (rc == 0 && st && g_vr.vr2_started)
    st[5] = 0;
  /* With the headset off the player's head, a console puts its own screen over the game
   * (isSystemUiOverlaid, byte 4), and that is what the game pauses for (AstroQuest). */
  if (rc == 0 && st && g_vr.hmd_off)
    st[4] = 1;
  /* the game only asks for events while the status counts some (eventNum, +0) */
  if (rc == 0 && st && vr_reset_event(0))
    *(s32 *)st += 1;
  return rc;
}

/* A seat reset tells the game to recentre, as a console does when the player resets the view. */
#define SYSTEM_EVENT_RESET_VR_POSITION 0x1000000a
static s32 h_sceSystemServiceReceiveEvent(u8 *ev) {
  if (ev && vr_reset_event(1)) {
    memset(ev, 0, 4 + 8192); /* SceSystemServiceEvent: type, 8 KiB of parameters */
    *(u32 *)ev = SYSTEM_EVENT_RESET_VR_POSITION;
    LOG("system: ResetVrPosition -> game");
    return 0;
  }
  s32 rc = CALL_ORIG(s32(*)(u8 *), sceSystemServiceReceiveEvent, ev);
  /* what the system tells the game, PS VR2's own screens among it (agent.handwatch's question) */
  static u32 n;
  if (rc == 0 && ev && __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 40)
    LOG_DETAIL("system: event 0x%x -> game (%u.%03u s)", *(u32 *)ev, (u32)(now_us() / 1000000),
        (u32)(now_us() / 1000 % 1000));
  return rc;
}

/* ---------------------------------------------------------------- setup dialog */
static s32 h_sceHmdSetupDialogInitialize(void) {
  static u32 n;
  LOG_FIRST(n, 3, "hmdsetup: Initialize");
  return 0;
}
static s32 h_sceHmdSetupDialogOpen(const void *param) {
  /* OrbisHmdSetupDialogParam: CommonDialog base (48 bytes), u64 size, s32 user_id */
  s32 user = param ? *(const s32 *)((const u8 *)param + 56) : -1;
  static u32 n;
  LOG_FIRST(n, 3, "hmdsetup: Open(%p) user %d", param, user);
  if (user > 0) /* Summer Lesson leaves it 0: the initial user (GetDeviceInformation) stands */
    g_vr.user_id = user;
  return 0;
}
static s32 h_sceHmdSetupDialogClose(void) {
  static u32 n;
  LOG_FIRST(n, 3, "hmdsetup: Close");
  return 0;
}
/* A real dialog the game opened before the hooks went in (the agent came late) is ShellUI's
 * "connect PS VR" screen. Terminate alone left it on screen until a quick-settings visit or a
 * headset off/on cleared it (issue #4), so it is closed the CommonDialog way: real Close, real
 * UpdateStatus until FINISHED, and only then does the game see FINISHED and go on. */
static s32 h_sceHmdSetupDialogUpdateStatus(void) {
  static u32 closing;
  static u64 since;
  s32 real = CALL_ORIG(s32(*)(void), sceHmdSetupDialogUpdateStatus);
  if (real == CDLG_STATUS_RUNNING) {
    if (!closing) {
      closing = 1;
      since = now_us();
      s32 rc = sceHmdSetupDialogClose ? sceHmdSetupDialogClose() : -1;
      LOG("hmdsetup: the real dialog is up (opened before the agent): Close -> 0x%x", rc);
    }
    if (now_us() - since < 5000000)
      return CDLG_STATUS_RUNNING;
    if (closing == 1) {
      closing = 2;
      LOG("hmdsetup: the real dialog is still up after 5 s");
    }
    return CDLG_STATUS_FINISHED;
  }
  if (closing) {
    LOG("hmdsetup: the real dialog closed after %lu ms (status 0x%x)",
        (unsigned long)((now_us() - since) / 1000), real);
    closing = 0;
  }
  return CDLG_STATUS_FINISHED;
}
static s32 h_sceHmdSetupDialogGetResult(struct hmd_setup_dialog_result *r) {
  static u32 n;
  LOG_FIRST(n, 3, "hmdsetup: GetResult -> OK");
  if (r)
    r->result = 0;
  return 0;
}
static s32 h_sceHmdSetupDialogTerminate(void) {
  static u32 n;
  /* A dialog the real library opened before the hooks went in is closed by the real one. */
  s32 real = CALL_ORIG(s32(*)(void), sceHmdSetupDialogTerminate);
  LOG_FIRST(n, 3, "hmdsetup: Terminate (real -> 0x%x)", real);
  return 0;
}

/* ---------------------------------------------------------------- libSceVrTracker */
static s32 h_sceVrTrackerQueryMemory(const void *param, struct vrt_query_memory_result *r) {
  LOG("vrt: QueryMemory");
  if (!r)
    return VRT_ERR_ARGUMENT_INVALID;
  u32 size = r->size;
  memset(r, 0, sizeof *r);
  r->size = size;
  r->onion_size = 0x400000;
  r->onion_align = 0x10000;
  r->garlic_size = 0x3000000;
  r->garlic_align = 0x10000;
  r->work_size = 0x1000000;
  r->work_align = 0x10000;
  return 0;
}

static s32 h_sceVrTrackerInit(const void *param) {
  LOG("vrt: Init(%p)", param);
  g_vr.tracker_init = 1;
  return 0;
}

static s32 h_sceVrTrackerTerm(void) {
  LOG("vrt: Term");
  g_vr.tracker_init = 0;
  return 0;
}

static s32 h_sceVrTrackerRegisterDevice(s32 type, s32 handle) {
  LOG("vrt: RegisterDevice(type %d, handle 0x%x)", type, handle);
  if (type == VRT_DEVICE_HMD)
    g_vr.hmd_tracker_handle = handle;
  else if (type == VRT_DEVICE_DUALSHOCK4 && !g_vr.pad_tracker_handle)
    g_vr.pad_tracker_handle = handle;
  return 0;
}

static s32 h_sceVrTrackerUnregisterDevice(s32 handle) {
  LOG("vrt: UnregisterDevice(0x%x)", handle);
  if (handle == g_vr.pad_tracker_handle)
    g_vr.pad_tracker_handle = 0;
  return 0;
}

static s32 h_sceVrTrackerGpuSubmit(const void *p) { return 0; }
static s32 h_sceVrTrackerGpuWait(const void *p) { return 0; }
static s32 h_sceVrTrackerCpuProcess(const void *p) { return 0; }
static s32 h_sceVrTrackerGpuWaitAndCpuProcess(void) { return 0; } /* Summer Lesson's tracker thread */
/* the game feeds its controllers' motion sensors to the tracker (param: size 0x20, device type) */
static s32 h_sceVrTrackerUpdateMotionSensorData(const u32 *p) {
  static u32 n;
  LOG_FIRST(n, 3, "vrt: UpdateMotionSensorData(device %u)", p ? p[1] : 0);
  return 0;
}
/* the tracker's clock, which the game's prediction times are counted from */
static s32 h_sceVrTrackerGetTime(u64 *out) {
  static u32 n;
  if (!out)
    return VRT_ERR_ARGUMENT_INVALID;
  *out = now_us();
  LOG_FIRST(n, 2, "vrt: GetTime -> %lu", *out);
  return 0;
}
/* A console's tracker answers "calibrating" for a moment after sceVrTrackerRecalibrate, then
 * "tracking" again. The game from 1.01 on waits on its silhouette screen until it has seen that
 * for the controller (AstroQuest 0.18), so at least one result says so, however late it asks. */
static struct {
  volatile u64 until;
  volatile u32 reported;
} g_recal[4] = {{0, 1}, {0, 1}, {0, 1}, {0, 1}};

static int recalibrating(u32 type, u64 now) {
  if (g_recal[type].reported && now >= g_recal[type].until)
    return 0;
  g_recal[type].reported = 1;
  return 1;
}

static s32 h_sceVrTrackerRecalibrate(const u32 *p) {
  u32 type = p ? p[1] : 0;
  LOG("vrt: Recalibrate(device %u, calibration %u)", type, p ? p[2] : 0);
  if (type < 4) {
    g_recal[type].until = now_us() + 200000;
    g_recal[type].reported = 0;
  }
  if (type == VRT_DEVICE_HMD)
    vr_recenter();
  return 0;
}
/* How the game registers its controllers (Func_B4D26B7D8B18DF06(type, handle, colour); the HMD
 * goes through RegisterDevice): type 1 = DualShock 4, 2 = Move; colour = light bar 0..4. */
static s32 h_sceVrTrackerFuncB4D2(s32 type, s32 handle, s32 colour) {
  LOG("vrt: register controller (type %d, handle 0x%x, colour %d)", type, handle, colour);
  if (type == VRT_DEVICE_DUALSHOCK4 && !g_vr.pad_tracker_handle) {
    g_vr.pad_tracker_handle = handle;
    g_vr.pad_led = colour;
  }
  return 0;
}

static void write_pose(struct vrt_pose *o, const struct pose *p) {
  o->px = p->pos[0];
  o->py = p->pos[1];
  o->pz = p->pos[2];
  o->qx = p->rot[0];
  o->qy = p->rot[1];
  o->qz = p->rot[2];
  o->qw = p->rot[3];
}

static s32 h_sceVrTrackerGetResult(const struct vrt_get_result_param *param, struct vrt_result *r) {
  static u32 n;
  if (!param || !r)
    return VRT_ERR_ARGUMENT_INVALID;
  s32 handle = param->handle;
  int is_hmd = handle == g_vr.hmd_tracker_handle;
  int is_pad = !is_hmd && handle == g_vr.pad_tracker_handle;
  memset(r, 0, sizeof *r);
  r->handle = handle;
  r->user_frame_number = param->user_frame_number;
  r->camera_orientation[3] = 1.0f;
  if (!is_hmd && !is_pad) {
    r->status = 2; /* not tracking */
    return 0;
  }
  agent_tick();
  __atomic_fetch_add(&g_vr.tracker_reads, 1, __ATOMIC_RELAXED);
  struct tracked dev;
  u64 now = now_us();
  /* the game predicts ~33 ms ahead at 120 (55 at 60): GetTime + base + frame time x 2.5
   * (0x2591f38); a 10 ms cap tried on 2026-11-01 did not change the native modes' jitter */
  u64 pred = param->prediction_time;
  /* agent.predict=N: the headset's pose predicted N ms ahead instead (the game's ~33 ms at 120
   * was made for frames that reached the headset up to 34 ms after the hand-over; with the newest
   * frame passed they are there in ~12). The time the pose is for goes with it (the frame's
   * render time, which Hmd2 reprojects from) */
  if (is_hmd && pred && g_opt_predict)
    pred = now + (u64)g_opt_predict * 1000;
  vr_get_pose(is_hmd, pred, &dev);
  if (is_hmd && pred) {
    lead_note(&g_lead_ask_sum, &g_lead_ask_min, &g_lead_ask_max, &g_lead_ask_n, (s64)pred - (s64)now);
  }
  r->connected = 1;
  r->timestamp = pred ? pred : now;
  if (is_hmd) /* (multi-view: each frame's insets found again by the pose it was made with) */
    multiview_note_pose(r->timestamp);
  r->device_timestamp = now;
  r->led_color = is_pad ? (u32)g_vr.pad_led : 0;
  r->status = recalibrating(is_hmd ? VRT_DEVICE_HMD : VRT_DEVICE_DUALSHOCK4, now) ? 3 : 1;
  r->position_quality = 9;
  r->orientation_quality = 9;
  for (int i = 0; i < 3; i++) {
    r->vel[i] = dev.vel[i];
    r->ang_vel[i] = dev.ang_vel[i];
  }
  if (is_pad) {
    write_pose(&r->pad.device_pose, &dev.pose);
  } else {
    write_pose(&r->hmd.device_pose, &dev.pose);
    write_pose(&r->hmd.head_pose, &dev.head);
    write_pose(&r->hmd.left_eye_pose, &dev.eye[0]);
    write_pose(&r->hmd.right_eye_pose, &dev.eye[1]);
    r->hmd.rear_tracking_status = 1;
    r->hmd.sensor_read_system_timestamp = now;
  }
  LOG_FIRST(n, 6, "vrt: GetResult(%s 0x%x, type %u, pred %lu, orient %u, usage %u) pos %f %f %f",
            is_hmd ? "hmd" : "pad", handle, param->result_type, param->prediction_time,
            param->orientation_type, param->usage_type, dev.pose.pos[0], dev.pose.pos[1],
            dev.pose.pos[2]);
  return 0;
}

/* ---------------------------------------------------------------- libSceCamera */
#define CAM_W 1280
#define CAM_H 800
static u8 g_cam_frame[CAM_W * CAM_H * 2] __attribute__((aligned(256)));
static struct {
  volatile u32 started;
  u32 format[2][4];
  struct cam_exposure_gain eg[2];
  u64 count, frame_time, next_time;
} g_cam;

static s32 h_sceCameraOpen(s32 user, s32 type, s32 index, void *param) {
  LOG("cam: Open(user %d, type %d, index %d) -> 1", user, type, index);
  return 1;
}
static s32 h_sceCameraStart(s32 handle, const void *param) {
  LOG("cam: Start(%d)", handle);
  g_cam.started = 1;
  g_cam.next_time = 0;
  return 0;
}
static s32 h_sceCameraStop(s32 handle) {
  LOG("cam: Stop(%d)", handle);
  g_cam.started = 0;
  return 0;
}
static s32 h_sceCameraSetConfig(s32 handle, const struct cam_config *c) {
  if (c) {
    LOG("cam: SetConfig(%d, type %u, fmt0 %u/%u/%u/%u fmt1 %u, %u fps)", handle, c->type,
        c->ext[0].format.level[0], c->ext[0].format.level[1], c->ext[0].format.level[2],
        c->ext[0].format.level[3], c->ext[1].format.level[0], c->ext[0].framerate);
    for (int d = 0; d < 2; d++)
      for (int l = 0; l < 4; l++)
        g_cam.format[d][l] = c->ext[d].format.level[l];
  }
  return 0;
}
static s32 h_sceCameraSetVideoSync(s32 handle, const void *p) { return 0; }
static s32 h_sceCameraSetAutoWhiteBalance(s32 handle, s32 ch, u32 en, void *opt) { return 0; }
static s32 h_sceCameraSetExposureGain(s32 handle, s32 channel, const struct cam_exposure_gain *eg,
                                      void *opt) {
  if (eg)
    for (int d = 0; d < 2; d++)
      if (channel & (1 << d))
        g_cam.eg[d] = *eg;
  return 0;
}
static s32 h_sceCameraGetCalibrationData(const u32 *param, struct cam_calibration_data *out) {
  LOG("cam: GetCalibrationData");
  if (!param || !out)
    return CAM_ERR_PARAM;
  memset(out, 0, sizeof *out);
  out->format_type = param[1];
  out->function_type = param[2];
  return 0;
}
static s32 h_sceCameraGetFrameData(s32 handle, struct cam_frame_data *fd) {
  static u32 n;
  if (!fd || fd->size_this > sizeof(struct cam_frame_data) + 64)
    return CAM_ERR_PARAM;
  if (!g_cam.started)
    return CAM_ERR_NOT_START;
  if ((fd->read_mode & 1) || g_cam.count == 0) {
    u64 t = now_us();
    if (g_cam.next_time > t)
      sceKernelUsleep((u32)(g_cam.next_time - t));
    g_cam.frame_time = now_us();
    g_cam.next_time =
        (g_cam.next_time > g_cam.frame_time ? g_cam.next_time : g_cam.frame_time) + 16667;
    g_cam.count++;
  }
  struct cam_frame_data f;
  memset(&f, 0, sizeof f);
  for (int d = 0; d < 2; d++) {
    for (int l = 0; l < 4; l++) {
      u32 w = CAM_W >> l, h = CAM_H >> l;
      f.position[d][l].xsize = w;
      f.position[d][l].ysize = h;
      f.frame[d][l] = g_cam_frame;
      f.frame_garlic[d][l] = g_cam_frame;
      f.frame_size[d][l] = w * h * 2;
      f.meta.format[d][l] = g_cam.format[d][l];
    }
    f.status[d] = 1;
    f.meta.frame[d] = g_cam.count - 1;
    f.meta.timestamp[d] = g_cam.frame_time;
    f.meta.device_timestamp[d] = (u32)g_cam.frame_time;
    f.meta.exposure_gain[d] = g_cam.eg[d];
  }
  f.meta.acc_y = 1.0f;
  f.meta.vcounter = g_cam.count - 1;
  size_t hdr = 8, size = fd->size_this < sizeof f ? fd->size_this : sizeof f;
  if (size > hdr)
    memcpy((u8 *)fd + hdr, (u8 *)&f + hdr, size - hdr);
  LOG_FIRST(n, 2, "cam: GetFrameData(size %u, mode %u) frame %lu", fd->size_this, fd->read_mode,
            g_cam.count - 1);
  return 0;
}

/* ---------------------------------------------------------------- sysmodule / videoout */
static s32 h_sceSysmoduleLoadModule(u16 id) {
  s32 rc = CALL_ORIG(s32(*)(u16), sceSysmoduleLoadModule, id);
  LOG("sysmodule: LoadModule(0x%x) -> 0x%x", id, rc);
  /* Loading a module re-binds the game's imports from it, over our hooks: have the
   * injector re-point them before the game can call the real functions. */
  __atomic_store_n(&g_shared.repatch, 1, __ATOMIC_RELEASE);
  for (int i = 0; i < 400 && __atomic_load_n(&g_shared.repatch, __ATOMIC_ACQUIRE); i++)
    sceKernelUsleep(5000);
  LOG("sysmodule: re-patch %s", g_shared.repatch ? "timed out" : "done");
  return rc;
}

static s32 h_sceSysmoduleIsLoaded(u16 id) {
  s32 rc = CALL_ORIG(s32(*)(u16), sceSysmoduleIsLoaded, id);
  static u32 n;
  LOG_FIRST(n, 20, "sysmodule: IsLoaded(0x%x) -> 0x%x", id, rc);
  return rc;
}

#define BUS_MAIN 0

static s32 h_sceVideoOutOpen(s32 user, s32 bus, s32 index, const void *param) {
  s32 rc = CALL_ORIG(s32(*)(s32, s32, s32, const void *), sceVideoOutOpen, user, bus, index, param);
  LOG("videoout: Open(user %d, bus %d, index %d, %p) -> 0x%x", user, bus, index, param, rc);
  if (bus == BUS_MAIN && rc > 0 && g_main_port < 0)
    g_main_port = rc;
  return rc;
}

/* Summer Lesson opens the main port once for its display information and closes it again
 * before its renderer opens its own: the renderer's is the main port here. */
static s32 h_sceVideoOutClose(s32 h) {
  s32 rc = CALL_ORIG(s32(*)(s32), sceVideoOutClose, h);
  LOG("videoout: Close(0x%x) -> 0x%x", h, rc);
  if (h == g_main_port)
    g_main_port = -1;
  return rc;
}

s32 hooks_register_buffers(s32 port, s32 start, void *const *addrs, s32 n, const void *attr) {
  return CALL_ORIG(s32(*)(s32, s32, void *const *, s32, const void *), sceVideoOutRegisterBuffers,
                   port, start, addrs, n, attr);
}

/* ---------------------------------------------------------------- audio */
/* The game's audio ports, and where the system sends them. With PS VR the wearer heard the main
 * port and the TV the AUX port (type 127, a mix for whoever watches), and the BGM port (type 1)
 * went to both. PS VR2 keeps the TV quiet, so the AUX port plays silence and the BGM port opens
 * as a main port, which the system sends to the headset's headphones only. A port's type is in
 * bits 16-23 of its handle. */
#define AUDIO_MAIN 0
#define AUDIO_BGM 1
#define AUDIO_AUX 127
static struct {
  s32 handle, type;
} g_audio[8];
static volatile u32 g_naudio, g_aux_silenced;
/* the longest buffer a port takes: 2048 samples of 8 float channels */
static const u8 g_silence[2048 * 8 * 4];

static int is_aux(s32 h) { return h >= 0 && ((h >> 16) & 0xff) == AUDIO_AUX; }

static s32 h_sceAudioOutOpen(s32 user, s32 type, s32 index, u32 len, u32 freq, u32 param) {
  s32 h = -1;
  if (type == AUDIO_BGM)
    h = CALL_ORIG(s32(*)(s32, s32, s32, u32, u32, u32), sceAudioOutOpen, user, AUDIO_MAIN, index,
                  len, freq, param);
  int moved = h >= 0;
  if (h < 0)
    h = CALL_ORIG(s32(*)(s32, s32, s32, u32, u32, u32), sceAudioOutOpen, user, type, index, len,
                  freq, param);
  LOG("audio: Open(user 0x%x, type %d, len %u, %u Hz, format 0x%x) -> 0x%x%s", user, type, len,
      freq, param, h,
      moved ? " (as a main port: headphones only)"
            : type == AUDIO_AUX && h >= 0 ? " (TV mix: played as silence)" : "");
  u32 n = g_naudio;
  if (h >= 0 && n < 8) {
    g_audio[n].handle = h, g_audio[n].type = type;
    g_naudio = n + 1;
  }
  return h;
}

static s32 h_sceAudioOutOutput(s32 h, const void *buf) {
  if (buf && is_aux(h)) {
    buf = g_silence;
    __atomic_fetch_add(&g_aux_silenced, 1, __ATOMIC_RELAXED);
  }
  return CALL_ORIG(s32(*)(s32, const void *), sceAudioOutOutput, h, buf);
}

/* SceAudioOutOutputParam */
struct audio_out_param {
  s32 handle;
  const void *ptr;
};

static s32 h_sceAudioOutOutputs(const struct audio_out_param *p, u32 num) {
  struct audio_out_param q[25];
  if (p && num <= 25) {
    for (u32 i = 0; i < num; i++) {
      q[i] = p[i];
      if (q[i].ptr && is_aux(q[i].handle)) {
        q[i].ptr = g_silence;
        __atomic_fetch_add(&g_aux_silenced, 1, __ATOMIC_RELAXED);
      }
    }
    p = q;
  }
  return CALL_ORIG(s32(*)(const struct audio_out_param *, u32), sceAudioOutOutputs, p, num);
}

void hooks_log_audio(void) {
  for (u32 i = 0; i < g_naudio; i++) {
    u8 st[32];
    memset(st, 0, sizeof st);
    s32 rc = sceAudioOutGetPortState ? sceAudioOutGetPortState(g_audio[i].handle, st) : -1;
    /* SceAudioOutPortState: output (u16, connected devices), channels, volume, reroute count */
    LOG("audio: port type %d (0x%x): state 0x%x output 0x%x channels %u volume %d reroutes %u",
        g_audio[i].type, g_audio[i].handle, rc, *(u16 *)st, st[2], *(s16 *)(st + 4),
        *(u16 *)(st + 6));
  }
  LOG("audio: %u TV mix buffers played as silence", g_aux_silenced);
}

static s32 h_sceVideoOutSubmitFlip(s32 vo, s32 index, u32 mode, s64 arg) {
  s32 rc = CALL_ORIG(s32(*)(s32, s32, u32, s64), sceVideoOutSubmitFlip, vo, index, mode, arg);
  static u32 n;
  LOG_FIRST(n, 12, "videoout: game flip(port 0x%x, buffer %d, mode %u, arg %ld) -> 0x%x", vo, index,
            mode, arg, rc);
  return rc;
}

static s32 h_sceVideoOutRegisterBuffers(s32 h, s32 start, void *const *addrs, s32 n,
                                        const u32 *attr) {
  s32 rc = CALL_ORIG(s32(*)(s32, s32, void *const *, s32, const u32 *), sceVideoOutRegisterBuffers,
                     h, start, addrs, n, attr);
  LOG("videoout: RegisterBuffers(0x%x, start %d, n %d, %p) -> 0x%x", h, start, n,
      n > 0 ? addrs[0] : 0, rc);
  if (attr)
    LOG("  attr %08x %08x %08x %08x %08x %08x %08x %08x", attr[0], attr[1], attr[2], attr[3],
        attr[4], attr[5], attr[6], attr[7]);
  for (s32 i = 0; i < n && i < 8; i++)
    if (start + i >= 0 && start + i < 16)
      g_vr.vo_buffers[start + i] = addrs[i];
  if (rc >= 0 && h == g_main_port && start == 0)
    mirror_note_port(h, attr);
  return rc;
}

static s32 h_sceVideoOutSetBufferAttribute(u32 *attr, u32 fmt, u32 tiling, u32 aspect, u32 w,
                                           u32 h, u32 pitch) {
  LOG("videoout: SetBufferAttribute(fmt 0x%x, tiling %u, aspect %u, %ux%u pitch %u)", fmt, tiling,
      aspect, w, h, pitch);
  return CALL_ORIG(s32(*)(u32 *, u32, u32, u32, u32, u32, u32), sceVideoOutSetBufferAttribute, attr,
                   fmt, tiling, aspect, w, h, pitch);
}

static s32 h_sceVideoOutGetDeviceCapabilityInfo_(s32 h, u64 *info) {
  s32 rc = CALL_ORIG(s32(*)(s32, u64 *), sceVideoOutGetDeviceCapabilityInfo_, h, info);
  static u32 n;
  if (rc != 0) /* not filled: nothing added, the failure passed on */
    return rc;
  if (info) {
    LOG_FIRST(n, 4, "videoout: GetDeviceCapabilityInfo(0x%x) -> 0x%x cap 0x%lx, adding VR view", h,
              rc, info[0]);
    info[0] |= 0x20;
  }
  return 0;
}

/* The game asks for the PS VR output mode (119.88 Hz, refresh code 0x0d) on its main port.
 * Passed through, the system sees a PS VR mode with no PS VR attached and suspends the game
 * ("can't continue ... PlayStation VR isn't connected"), so it is only pretended. */
static s32 h_sceVideoOutConfigureOutputMode_(s32 h, u32 a1, const void *mode, const void *opt,
                                             u32 size_mode, u32 size_opt) { /* six arguments */
  const u8 *m = mode;
  if (m && (m[8] == 0x0d || m[8] == 0x23)) { /* 119.88 Hz, or PS VR's 90 Hz (Summer Lesson's mode 2) */
    LOG("videoout: ConfigureOutputMode(0x%x, %u) PS VR mode, refresh code 0x%x (%s): pretended", h,
        a1, m[8], m[8] == 0x0d ? "119.88 Hz" : "90 Hz");
    g_vr.vr_mode_vo = h;
    g_vr.vr_mode_refresh = m[8];
    return 0;
  }
  s32 rc = CALL_ORIG(s32(*)(s32, u32, const void *, const void *, u32, u32),
                     sceVideoOutConfigureOutputMode_, h, a1, mode, opt, size_mode, size_opt);
  if (m)
    LOG("videoout: ConfigureOutputMode(0x%x, %u) mode %02x %02x %02x %02x %02x %02x %02x %02x "
        "%02x %02x %02x %02x -> 0x%x",
        h, a1, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], rc);
  return rc;
}

static s32 h_sceVideoOutModeSetAny_(void *mode, u32 size) {
  s32 rc = CALL_ORIG(s32(*)(void *, u32), sceVideoOutModeSetAny_, mode, size);
  LOG("videoout: ModeSetAny(%p, %u) -> 0x%x", mode, size, rc);
  return rc;
}

/* The game sizes everything from the TV's resolution (SceVideoOutResolutionStatus: full width and
 * height, pane width and height, refresh rate): its main port's buffers and its eye textures, which
 * are the screen times r.ScreenPercentage side by side. On a 4K output that is four times 1080p's
 * memory (eyes 2496x2808 at its own 130%), and the game runs out of GPU memory within seconds even
 * at its own settings (a 108 MB grow refused, 2026-11-03). Over 1920x1080 it is told 1920x1080, as
 * it was on a 1080p TV: the eye textures, resolution.txt and the memory are then the same whatever
 * the TV is set to, and the system scales the 1080p main port up. */
static s32 h_sceVideoOutGetResolutionStatus(s32 h, u32 *st) {
  s32 rc = CALL_ORIG(s32(*)(s32, u32 *), sceVideoOutGetResolutionStatus, h, st);
  static u32 n;
  u32 real_w = st ? st[0] : 0, real_h = st ? st[1] : 0;
  if (st && rc == 0 && (st[0] > 1920 || st[1] > 1080)) {
    st[0] = st[2] = 1920;
    st[1] = st[3] = 1080;
  }
  if (st && rc == 0 && h == g_vr.vr_mode_vo && g_vr.vr_mode_vo)
    *(u64 *)(st + 4) = g_vr.vr_mode_refresh; /* refreshRate: the PS VR mode the game asked for */
  if (st)
    LOG_FIRST(n, 6, "videoout: GetResolutionStatus(0x%x) -> 0x%x %ux%u (told %ux%u) refresh 0x%x", h,
              rc, real_w, real_h, st[0], st[1], st[4]);
  return rc;
}

/* ---------------------------------------------------------------- the video decoder's GPU queues
 * The tutorial screens play with AvPlayer's Software2 decoder (Videodec2), which maps a GPU compute
 * queue (sceGnmMapComputeQueue, through libSceVdecCore) when a video starts. With the PS5
 * libraries in the game, every new gen1 compute queue is refused (0x80d170ff on all pipes; queues
 * mapped before keep working), so the decoder never started and the screens stayed blue (issue
 * #4). Two of its queues are made before the PS5 libraries load, on pipe 2 (Hmd2's queue is pipe 1
 * queue 0), and lent to AvPlayer in place of the ones it would make. */
#define VQ_N 2
static struct {
  u64 handle;
  volatile u32 used;
} g_vq[VQ_N];
static int vq_alloc(const void *cfg, const void *mem, u64 *out) {
  for (u32 i = 0; i < VQ_N && out; i++)
    if (g_vq[i].handle && !__atomic_exchange_n(&g_vq[i].used, 1, __ATOMIC_ACQ_REL)) {
      *out = g_vq[i].handle;
      return 0;
    }
  int rc = sceVideodec2AllocateComputeQueue(cfg, mem, out);
  LOG("video: no free decoder queue; AvPlayer's own -> 0x%x", rc);
  return rc;
}

static int vq_release(u64 q) {
  for (u32 i = 0; i < VQ_N; i++)
    if (g_vq[i].handle && g_vq[i].handle == q) {
      __atomic_store_n(&g_vq[i].used, 0, __ATOMIC_RELEASE);
      return 0;
    }
  return sceVideodec2ReleaseComputeQueue(q);
}

void hooks_video_queues(void) {
  agent_call(CMD_RESOLVE, 0, 0, 0, 0, 3000, 0, 0); /* the game loaded Videodec2 after the agent */
  if (!sceVideodec2QueryComputeMemoryInfo || !sceVideodec2AllocateComputeQueue ||
      !sceVideodec2ReleaseComputeQueue) {
    LOG("video: libSceVideodec2 isn't loaded: no decoder queues");
    return;
  }
  /* SceVideodec2ComputeMemoryInfo: size of the struct, memory size, memory */
  u64 mi[3] = {0x18, 0, 0};
  int rc = sceVideodec2QueryComputeMemoryInfo(mi);
  u32 made = 0;
  for (u32 i = 0; rc == 0 && i < VQ_N; i++) {
    void *mem = vr2_dalloc_pref((mi[1] + 0x1fffff) & ~(u64)0x1fffff, 0x200000, "a video decoder queue");
    /* SceVideodec2ComputeConfigInfo: size, u16 pipe, u16 queue, checkMemoryType 0 */
    u64 cfg[2] = {0x10, 2 | ((u64)i << 16)}, mem_info[3] = {0x18, mi[1], (u64)mem};
    rc = mem ? sceVideodec2AllocateComputeQueue(cfg, mem_info, &g_vq[i].handle) : -1;
    made += rc == 0;
  }
  LOG("video: %u decoder queues ready on pipe 2 (%lu KB each, 0x%x)", made,
      (unsigned long)(mi[1] >> 10), rc);
}

static void video_queues_lend(void) {
  static volatile u32 done;
  if (!g_vq[0].handle || !once(&done))
    return;
  int m = vr2_find_module("libSceAvPlayer.sprx");
  int a = vr2_override(m, "eD+X2SmxUt4", (void *)vq_alloc);     /* AllocateComputeQueue */
  int r = vr2_override(m, "UvtA3FAiF4Y", (void *)vq_release);   /* ReleaseComputeQueue */
  LOG("video: AvPlayer (%d) gets the decoder queues (%d %d)", m, a, r);
}

/* ---------------------------------------------------------------- libSceAvPlayer (logged)
 * The game plays its tutorial videos with AvPlayer. Passed through; what AvPlayer reports
 * (events, warnings, whether frames come) goes to the log. SceAvPlayerInitData: event
 * replacement (object, callback) at +0x50, 0x78 bytes in all. */
struct avp_watch {
  void *handle, *game_obj;
  void (*game_cb)(void *, s32, s32, void *);
  u32 frames, empty;
  u8 init[0x78];
};
static struct avp_watch g_avp[4];
static u32 g_avp_next;

static struct avp_watch *avp_find(void *h) {
  for (u32 i = 0; i < 4; i++)
    if (h && g_avp[i].handle == h)
      return &g_avp[i];
  return 0;
}

static void avp_event(void *p, s32 event, s32 src, void *data) {
  struct avp_watch *w = p;
  static u32 n;
  if (event == 0x20 && data) /* WarningId */
    LOG_FIRST(n, 60, "avplayer: %p warning 0x%x (stream %d)", w->handle, *(s32 *)data, src);
  else
    LOG_FIRST(n, 60, "avplayer: %p event 0x%x (stream %d)", w->handle, event, src);
  if (w->game_cb)
    w->game_cb(w->game_obj, event, src, data);
}

static void *h_sceAvPlayerInit(const u8 *init) {
  video_queues_lend();
  struct avp_watch *w = &g_avp[__atomic_fetch_add(&g_avp_next, 1, __ATOMIC_RELAXED) % 4];
  const u8 *use = init;
  w->handle = 0, w->frames = w->empty = 0;
  if (init) {
    memcpy(w->init, init, sizeof w->init);
    w->game_obj = *(void *const *)(init + 0x50);
    w->game_cb = *(void *const *)(init + 0x58);
    *(void **)(w->init + 0x50) = w;
    *(void **)(w->init + 0x58) = (void *)avp_event;
    use = w->init;
  }
  void *h = CALL_ORIG(void *(*)(const u8 *), sceAvPlayerInit, use);
  w->handle = h;
  if (init)
    LOG("avplayer: Init(%d frame buffers, auto start %u, own file reader %s) -> %p",
        *(const s32 *)(init + 0x68), init[0x6c], *(void *const *)(init + 0x30) ? "yes" : "no", h);
  return h;
}

static s32 h_sceAvPlayerPostInit(void *h, const void *data) {
  s32 rc = CALL_ORIG(s32(*)(void *, const void *), sceAvPlayerPostInit, h, data);
  LOG("avplayer: %p PostInit -> 0x%x", h, rc);
  return rc;
}

static s32 h_sceAvPlayerAddSource(void *h, const char *path) {
  s32 rc = CALL_ORIG(s32(*)(void *, const char *), sceAvPlayerAddSource, h, path);
  LOG("avplayer: %p AddSource(%s) -> 0x%x", h, path ? path : "(null)", rc);
  return rc;
}

static s32 h_sceAvPlayerStart(void *h) {
  s32 rc = CALL_ORIG(s32(*)(void *), sceAvPlayerStart, h);
  LOG("avplayer: %p Start -> 0x%x", h, rc);
  return rc;
}

static s32 h_sceAvPlayerStop(void *h) {
  s32 rc = CALL_ORIG(s32(*)(void *), sceAvPlayerStop, h);
  struct avp_watch *w = avp_find(h);
  LOG("avplayer: %p Stop -> 0x%x (%u video frames, %u empty polls)", h, rc, w ? w->frames : 0,
      w ? w->empty : 0);
  return rc;
}

static s32 h_sceAvPlayerClose(void *h) {
  struct avp_watch *w = avp_find(h);
  LOG("avplayer: %p Close (%u video frames, %u empty polls)", h, w ? w->frames : 0,
      w ? w->empty : 0);
  s32 rc = CALL_ORIG(s32(*)(void *), sceAvPlayerClose, h);
  if (w)
    w->handle = 0;
  return rc;
}

/* SceAvPlayerFrameInfoEx: data pointer, timestamp at +0x10, video details (width, height) +0x18 */
static u8 h_sceAvPlayerGetVideoDataEx(void *h, u8 *info) {
  u8 ok = CALL_ORIG(u8(*)(void *, u8 *), sceAvPlayerGetVideoDataEx, h, info);
  struct avp_watch *w = avp_find(h);
  if (!w)
    return ok;
  if (ok && w->frames++ == 0 && info)
    LOG("avplayer: %p first video frame %ux%u at %p (after %u empty polls)", h,
        *(u32 *)(info + 0x18), *(u32 *)(info + 0x1c), *(void **)info, w->empty);
  else if (!ok && ++w->empty % 600 == 0 && !w->frames)
    LOG("avplayer: %p no video frame yet (%u polls)", h, w->empty);
  return ok;
}

/* Generic pass-through with logging (integer/pointer arguments only). */
#define PASS(name, force_ok)                                                                     \
  static s64 h_##name(s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {                               \
    s64 rc = ((s64(*)(s64, s64, s64, s64, s64, s64))orig_##name)(a, b, c, d, e, f);              \
    static u32 n;                                                                                \
    LOG_FIRST(n, 8, #name "(0x%lx, 0x%lx, 0x%lx, 0x%lx) -> 0x%x", a, b, c, d, (u32)rc);         \
    return ((s32)rc < 0 && (force_ok)) ? 0 : rc;                                                 \
  }
PASS(sceAudioInOpen, 0)
/* the game's compute queues (pipe, queue, ...): hand tracking's submits on pipe 5 queue 0 were
 * refused (0x8a6d0103) in play test 3 */
PASS(sceGnmMapComputeQueue, 0)
PASS(sceGnmMapComputeQueueWithPriority, 0)
/* the PS VR reprojection's work buffers, which the game allocates and hands to
 * ReprojectionInitialize (answered without them): the real library's sizes, logged */
PASS(sceHmdReprojectionQueryOnionBuffSize, 0)
PASS(sceHmdReprojectionQueryOnionBuffAlign, 0)
PASS(sceHmdReprojectionQueryGarlicBuffSize, 0)
PASS(sceHmdReprojectionQueryGarlicBuffAlign, 0)
PASS(sceAudioInHqOpen, 0)
PASS(sceVideoOutAddFlipEvent, 0)
PASS(sceSocialScreenInitialize, 1)
PASS(sceSocialScreenSetMode, 1)
PASS(sceSocialScreenInitializeSeparateModeParameter, 1)
PASS(sceSocialScreenConfigureSeparateMode, 1)
PASS(sceSocialScreenOpenSeparateMode, 0)
PASS(sceSocialScreenCloseSeparateMode, 1)

/* ---------------------------------------------------------------- direct memory (observed)
 * Run 6: with the libraries loaded, every direct-memory allocation for PS VR2 failed (32 MB,
 * 1 MB, 96 MB): the game takes its budget. Logged here, unchanged: each allocation, the
 * available size it asks for, each release, and the totals. */
/* what the game is told less is free when it sizes its pools at its start (see
 * h_sceKernelAvailableDirectMemorySize); its CPU pool retries exactly this much all session */
#define DMEM_KEEP (224ull << 20)
static u64 caller_site(u64 *fp);
static volatile u64 g_dmem_taken;
static volatile u32 g_dmem_n;
static s32 h_sceKernelAllocateDirectMemory(s64 start, s64 end, u64 len, u64 align, s32 type,
                                           s64 *phys) {
  /* Once PS VR2 runs, the CPU pool's retry of its 224 MB (type 0) is refused outright: it failed
   * every time by 2 MB only (222 MB free with the palm tracker off), and granted it would take the
   * room the GPU pool's scene-change peaks need (see h_sceKernelAvailableDirectMemorySize); the
   * game copes with it refused, as all along */
  s32 rc = g_reserve.va && type == 0 && len == DMEM_KEEP
               ? (s32)0x80020023
               : CALL_ORIG(s32(*)(s64, s64, u64, u64, s32, s64 *), sceKernelAllocateDirectMemory,
                           start, end, len, align, type, phys);
  /* In inset mode, the first of those retries is granted from the extra segment instead: refused,
   * the pool grows into the game's flexible memory (448 MB, shared with libc, the stacks and the
   * libraries) and two inset sessions ended there after 19 and 25 minutes, the game out of memory
   * (abort through FGenericPlatformMemory::OnOutOfMemory: logs/manual-1440, logs/inset-day2).
   * Granted, the pool has its PS4 size back; asked again it is told 1 MB free and grows no more.
   * Inset mode's GPU blocks are 52 MB (156 MB at the most seen), with 292 MB left for them; the
   * stock game's 74 MB blocks need the room (5 of them at 2048), so it stays refused there. */
  /* (until it is given: one refused there, the GPU blocks of a scene change in the way, is asked
   * for again at the pool's next retry; the pool retries from one thread) */
  if (rc == (s32)0x80020023 && type == 0 && len == DMEM_KEEP && g_multiview_rows &&
      g_reserve.spare_hi > g_reserve.spare_lo && !g_pool_granted) {
    rc = CALL_ORIG(s32(*)(s64, s64, u64, u64, s32, s64 *), sceKernelAllocateDirectMemory,
                   g_reserve.spare_lo, g_reserve.spare_hi, len, align, type, phys);
    static u32 said;
    if (rc == 0 || said++ < 3) {
      size_t flex = 0;
      sceKernelAvailableFlexibleMemorySize(&flex);
      LOG("dmem: inset mode: the game's CPU pool given its 224 MB from the extra segment -> 0x%x at "
          "0x%lx (flexible memory free %lu MB)", rc, rc == 0 && phys ? *phys : 0, (u64)flex >> 20);
    }
    if (rc == 0)
      g_pool_granted = 1;
    else
      rc = (s32)0x80020023;
  }
  /* Step 3 (after Ghost-VR's h_sceKernelAllocateDirectMemory): a block refused for want of memory
   * is asked for again in the container's extra segment below the reserve (256 MB nobody else
   * allocates): to the game, a block like any other. Only once the reserve is in (PS VR2 started) */
  /* Not the CPU pool's retry of exactly what the game was told less (224 MB, type 0): granted (it
   * took 224 of the 256 MB there, 2026-11-04 01:17) it changed nothing the game showed, and at 2000
   * the GPU pool's 74 MB blocks then found no room and the game quit; refused, as all along, the
   * game copes. */
  if (rc == (s32)0x80020023 && g_reserve.spare_hi > g_reserve.spare_lo &&
      !(type == 0 && len == DMEM_KEEP)) {
    s32 rc2 = CALL_ORIG(s32(*)(s64, s64, u64, u64, s32, s64 *), sceKernelAllocateDirectMemory,
                        g_reserve.spare_lo, g_reserve.spare_hi, len, align, type, phys);
    static u32 said;
    if (rc2 == 0 || said++ < 8)
      LOG("dmem: the game's %lu MB (type %d) refused in its range, from the extra segment -> 0x%x at "
          "0x%lx", len >> 20, type, rc2, rc2 == 0 && phys ? *phys : 0);
    if (rc2 == 0)
      rc = 0;
  }
  if (g_reserve.va && type == 3 && len >= (8ull << 20)) {
    /* which of the game's code makes the big GPU blocks once PS VR2 runs (the scene-change peaks):
     * its call site and the one above it in the eboot, and the frame */
    static u32 said;
    if (said++ < 64)
      LOG("dmem: the game's %lu MB GPU block from eboot+0x%lx (caller eboot+0x%lx), frame count %lu "
          "-> 0x%x", len >> 20, (u64)__builtin_return_address(0) - astro_hdr.game_base,
          caller_site((u64 *)__builtin_frame_address(0)), hooks_frame_count(), rc);
  }
  u32 k = __atomic_fetch_add(&g_dmem_n, 1, __ATOMIC_RELAXED);
  int retry = rc != 0 && type == 0 && len == DMEM_KEEP; /* the game's known CPU-pool retry */
  if (rc == 0)
    g_dmem_taken += len;
  else if (!retry)
    g_rec_dmem_refused++;
  if (retry && __atomic_fetch_add(&g_pool_retries, 1, __ATOMIC_RELAXED) >= 3)
    return rc; /* logged three times, then counted (health_report): ~5 a second before */
  if (k < 60 || len >= (16u << 20))
    LOG("dmem: game Allocate(0x%lx-0x%lx, %lu MB 0x%lx, align 0x%lx, type %d) -> 0x%x at 0x%lx; "
        "the game holds %lu MB", start, end, len >> 20, len, align, type, rc,
        rc == 0 && phys ? *phys : 0, g_dmem_taken >> 20);
  return rc;
}
/* Run 7: the game takes 8, 256, 3624 (its GPU heap) and 200 MB, then sizes its last pool from
 * this call: what is free minus 1 MB (997 MB), leaving PS VR2 1 MB. It is told DMEM_KEEP less is
 * free (its fixed allocations still fit with gigabytes to spare), so its last pool comes out
 * DMEM_KEEP smaller and PS VR2 gets that much (it took 155 MB in run 8: the tracker 32, the
 * reprojection's display buffers 96, the TV mirror 25, small buffers 2). With 256 MB kept the
 * game ran short of its pool and retried a 256 MB allocation 4 times a second (run 8). */
static s32 h_sceKernelAvailableDirectMemorySize(s64 start, s64 end, u64 align, s64 *phys,
                                                u64 *avail) {
  s32 rc = CALL_ORIG(s32(*)(s64, s64, u64, s64 *, u64 *), sceKernelAvailableDirectMemorySize,
                     start, end, align, phys, avail);
  u64 real = avail ? *avail : 0;
  if (rc == 0 && avail && real > 2 * DMEM_KEEP)
    *avail = real - DMEM_KEEP;
  else if (rc == 0 && avail && g_reserve.va && real > (1ull << 20))
    /* Once PS VR2 runs (its buffers in the reserve), what is free in the game's range is the
     * room its GPU pool's peaks need at a scene change (74 MB blocks, released after). Told the
     * truth, the game's CPU pool took it all for good right after the first peak ("free minus
     * 1 MB": 159 MB, manual2, 2026-11-04 01:30), every later peak went to the spare, and after 23
     * minutes at 2048 a block was refused and the game aborted. Told 1 MB it takes nothing (the
     * same log: asked with 1 and 12 MB free, nothing allocated); the GPU pool allocates without
     * asking and still gets the room. */
    *avail = 1ull << 20;
  static u32 n;
  LOG_FIRST(n, 20, "dmem: game AvailableDirectMemorySize(0x%lx-0x%lx, align 0x%lx) -> 0x%x: %lu MB "
            "at 0x%lx (told %lu MB: %lu kept for PS VR2)", start, end, align, rc, real >> 20,
            phys ? *phys : 0, avail ? *avail >> 20 : 0, (real - (avail ? *avail : 0)) >> 20);
  return rc;
}
static s32 h_sceKernelReleaseDirectMemory(s64 start, u64 len) {
  s32 rc = CALL_ORIG(s32(*)(s64, u64), sceKernelReleaseDirectMemory, start, len);
  if (rc == 0)
    g_dmem_taken -= len;
  static u32 n;
  LOG_FIRST(n, 30, "dmem: game Release(0x%lx, %lu KB) -> 0x%x; the game holds %lu MB", start,
            len >> 10, rc, g_dmem_taken >> 20);
  return rc;
}
/* Probe only (2026-11-04, after Ghost-VR's vr2.c reserve_hidden): a PS4 game's direct-memory
 * container on PS5 is said to hold 512 MB more than the game is told, at offsets of their own
 * (0x4000000000 + its size; "SISR DMEM" in the launch log), which nothing allocates. Asked with the
 * call the agent already makes for the game's own range (the largest free piece in a range):
 * nothing is allocated, nothing changes. */
void hooks_probe_hidden_dmem(const char *when) {
  if (!sceKernelAvailableDirectMemorySize)
    return;
  u64 total = (u64)sceKernelGetDirectMemorySize();
  s64 base = 0x4000000000ll + (s64)total, top = base + 0x20000000ll;
  s64 at = 0, at_all = 0;
  u64 avail = 0, avail_all = 0;
  s32 rc = sceKernelAvailableDirectMemorySize(base, top, 0x200000, &at, &avail);
  s32 rc_all = sceKernelAvailableDirectMemorySize(0, top, 0x200000, &at_all, &avail_all);
  LOG("dmem probe %s: %lu MB direct memory; the extra segment 0x%lx-0x%lx: 0x%x, %lu MB free in one "
      "piece at 0x%lx; up to its top: 0x%x, %lu MB free in one piece at 0x%lx", when, total >> 20,
      base, top, rc, avail >> 20, at, rc_all, avail_all >> 20, at_all);
}

void hooks_log_dmem(const char *when) {
  u64 avail = 0;
  s64 at = 0;
  s32 rc = sceKernelAvailableDirectMemorySize
               ? sceKernelAvailableDirectMemorySize(0, (s64)sceKernelGetDirectMemorySize(), 0x10000,
                                                    &at, &avail)
               : -1;
  LOG("dmem %s: %lu MB in all, the game holds %lu MB in %u allocations, %lu MB free in one piece "
      "(0x%x)", when, (u64)sceKernelGetDirectMemorySize() >> 20, g_dmem_taken >> 20, g_dmem_n,
      avail >> 20, rc);
}

/* ---------------------------------------------------------------- where the game blocks
 * (after Ghost-VR's waitprof.c) Its condition waits, timed sleeps and event-flag waits: per call
 * site in a 10 s census (above), and every single wait
 * over 30 ms once PS VR2 runs logged on its own with its site and its caller (the hitches are
 * single frames of 100-180 ms, which a 10 s sum hides), 12 at most per 10 s. Passed through. A
 * debug tool: agent.waits=1 in cvars.txt switches it on. */
static volatile u32 g_long_n;
static volatile u64 g_long_reset;
static u64 caller_site(u64 *fp);
static void blocked(const char *what, u64 site, u64 *fp, u64 us) {
  census_add(&g_blocks, site, us);
  if (us < 30000 || !g_vr.vr2_started)
    return;
  u64 caller = caller_site(fp);
  u64 t = now_us();
  if (t - g_long_reset > 10000000)
    g_long_reset = t, g_long_n = 0;
  if (__atomic_fetch_add(&g_long_n, 1, __ATOMIC_RELAXED) < 12)
    LOG("block: %s for %lu ms from eboot+0x%lx (caller eboot+0x%lx)", what, us / 1000, site,
        caller);
}
/* the caller of the game function that waited (its frame's return address), for a long wait only:
 * the hook's frame pointer is passed in; the game's saved one is followed only if it is 8-byte
 * aligned, just above on the stack and, asked of the system first (sceKernelVirtualQuery, a
 * query), in readable memory: a function without a frame pointer leaves anything there */
static int readable(const void *p, u64 n) {
  u8 info[128];
  memset(info, 0, sizeof info);
  /* SceKernelVirtualQueryInfo: start, end, offset, protection, type, flags, name[32]: 0x48 */
  if (!sceKernelVirtualQuery || sceKernelVirtualQuery(p, 0, info, 0x48) != 0)
    return 0;
  u64 start = *(u64 *)info, end = *(u64 *)(info + 8);
  s32 prot = *(s32 *)(info + 24);
  return start <= (u64)p && (u64)p + n <= end && (prot & 1);
}
static u64 caller_site(u64 *fp) {
  if (!fp || !readable(fp, 16))
    return 0;
  u64 *up = (u64 *)fp[0];
  if (!up || ((u64)up & 7) || up <= fp || (u64)up - (u64)fp > 0x100000 || !readable(up, 16))
    return 0;
  return up[1] - astro_hdr.game_base;
}
#define WAITHOOK(name, label)                                                                    \
  static s64 h_##name(s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {                               \
    rtime_note(RT_WAIT, 0, 0); /* (agent.rtime=1: the thread's CPU time; nothing when off) */  \
    if (!g_opt_waits) /* a debug tool (agent.waits=1): passed straight through */                \
      return ((s64(*)(s64, s64, s64, s64, s64, s64))orig_##name)(a, b, c, d, e, f);              \
    u64 site = (u64)__builtin_return_address(0) - astro_hdr.game_base;                           \
    u64 t0 = now_us();                                                                           \
    s64 rc = ((s64(*)(s64, s64, s64, s64, s64, s64))orig_##name)(a, b, c, d, e, f);              \
    blocked(label, site, (u64 *)__builtin_frame_address(0), now_us() - t0);                      \
    return rc;                                                                                   \
  }
WAITHOOK(nanosleep, "nanosleep")
WAITHOOK(pthread_cond_wait, "pthread_cond_wait")
WAITHOOK(pthread_cond_timedwait, "pthread_cond_timedwait")
WAITHOOK(sceKernelWaitEventFlag, "sceKernelWaitEventFlag")

/* ---------------------------------------------------------------- PSN leaderboards (logged)
 * PSN is not reachable from this console. Each call passed through, its first ones logged with
 * their result, and any that takes over 5 ms logged with how long (a slow failure is a hitch). */
#define NPHOOK(name)                                                                             \
  static s64 h_##name(s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {                               \
    static u32 n;                                                                                \
    u64 t0 = now_us();                                                                           \
    s64 rc = ((s64(*)(s64, s64, s64, s64, s64, s64))orig_##name)(a, b, c, d, e, f);              \
    u64 us = now_us() - t0;                                                                      \
    if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 6 || us > 5000)                            \
      LOG("np: " #name "(0x%lx, 0x%lx) -> 0x%x in %lu us", a, b, (u32)rc, us);                    \
    return rc;                                                                                   \
  }
NPHOOK(sceNpScoreCreateNpTitleCtx)
NPHOOK(sceNpScoreDeleteNpTitleCtx)
NPHOOK(sceNpScoreCreateRequest)
NPHOOK(sceNpScoreDeleteRequest)
NPHOOK(sceNpScorePollAsync)
NPHOOK(sceNpScoreRecordScoreAsync)

/* ---------------------------------------------------------------- modules the game loads */
static s32 h_sceKernelLoadStartModule(const char *path, u64 argc, const void *argv, u32 flags,
                                      void *opt, s32 *res) {
  u64 t0 = now_us();
  s32 h = CALL_ORIG(s32(*)(const char *, u64, const void *, u32, void *, s32 *),
                    sceKernelLoadStartModule, path, argc, argv, flags, opt, res);
  LOG("kernel: the game loads %s -> 0x%x in %lu ms", path ? path : "?", h, (now_us() - t0) / 1000);
  if (h >= 0) { /* its imports may re-bind ours: the injector re-points them, as for sysmodule */
    __atomic_store_n(&g_shared.repatch, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 400 && __atomic_load_n(&g_shared.repatch, __ATOMIC_ACQUIRE); i++)
      sceKernelUsleep(5000);
  }
  return h;
}

/* ---------------------------------------------------------------- why the game ends
 * (after Ghost-VR's exitlog.c) The game's calls to abort, exit and _Exit (its main program imports
 * these three) are logged with their callers (module and offset, from the frame-pointer chain)
 * and the last of what it printed through puts, before they go on; then a pause, so the payload
 * (which reads the log every 20 ms) has the lines before the process is gone. printf and fprintf
 * are not hooked: their arguments vary, and the game imports no vprintf to pass them on with. */
#define OUT_TAIL 4096
static char g_out_tail[OUT_TAIL];
static volatile u64 g_out_w, g_out_logged;

static void log_text(const char *tag, const char *p, u64 n) {
  char line[200];
  while (n) {
    u64 k = 0;
    while (k < n && k < sizeof line - 1 && p[k] != '\n')
      line[k] = p[k], k++;
    line[k] = 0;
    if (k)
      LOG("%s%s", tag, line);
    p += k, n -= k;
    if (n && *p == '\n')
      p++, n--;
  }
}

static s32 h_puts(const char *s) {
  if (s) {
    u64 n = strlen(s), w = __atomic_fetch_add(&g_out_w, n + 1, __ATOMIC_RELAXED);
    for (u64 i = 0; i < n; i++)
      g_out_tail[(w + i) % OUT_TAIL] = s[i];
    g_out_tail[(w + n) % OUT_TAIL] = '\n';
    if (__atomic_fetch_add(&g_out_logged, n, __ATOMIC_RELAXED) < 16384)
      log_text("out: ", s, n);
  }
  return CALL_ORIG(s32(*)(const char *), puts, s);
}

static void last_words(const char *what, s64 code, u64 from) {
  const char *mod;
  u64 base = vr2_module_of(from, &mod);
  LOG("exit: %s(%ld) called from %s+0x%lx", what, (long)code, mod, (unsigned long)(from - base));
  /* the callers: each frame holds the caller's frame pointer, then the return address (frames
   * looked at only up the stack, 8-byte aligned, within 256 KB of this one, and in readable memory:
   * a function without a frame pointer leaves anything there) */
  u64 *fp = (u64 *)__builtin_frame_address(0), *top = fp;
  for (int i = 0; i < 12 && fp && !((u64)fp & 7) && (u64)fp - (u64)top < 0x40000 && readable(fp, 16);
       i++) {
    u64 ret = fp[1];
    if (ret < 0x10000)
      break;
    base = vr2_module_of(ret, &mod);
    LOG("exit:   #%d %s+0x%lx", i, mod, (unsigned long)(ret - base));
    if ((u64 *)fp[0] <= fp)
      break;
    fp = (u64 *)fp[0];
  }
  u64 w = g_out_w, n = w < OUT_TAIL ? w : OUT_TAIL;
  static char tail[OUT_TAIL];
  for (u64 i = 0; i < n; i++)
    tail[i] = g_out_tail[(w - n + i) % OUT_TAIL];
  if (n)
    LOG("exit: the game's last %lu bytes through puts:", (unsigned long)n);
  log_text("exit| ", tail, n);
  sceKernelUsleep(150000);
}

static void h_abort(void) {
  last_words("abort", 0, (u64)__builtin_return_address(0));
  CALL_ORIG(void (*)(void), abort);
}
static void h_exit(s32 code) {
  last_words("exit", code, (u64)__builtin_return_address(0));
  CALL_ORIG(void (*)(s32), exit, code);
}
static void h__Exit(s32 code) {
  last_words("_Exit", code, (u64)__builtin_return_address(0));
  CALL_ORIG(void (*)(s32), _Exit, code);
}

/* ---------------------------------------------------------------- table */
#define H(name) {HNID_##name, (void *)h_##name, &orig_##name, 0},
struct agent_hook g_hooks[] = {
#include "hooks.inc"
    {0, 0, 0, 0},
};
#undef H

/* Called by the injector on a hijacked game thread right after the hooks go live. */
u64 agent_boot(u64 multiview) {
  /* the headset's own eye distance, as measured in an earlier session (vr2.c), for the game's
   * eye offsets: the game asks for them once, before PS VR2 is up */
  extern struct agent_header astro_hdr;
  if (astro_hdr.ipd_um >= 50000 && astro_hdr.ipd_um <= 80000)
    g_vr.ipd = (float)astro_hdr.ipd_um * 1e-6f;
  LOG("agent: boot (user %d, eq %p, vo 0x%x, eye distance %f mm)", g_vr.user_id, g_vr.end_eq,
      g_vr.display_vo, g_vr.ipd * 1000.0f);
  hooks_probe_hidden_dmem("at the game's start");
  agent_tick();
  u64 table = multiview_boot(multiview); /* (multiview.c: the patch table for inset mode, agent.multiview; 0 without it) */
  multiview_booted(); /* (the payload applies the table after this returns) */
  return table;
}
