/* PS VR2 from inside the PS4 game: loads the PS5 VR stack (AgcDriver, Agc, Hmd2, VrTracker2),
 * reads the head pose from VrTracker2, and hands every frame the game renders for PS VR (its two
 * eye textures and the pose they were drawn for) to Hmd2's reprojection at 120 Hz.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

#define VR_QUEUE_FULL ((int)0x8A72001A)
#define VR_MODE_120HZ 0x2000full
#define FLAT_TAN 0.8f /* a flat picture's half-width as a tangent (vr2.c: Start2dVr screens) */
#define VR_MODE_90HZ 0x2000cull /* Hmd2 takes these two (EnableVrMode); its timings 81:108 (90 Hz unused here) */

struct layer_set {
  void *layers;
  u32 count, pad;
};
struct vo_buffer {
  void *data, *metadata, *r0, *r1;
};

static int g_hmd = -1;
static int g_vh = -1;

/* ---------------------------------------------------------------- helpers */
int vr2_override(int handle, const char *nid, void *fn) {
  if (handle < 0)
    return -1;
  return agent_call(CMD_OVERRIDE, (u64)handle, (u64)nid, (u64)fn, 0, 3000, 0, 0);
}

struct mod_info {
  u64 size;
  char name[256];
  struct {
    u64 addr;
    u32 size;
    s32 prot;
  } seg[4];
  u32 nseg;
  u8 rest[0x160 - 0x14c];
};

int vr2_find_module(const char *name) {
  static struct mod_info mi;
  size_t n = strlen(name) + 1;
  for (int h = 0; h < 2000; h++) {
    memset(&mi, 0, sizeof mi);
    mi.size = sizeof mi;
    if (sceKernelGetModuleInfo(h, &mi) == 0 && !memcmp(mi.name, name, n))
      return h;
  }
  return -1;
}

/* the module an address is in (its name, and its base): for exit logging's callers */
u64 vr2_module_of(u64 addr, const char **name) {
  static struct mod_info mi;
  static char found[64];
  *name = "?";
  for (int h = 0; h < 2000; h++) {
    memset(&mi, 0, sizeof mi);
    mi.size = sizeof mi;
    if (sceKernelGetModuleInfo(h, &mi) != 0)
      continue;
    for (u32 s = 0; s < mi.nseg && s < 4; s++)
      if (addr >= mi.seg[s].addr && addr < mi.seg[s].addr + mi.seg[s].size) {
        u32 k = 0;
        for (; k < sizeof found - 1 && mi.name[k]; k++)
          found[k] = mi.name[k];
        found[k] = 0;
        *name = found;
        return mi.seg[0].addr;
      }
  }
  return 0;
}

u64 vr2_module_base(int handle) {
  static struct mod_info mi;
  memset(&mi, 0, sizeof mi);
  mi.size = sizeof mi;
  return handle >= 0 && sceKernelGetModuleInfo(handle, &mi) == 0 ? mi.seg[0].addr : 0;
}

/* CPU and GPU readable and writable direct memory of one memory type */
void *vr2_dalloc_type(size_t size, size_t align, int type) {
  size = (size + 0xffff) & ~(size_t)0xffff;
  if (align < 0x10000)
    align = 0x10000;
  s64 off = 0;
  void *va = 0;
  if (sceKernelAllocateDirectMemory(0, (s64)sceKernelGetDirectMemorySize(), size, align, type,
                                    &off) != 0 ||
      sceKernelMapDirectMemory(&va, size, 0x33, 0, off, align) != 0)
    return 0;
  memset(va, 0, size);
  static u64 total;
  total += size;
  LOG("vr2: %lu KB of direct memory (type %d), %lu KB in all", (u64)size >> 10, type,
      total >> 10);
  return va;
}

/* ---------------------------------------------------------------- the reserve
 * PS VR2's memory beyond the game's (after Ghost-VR's vr2.c reserve_hidden): the game's direct-memory
 * container holds a 512 MB segment past the size it is told (0x4000000000 + its size; free, the
 * probe found, 2026-11-04). RESERVE_MB at its top, allocated and mapped as vr2_dalloc maps (CPU and
 * GPU read and write), its first and last bytes written and read back, holds PS VR2's buffers
 * (vr2_dalloc_pref: the reprojection's work and display buffers, its shader, the head tracker, the
 * video decoder's queues, the TV mirror, the fingerprints); never the palm tracker's (hand.c). The
 * rest of the segment, spare_lo to spare_hi, serves the game's blocks refused in its own range
 * (hooks.c h_sceKernelAllocateDirectMemory). */
#define RESERVE_MB 154 /* PS VR2's buffers in it: 153.0 MB, all made at its start and fixed (the
                         * head tracker 32, Hmd2's work 1 and display buffers 96, the TV mirror 24,
                         * shaders); + the fingerprints' 48 KB when asked for.
                         * A buffer that finds it full goes to the game's range (vr2_dalloc_pref).
                         * The rest of the 512 MB is the game's (step 3) */
struct vr2_reserve g_reserve;
void vr2_reserve_init(void) {
  u64 total = (u64)sceKernelGetDirectMemorySize(), size = (u64)RESERVE_MB << 20;
  s64 top = 0x4000000000ll + (s64)total + 0x20000000ll, phys = 0;
  void *va = 0;
  int rc = sceKernelAllocateDirectMemory(top - (s64)size, top, size, 0x200000, 0, &phys);
  int mrc = rc == 0 ? sceKernelMapDirectMemory(&va, size, 0x33, 0, phys, 0x200000) : -1;
  int ok = 0;
  if (rc == 0 && mrc == 0 && va) {
    volatile u8 *b = va;
    b[0] = 0x5a, b[size - 1] = 0xa5;
    ok = b[0] == 0x5a && b[size - 1] == 0xa5;
    b[0] = 0, b[size - 1] = 0;
  }
  LOG("reserve: %u MB in the extra segment below 0x%lx: allocate -> 0x%x at 0x%lx, map -> 0x%x at %p, "
      "first and last byte %s", RESERVE_MB, top, rc, phys, mrc, va, ok ? "written and read back" : "NOT usable");
  /* (not usable: its allocation stays unused, in memory the game is never told of) */
  if (ok) {
    g_reserve.va = va, g_reserve.phys = phys, g_reserve.size = size, g_reserve.used = 0;
    g_reserve.spare_lo = 0x4000000000ll + (s64)total, g_reserve.spare_hi = phys;
  }
}

/* A piece of the reserve (0 when it isn't there or is full), cleared, as vr2_dalloc gives one. */
void *vr2_ralloc(size_t size, size_t align) {
  static volatile u32 lock;
  if (!g_reserve.va || align > 0x200000)
    return 0; /* (the reserve is 2 MB aligned: a larger alignment from the game's range) */
  size = (size + 0xffff) & ~(size_t)0xffff;
  if (align < 0x10000)
    align = 0x10000;
  while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE))
    sceKernelUsleep(100);
  u64 at = (g_reserve.used + align - 1) & ~(u64)(align - 1);
  void *va = 0;
  if (at + size <= g_reserve.size) {
    va = g_reserve.va + at;
    g_reserve.used = at + size;
  }
  __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
  if (va) {
    memset(va, 0, size);
    LOG("reserve: %lu KB at +0x%lx, %lu of %lu MB used", (u64)size >> 10, at, g_reserve.used >> 20,
        g_reserve.size >> 20);
  }
  return va;
}

/* the reserve first, else the game's range */
void *vr2_dalloc_pref(size_t size, size_t align, const char *what) {
  void *va = vr2_ralloc(size, align);
  LOG("reserve: %s, %lu KB, %s", what, (u64)size >> 10, va ? "in the reserve" : "in the game's range");
  return va ? va : vr2_dalloc(size, align);
}

/* ... of the first type that is available */
void *vr2_dalloc(size_t size, size_t align) {
  static const int types[] = {0, 3, 10, 12};
  for (unsigned t = 0; t < sizeof types / sizeof types[0]; t++) {
    void *va = vr2_dalloc_type(size, align, types[t]);
    if (va)
      return va;
  }
  LOG("vr2: out of direct memory (0x%lx bytes)", size);
  hooks_log_dmem("at the failure");
  return 0;
}

/* ---------------------------------------------------------------- the PS5 libraries */
/* They are PS5-only (ELF ABI version 2); the payload built version-3 fake-signed copies from this
 * console's firmware. The injector makes them visible in the game's sandbox (a read-only mount
 * at /summerlessonvr2) while they load. */
static int load_stack(void) {
  static const char *const names[NLIBS] = {"libSceAgcDriver.sprx", "libSceAgc.sprx",
                                           "libSceHmd2.sprx", "libSceVrTracker2.sprx"};
  int h[NLIBS] = {-1, -1, -1, -1}, ok = 1;
  char path[64];
  cvars_fetch(); /* the agent.* options, before anything they govern starts */
  for (int i = 0; i < NLIBS && ok; i++) {
    agent_call(CMD_STAGE, (u64)i, 0, 0, 0, 3000, 0, 0);
    agent_snprintf(path, sizeof path, "/summerlessonvr2/%s", names[i]);
    int res = 0;
    { /* said before the load: the game died inside the first load once (run 1) */
      u8 st[256];
      u32 sdk = 0;
      int sr = sceKernelStat ? sceKernelStat(path, st) : -1;
      if (sceKernelGetCompiledSdkVersion)
        sceKernelGetCompiledSdkVersion(&sdk); /* (called without its pointer in run 2, it wrote
                                                  the version over the path: EINVAL) */
      /* sceKernelIsNeoMode = sysctl kern.neomode: whether the PS5 runs this PS4 title in its
       * PS4 Pro (Neo) configuration. The title declares Neo support (param.sfo ATTRIBUTE bit
       * 0x800000) and asks at nine sites (one cached at 0x4b8ef08: memory pools +512 MB, the
       * RHI's Neo settings, the device-profile name) */
      LOG("vr2: loading %s (stat %s, size %lu; the game's SDK 0x%x, Neo mode %d)", path,
          sr == 0 ? "ok" : "failed", sr == 0 ? *(u64 *)(st + 0x48) : 0, sdk,
          sceKernelIsNeoMode ? sceKernelIsNeoMode() : -1);
    }
    h[i] = sceKernelLoadStartModule(path, 0, 0, 0, 0, &res);
    LOG("vr2: load %s -> 0x%x (start 0x%x)", names[i], h[i], res);
    ok = h[i] >= 0;
    agent_call(CMD_STAGE, (u64)i, 1, 0, 0, 3000, 0, 0);
  }
  agent_call(CMD_STUB, h[0] >= 0 ? (u64)h[0] : 0, h[1] >= 0 ? (u64)h[1] : 0,
             h[2] >= 0 ? (u64)h[2] : 0, h[3] >= 0 ? (u64)h[3] : 0, 3000, 0, 0);
  if (!ok)
    return -1;
  ps5compat_install(h);
  gen1_install(h);
  /* the palm tracker's library now too (its start function runs with the four's overrides in
   * place, as when it was loaded later), not in mid-game: hand.c */
  hand_load();
  agent_call(CMD_RESOLVE, 0, 0, 0, 0, 3000, 0, 0);
  return sceHmd2Initialize && sceHmd2ReprojectionSetParam && sceVrTracker2GetResult &&
                 sceVideoOutRegisterBuffers2
             ? 0
             : -1;
}

static int open_hmd(void) {
  u8 init[16] = {0};
  int rc = sceHmd2Initialize(init);
  float fov[16];
  memset(fov, 0, sizeof fov);
  sceHmd2GetFieldOfViewWithoutHandle(fov);
  g_hmd = sceHmd2Open(g_vr.user_id, 0, 0, 0);
  u8 info[64];
  memset(info, 0, sizeof info);
  sceHmd2GetDeviceInformation(info);
  LOG("vr2: Hmd2Initialize 0x%x, Hmd2Open 0x%x (status %u, worn %u, lens separation %u)", rc,
      g_hmd, *(u32 *)info, info[20], info[21]);
  /* The lenses are set to the player's eye distance, which the game should draw its eyes
   * apart by. It asks once, before PS VR2 is up, so it is kept for the next start. */
  if (info[21] >= 50 && info[21] <= 80 && (u32)(g_vr.ipd * 1000.0f + 0.5f) != info[21]) {
    agent_call(CMD_SAVE_IPD, (u64)info[21] * 1000, 0, 0, 0, 3000, 0, 0);
    LOG("vr2: eye distance %u mm kept for the next start (this one draws at %f mm)", info[21],
        g_vr.ipd * 1000.0f);
  }
  return g_hmd < 0 ? -1 : 0;
}

/* The output switches to VR only once PS VR2 is ready (Hmd2 status 0). Switched while the
 * headset is off (status 2), the system puts its own "turn on PS VR2" screen over the game.
 * The game runs on meanwhile. A status other than "off" that never turns ready gets 20 s. */
static void wait_headset_ready(void) {
  u8 info[64];
  for (u32 i = 0; g_shared.cmd != CMD_QUIT; i++) {
    memset(info, 0, sizeof info);
    int rc = sceHmd2GetDeviceInformation(info);
    u32 st = *(u32 *)info;
    if (rc == 0 && st == 0) {
      if (i)
        LOG("vr2: headset ready after %u s", i / 2);
      return;
    }
    if (i == 0)
      LOG("vr2: headset not ready (status %u, rc 0x%x): the VR switch waits for it", st, rc);
    if (i == 2) {
      agent_call(CMD_NOTIFY,
                 (u64) "SummerLessonVR2: turn on your PS VR2. The game goes to the headset as soon "
                       "as it's on.",
                 0, 0, 0, 3000, 0, 0);
    }
    if (st != 2 && i >= 40) {
      LOG("vr2: headset status %u for 20 s, switching anyway", st);
      return;
    }
    sceKernelUsleep(500000);
  }
}

/* ---------------------------------------------------------------- output */
static int output_init(void) {
  struct size_align wb = sceHmd2ReprojectionQueryBufferSizeAlign();
  struct size_align db = sceHmd2ReprojectionQueryDisplayBufferSizeAlign();
  /* step 2c: from the reserve (Ghost-VR keeps its headset work and display buffers there) */
  void *work = vr2_dalloc_pref(wb.size, wb.align, "the reprojection's work"),
       *disp = vr2_dalloc_pref(db.size, db.align, "the headset's display buffers");
  if (!work || !disp)
    return -1;
  u8 rp[72];
  memset(rp, 0, sizeof rp);
  *(void **)(rp + 0x00) = work;
  *(void **)(rp + 0x08) = disp;
  /* the reprojection thread gets the game's own PS VR reprojection priority and CPUs (a PS4
   * process may not use the cores a PS5 app would name) */
  *(u32 *)(rp + 0x10) = g_vr.reproj_prio > 0 ? (u32)g_vr.reproj_prio : 256;
  *(s32 *)(rp + 0x14) = g_vr.reproj_affinity ? (s32)g_vr.reproj_affinity : 0x30;
  *(u32 *)(rp + 0x18) = 1;
  int rc = sceHmd2ReprojectionInitialize(rp, 0);
  LOG("vr2: ReprojectionInitialize -> 0x%x", rc);
  if (rc != 0 || gen1_upload_shader() != 0)
    return -1;
  ps5compat_allow_vr_layout();
  /* the headset's refresh: 120 Hz, or 90 with the game's native 90 Hz sequence (framerate.txt
   * 90: the game then asks PS VR for refresh 0x23 and paces on one vblank a frame) */
  int hz90 = astro_hdr.refresh_hz == 90;
  for (int tries = 0; tries < 30 && g_shared.cmd != CMD_QUIT; tries++) {
    rc = sceHmd2ReprojectionEnableVrMode(hz90 ? VR_MODE_90HZ : VR_MODE_120HZ);
    if (rc == 0)
      break;
    LOG("vr2: EnableVrMode(%s) -> 0x%x, retrying", hz90 ? "90 Hz" : "120 Hz", rc);
    if (hz90 && tries == 2) { /* refused: the 120 Hz mode, the game's 90 frames shown at 120 */
      hz90 = 0;
      LOG("vr2: the 90 Hz mode refused: 120 Hz");
    }
    sceKernelUsleep(2000000);
  }
  g_refresh_hz = hz90 ? 90 : 120; /* the vblank thread's clock and late-refresh judgement */
  g_vh = sceVideoOutOpen(0xff, 32, 0, 0);
  LOG("vr2: VR mode 0x%x at %s, VR port 0x%x", rc, hz90 ? "90 Hz" : "120 Hz", g_vh);
  return g_vh < 0 ? -1 : 0;
}

/* ---------------------------------------------------------------- tracking */
static volatile int g_tracker_ok;

static int tracker_init(void) {
  u32 qparam[8] = {0x20, 0}, qout[8] = {0};
  if (sceVrTracker2QueryMemory(qparam, qout) != 0)
    return -1;
  void *twork = vr2_dalloc_pref(qout[1], qout[2] > 0x10000 ? qout[2] : 0x10000, "the head tracker's work");
  if (!twork)
    return -1;
  const u64 mask = 0x30; /* CPUs 4 and 5, which a PS4 process may use */
  u8 ip[0x48];
  memset(ip, 0, sizeof ip);
  *(u32 *)(ip + 0x00) = 0x48;
  *(s32 *)(ip + 0x08) = 700;
  *(s32 *)(ip + 0x0c) = 700;
  *(u64 *)(ip + 0x10) = mask;
  *(u64 *)(ip + 0x18) = mask;
  *(void **)(ip + 0x20) = twork;
  *(u32 *)(ip + 0x28) = qout[1];
  *(u32 *)(ip + 0x2c) = qout[2];
  int rc = sceVrTracker2Initialize(ip);
  int rc2 = rc == 0 ? sceVrTracker2RegisterDevice(0, g_hmd) : -1;
  LOG("vr2: VrTracker2 0x%x, headset 0x%x", rc, rc2);
  if (rc != 0 || rc2 != 0)
    return -1;
  g_tracker_ok = 1;
  return 0;
}

/* Head pose in VrTracker2 space at `time` (process time, us; 0 = now). Called by the game's
 * tracker hook (vr.c) on game threads. */
int vr2_head_pose(u64 time, float pos[3], float quat[4]) {
  if (!g_tracker_ok)
    return 0;
  u8 gp[0x20], res[0x400] __attribute__((aligned(16)));
  memset(gp, 0, sizeof gp);
  *(u32 *)(gp + 0x00) = 0x20;
  *(s32 *)(gp + 0x04) = g_hmd;
  *(u64 *)(gp + 0x10) = time;
  if (sceVrTracker2GetResult(gp, res) != 0)
    return 0;
  { /* the tracker's own clock against ours (the time asked for is in ours): its result's u64s */
    static u32 said;
    if (said < 3 && time && (said++ , 1)) {
      const u64 *r = (const u64 *)res;
      LOG("vr2: GetResult(t %lu, now %lu): res %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx | "
          "%lx %lx %lx %lx", time, sceKernelGetProcessTime(), r[0], r[1], r[2], r[3], r[4], r[5],
          r[6], r[7], r[8], r[9], r[10], r[11], r[16], r[17], r[18], r[19]);
    }
  }
  /* Right after registration the tracker answers with fixed default poses, not the head; a
   * tracked head changes on every sample: wait for 8 changing samples in a row. */
  static float prev[7];
  static volatile u32 changes, live;
  if (!live) {
    float now[7];
    memcpy(now, res + 0x60, 12);
    memcpy(now + 3, res + 0x70, 16);
    float d = 0;
    for (int i = 0; i < 7; i++)
      d += (now[i] - prev[i]) * (now[i] - prev[i]);
    memcpy(prev, now, sizeof prev);
    changes = d > 1e-12f ? changes + 1 : 0;
    if (changes < 8)
      return 0;
    live = 1;
    LOG("vr2: head tracking live");
  }
  memcpy(pos, res + 0x60, 12);
  memcpy(quat, res + 0x70, 16);
  return 1;
}

/* ---------------------------------------------------------------- the game's frames */
/* each pair's registered layer records: record 0 the eyes (type 0x10), and with
 * agent.insetwin record 1, the insets' layer with Hmd2's window test (below) */
static u8 g_layers[MAX_PAIRS][2 * 0x130] __attribute__((aligned(16)));
static struct layer_set g_sets[MAX_PAIRS];
static u32 g_buf_off; /* the buffer index a pair is registered at, past a refused registration's */

/* ---- multi-view: the insets' layer with a window test (agent.insetwin) ----
 * A type 0x10 layer record registered with byte +0x44 = 1 makes Hmd2 set the layer's flag bit 0
 * (SetParam 0x202c2 copies it to the descriptor's +0xcd, the compositor 0xaddc sets the flag),
 * and its shader then draws the layer only where each channel's window uv is within 0..1
 * (0x2b4c: tested, the lanes outside skipped), through two 1D lookup tables an eye (u, v: cb B
 * +0x88/+0xa8, red channel, clamped) before its viewport. With identity tables that is type 0x10
 * with a window: each eye's inset no longer seen by the other eye, so the insets can sit side by
 * side in one band of rows (no per-eye cuts). Registration (Hmd2 0x1b7a0 via VideoOut 0x11920)
 * checks only counts, free slots, non-zero bases (the four tables at +0x48/+0x68/+0xa8/+0xc8:
 * 0x8a720023) and zero bytes; a refused one keeps its set and buffer slots taken.
 * (docs/MULTIVIEW.md 19; side-session note hmd2-type11.md)
 * agent.insetwin: 1 registered only (frames as before), 2 the insets' layer on it with both
 * windows off view, 3 in use. Registration is decided at the first registration (launch). */
#define LUT_N 1024u
#include "insetwin.h"
static u32 g_win_layers;  /* the pairs were registered with record 1 */
/* layer 1 kept to the insets' windows by Hmd2 (record 1's window test in use): inset.c's pass
 * then needs to cover only the insets' rows */
int vr2_window_test(void) { return g_win_layers && g_opt_insetwin >= 2; }
/* agent.insetblink=N (a measuring tool, live): the insets' layer left out every other N ms, so
 * the headset's own picture can be measured with and without it (mirror.c probe_hmd) */
volatile u32 g_blink_off; /* the newest frame passed without the layer */
volatile u64 g_blink_at;  /* when that last changed (process time, us) */
static u32 g_lut_tsharp[8]; /* the identity table's gfx10 T# (gen1.c converts it, as Hmd2's own) */
static int lut_make(void) {
  if (g_lut_tsharp[0])
    return 1;
  u16 *t = vr2_dalloc_pref(LUT_N * 8, 0x1000, "the insets' window lookup table");
  if (!t)
    return 0;
  lut_fill(t, LUT_N);
  u64 base = (u64)t;
  if (base < 0x100000000ull || base >= 0x10000000000ull || (base & 0xff)) {
    LOG("vr2: the insets' lookup table at 0x%lx: outside what gen1.c converts, no window test", base);
    return 0;
  }
  lut_tsharp(g_lut_tsharp, base, LUT_N);
  LOG("vr2: the insets' window lookup table: %u texels at 0x%lx (T# %08x %08x %08x %08x)", LUT_N,
      base, g_lut_tsharp[0], g_lut_tsharp[1], g_lut_tsharp[2], g_lut_tsharp[3]);
  return 1;
}
static u64 g_pair_base[MAX_PAIRS];
static u32 g_npairs;

static u64 tsharp_base(const u32 *t) { return ((u64)t[0] | ((u64)(t[1] & 0xff) << 32)) << 8; }

/* The game draws into a few fixed pairs of eye textures (Astro Bot: three separate pairs; Summer
 * Lesson: two, each eye half of one side-by-side texture, and a flat texture for its 2D screens).
 * They become the VR port's buffers, with the game's own GCN descriptors (no copy, no detiling):
 * the pairs seen in the first 3 s after the first frame as one set, and every pair first seen
 * later (a 2D screen's texture, a pair drawn into later) as a set of its own, up to MAX_PAIRS. */
static u32 g_nsets;
static u32 g_pair_failed; /* a registration refused: not tried again */

static void pair_note(u32 n, const struct vr_frame *f) {
  g_pair_base[n] = tsharp_base(f->tsharp[0]);
  *(u32 *)g_layers[n] = 0x10;
  memcpy(g_layers[n] + 0x04, f->tsharp[0], 32);
  memcpy(g_layers[n] + 0x24, f->tsharp[1], 32);
  gen1_note_game_tsharp(f->tsharp[0]);
  gen1_note_game_tsharp(f->tsharp[1]);
  g_sets[n].layers = g_layers[n];
  g_sets[n].count = 1;
  if (n == 0 && !g_npairs) /* (decided once, at the first registration) */
    g_win_layers = g_opt_insetwin && g_multiview_rows && lut_make();
  if (g_win_layers) { /* record 1: record 0 with the window test and the identity tables */
    win_record(g_layers[n] + 0x130, g_layers[n], g_lut_tsharp);
    g_sets[n].count = 2;
  }
  const u32 *t = f->tsharp[0];
  LOG("vr2: eye texture pair %u: L base 0x%lx, R base 0x%lx, %ux%u%s", n, tsharp_base(f->tsharp[0]),
      tsharp_base(f->tsharp[1]), (t[2] & 0x3fff) + 1, ((t[2] >> 14) & 0x3fff) + 1,
      f->flat ? " (a flat picture)" : "");
}

/* pairs first..first+n-1 as VR port buffers (one set); the buffer index is the pair's index */
static int pairs_register(u32 first, u32 n) {
  const u32 *t = (const u32 *)(g_layers[first] + 4);
  u32 w = (t[2] & 0x3fff) + 1, h = ((t[2] >> 14) & 0x3fff) + 1;
  struct vo_buffer bufs[MAX_PAIRS];
  memset(bufs, 0, sizeof bufs);
  for (u32 i = 0; i < n; i++)
    bufs[i].data = &g_sets[first + i];
  u8 attr[80];
  memset(attr, 0, sizeof attr);
  u32 wreg = w;
  if (w > 3840) {
    /* registered at one eye's width, as Astro Bot's 2000-wide eyes are; the frames' descriptors
     * keep the whole side-by-side texture (the compositor reads through those, up to 4096 wide).
     * The first registration of a 4000-wide buffer at its full width brought the console down
     * (2026-11-01 03:19); at one eye's width 2000 and 2048 run clean. */
    wreg = w / 2;
    LOG("vr2: a %ux%u texture registered as %ux%u (one eye's width)", w, h, wreg, h);
  }
  sceVideoOutSetBufferAttribute2(attr, 0x8000000000000000ull, 0, wreg, h, 0, 0, 0);
  int rc = sceVideoOutRegisterBuffers2(g_vh, (s32)g_nsets, (s32)(first + g_buf_off), bufs, (s32)n,
                                       attr, 0x10, 0);
  LOG("vr2: pairs %u-%u (%ux%u) registered as set %u, buffers %u-%u, %u layer records -> 0x%x",
      first, first + n - 1, w, h, g_nsets, first + g_buf_off, first + g_buf_off + n - 1,
      g_sets[first].count, rc);
  if (rc == 0) {
    g_nsets++;
  } else if (g_win_layers && g_nsets < 3 && first + g_buf_off + 2 * n <= 15) {
    /* refused with the window's record: once more without it, on the next set and the buffers
     * after these (a refused registration keeps its set and buffer slots, Hmd2 0x1b8a3/0x1b8f0) */
    g_win_layers = 0;
    for (u32 i = 0; i < n; i++)
      g_sets[first + i].count = 1;
    g_nsets++, g_buf_off += n;
    LOG("vr2: refused with the insets' window record: again without it (set %u, buffers from %u)",
        g_nsets, first + g_buf_off);
    return pairs_register(first, n);
  }
  return rc;
}

static int pair_known(const struct vr_frame *f) {
  u64 b = tsharp_base(f->tsharp[0]);
  for (u32 i = 0; i < g_npairs; i++) /* a texture of another size at a known base is a new pair
                                      * (the game remakes its eye textures on a new size) */
    if (g_pair_base[i] == b && *(const u32 *)(g_layers[i] + 4 + 8) == f->tsharp[0][2])
      return (int)i;
  return -1;
}

/* a pair first seen once the output runs: registered on the spot, as a set of its own */
static int pair_add_late(const struct vr_frame *f) {
  if (g_npairs >= MAX_PAIRS || g_pair_failed)
    return -1;
  pair_note(g_npairs, f);
  if (pairs_register(g_npairs, 1) != 0) {
    g_pair_failed = 1;
    return -1;
  }
  return (int)g_npairs++;
}

/* waits for the game's first frame (a 2D screen's or an eye pair's), takes the pairs seen in
 * the 3 s after the last new one, and registers them */
static int register_game_pairs(void) {
  u32 seen = 0, last = g_vr.frame_w;
  u64 settled = 0;
  while (seen < MAX_PAIRS && g_shared.cmd != CMD_QUIT) {
    u32 w = g_vr.frame_w;
    if (w == last) {
      if (seen && sceKernelGetProcessTime() >= settled)
        break; /* no new pair in 3 s: that is all of them for now */
      sceKernelUsleep(2000);
      continue;
    }
    last = w;
    const struct vr_frame *f = &g_vr.frame[w & 3];
    u64 b = tsharp_base(f->tsharp[0]);
    int known = 0;
    for (u32 i = 0; i < seen; i++)
      known |= g_pair_base[i] == b;
    if (known) {
      if (seen && sceKernelGetProcessTime() >= settled)
        break;
      continue;
    }
    settled = sceKernelGetProcessTime() + 3000000;
    pair_note(seen, f);
    seen++;
  }
  if (!seen)
    return -1;
  int rc = pairs_register(0, seen);
  if (rc != 0)
    return rc;
  g_npairs = seen;
  /* half rate: a frame every two refreshes, 60 fps -> 120 Hz; full rate at the game's native
   * 120 (framerate.txt) */
  int full = astro_hdr.frame_pace == 1;
  if (g_opt_hmdrate == 60 || g_opt_hmdrate == 120) { /* the crossed test (agent.hmdrate) */
    full = g_opt_hmdrate == 120;
    LOG("vr2: Hmd2's rate set by agent.hmdrate: %s, the game's own pace %u", full ? "full" : "half",
        astro_hdr.frame_pace);
  }
  int c2 = sceHmd2ReprojectionSetRenderConfig(full ? 0 : 1, 0, 0);
  LOG("vr2: %u pairs registered, %s rate 0x%x", seen, full ? "full" : "half", c2);
  return 0;
}

/* the game handed a frame over (hooks.c): game_loop's wait ends then, not up to 2 ms later as its
 * 1 ms sleep had it (measured: the hand-off went from 0.5-2 ms to under 0.1) */
#define READY_ID 0x5245
static void *volatile g_ready_eq;

void vr2_frame_ready(void) {
  void *eq = g_ready_eq;
  if (eq)
    sceKernelTriggerUserEvent(eq, READY_ID, 0);
}

static void game_loop(void) {
  static u8 lp[2 * 0x148] __attribute__((aligned(16))); /* layer 0, and the insets' layer 1 */
  struct {
    void *layers;
    s32 count, pad;
    void *ext;
  } param = {lp, 1, 0, 0};
  u64 frame = 0;
  u32 last = g_vr.frame_w, errors = 0;
  LOG("vr2: game frames -> headset");
  {
    void *eq = 0;
    int c = sceKernelCreateEqueue ? sceKernelCreateEqueue(&eq, "SummerLessonVR2 frames") : -1;
    int a = c == 0 ? sceKernelAddUserEventEdge(eq, READY_ID) : -1;
    if (c == 0 && a == 0)
      g_ready_eq = eq;
    LOG("vr2: woken by the game's frames (0x%x, 0x%x)", c, a);
  }
  u64 next_mount = 0, settle_at = 0;
  int mounted = -1;
  while (g_shared.cmd != CMD_QUIT) {
    /* PS VR2's attachment sensor: once the headset has been on for 2 s (strap done), the seat
     * is where the player is. A game started with the headset off took its seat elsewhere. */
    u64 now = sceKernelGetProcessTime();
    if (now >= next_mount) {
      u8 info[64];
      memset(info, 0, sizeof info);
      next_mount = now + 500000;
      static u32 last_status = ~0u;
      int irc = sceHmd2GetDeviceInformation(info);
      if (*(u32 *)info != last_status) {
        LOG("vr2: headset status %u (rc 0x%x, worn %u)", *(u32 *)info, irc, info[20]);
        last_status = *(u32 *)info;
      }
      int worn = info[20] != 0 || (g_test & TEST_MOUNTED) != 0; /* test.txt: counted as worn */
      if (irc == 0 && worn != mounted) {
        LOG("vr2: headset %s", mounted < 0 ? (worn ? "on" : "off")
                                           : (worn ? "put on" : "taken off"));
        settle_at = mounted == 0 && worn ? now + 2000000 : 0;
        mounted = worn;
        g_vr.hmd_off = !mounted; /* the game pauses (hooks.c) */
      }
    }
    static u64 audio_at;
    if (!audio_at)
      audio_at = now + 10000000;
    else if (now >= audio_at && audio_at != 1) {
      audio_at = 1;
      hooks_log_audio();
      hooks_log_dmem("10 s into the headset output (PS VR2's buffers made)");
    }
    if (settle_at && now >= settle_at) {
      settle_at = 0;
      vr_request_recenter("headset put on");
    }
    u32 w = g_vr.frame_w;
    /* Frames go to Hmd2 in order, one a refresh. Hmd2 builds each refresh just after the flip
     * before, from the newest frame passed by then (gen1.c sees which, in the flipArg it sets),
     * taking it just before the flip. A frame passed too late for that build is taken at the
     * next, and a frame passed meanwhile would be taken instead of it, the late one never
     * shown: so no frame goes while the last passed is not built yet. A pass that missed its
     * build (the build after it took an older frame, or it was made after the flip: the build's
     * record can predate the pass by microseconds) is a refresh late, and every frame after it
     * would be, each held: the frame after next is aimed a refresh later (hooks.c, on
     * g_late_pass), which has frames passed before their flips again. One release at a time:
     * landed once a pass is taken by the build after it, given up on after three misses in a
     * row. (Measured over the same four minutes replayed: the earlier hold, on the refresh
     * count, held every frame after the first late one for good; a 0.25 ms wait before the
     * flip cost repeats on frames that would have made it.) */
    static u32 last_vrf, build_at_pass, checked, releases_seen, misses;
    static u64 last_pass_us, last_pass_flip;
    static int last_after_flip;
    u32 built = 0, nb = gen1_last_built(&built);
    if (built > last_vrf + 4 || built + 8 < last_vrf)
      built = 0; /* not a frame passed lately */
    int seen = last_vrf && nb != build_at_pass && built != 0; /* a build since the last pass */
    int unbuilt = last_vrf && built != 0 && built < last_vrf;
    if (last_vrf && !checked && (seen || last_after_flip)) {
      checked = 1; /* once per pass: was it taken? */
      if (seen && built >= last_vrf) {
        releases_seen = g_release_n, misses = 0;
      } else {
        if (g_release_n != releases_seen && ++misses >= 3)
          releases_seen = g_release_n, misses = 0;
        if (g_release_n == releases_seen)
          g_late_pass = 1;
      }
    }
    int hold = w != last && unbuilt;
    static u64 held_since; /* a hold is bounded: Hmd2 may stop building (the headset dark) */
    if (!hold)
      held_since = 0;
    else if (!held_since)
      held_since = sceKernelGetProcessTime();
    else if (sceKernelGetProcessTime() - held_since > 20000)
      last_vrf = 0, hold = 0, held_since = 0; /* two refreshes without a build: the frame goes */
    if (w == last || hold) {
      if (hold) {
        sceKernelUsleep(200);
        continue;
      }
      /* woken as the game hands a frame over, or in 1 ms for the rest */
      if (g_ready_eq) {
        u8 ev[32];
        int out = 0;
        u32 timo = 1000;
        u64 t = sceKernelGetProcessTime();
        int rc = sceKernelWaitEqueue(g_ready_eq, ev, 1, &out, &timo);
        static u32 spurious; /* returns with nothing, in a row */
        if (out)
          spurious = 0;
        else if (sceKernelGetProcessTime() - t < 200 && ++spurious >= 3) {
          /* neither a frame nor the timeout: the wait doesn't work, sleep as before */
          LOG("vr2: the frames' wait returned 0x%x at once: sleeping instead", rc);
          g_ready_eq = 0;
        }
      } else {
        sceKernelUsleep(1000);
      }
      continue;
    }
    u32 next = last + 1;
    /* At a frame a refresh (the native modes) the game hands over one frame a refresh and Hmd2
     * takes one: a frame of backlog (two hand-overs in one refresh, a repeat after a hitch) never
     * drained, each frame then a refresh later to the headset, read after the game had begun
     * drawing the frame three on into its texture (2026-11-04 at 120: built 27 ms after the
     * hand-over, every refresh "other"; 16.6 ms, finished). As PS VR did: the newest frame handed
     * over, the ones between never shown. (60: a free refresh every frame, no backlog.)
     * agent.newest=0: in order, as before. */
    if (g_opt_newest && astro_hdr.frame_pace == 1 && w - next >= 1 && w - next < 4) {
      hooks_note_unpassed(w - next);
      next = w;
    }
    if (w - next >= 4) {
      hooks_note_unpassed(w - 3 - next); /* gone from the ring before they could be passed */
      next = w - 3; /* the ring is four deep: the oldest still there */
    }
    struct vr_frame f;
    if (!vr_frame_copy(next, &f)) { /* overwritten already, or being written over */
      hooks_note_unpassed(w - next + 1);
      last = w;
      continue;
    }
    last = next;
    int b = pair_known(&f);
    if (b < 0 && (f.tsharp[0][2] & 0x3fff) + 1 >= 256) /* not Astro Bot's black 1x1 "paused" picture */
      b = pair_add_late(&f);
    { /* how the game cycles its eye textures: frames since the same pair was last handed over (2:
       * ping-pong, the game drawing into the pair handed over two frames ago while the compositor
       * may still show it; 3: three in turn), and a sample of the order, per 1200 frames */
      static u32 last_use[MAX_PAIRS], hist[5], nframes;
      static char order[40];
      if (b >= 0) {
        u32 d = last_use[b] ? frame + 1 - last_use[b] : 0;
        hist[d >= 4 ? 4 : d]++;
        last_use[b] = (u32)frame + 1;
        if (nframes < 32)
          order[nframes] = (char)('0' + b);
        if (++nframes >= 1200) {
          order[32] = 0;
          LOG_DETAIL("vr2: pairs reused after 1 frame %u, 2 %u, 3 %u, 4 or more %u (first use %u); order %s",
              hist[1], hist[2], hist[3], hist[4], hist[0], order);
          memset(hist, 0, sizeof hist);
          nframes = 0;
        }
      }
    }
    if (b < 0) {
      /* a pair not registered (the 1x1 picture, or one more than there is room for): the headset
       * keeps the last frame */
      static u32 unknown;
      if (unknown++ < 6)
        LOG("vr2: frame %u in an unregistered pair (L base 0x%lx, %ux%u): not shown", f.seq,
            tsharp_base(f.tsharp[0]), (f.tsharp[0][2] & 0x3fff) + 1,
            ((f.tsharp[0][2] >> 14) & 0x3fff) + 1);
      continue;
    }
    /* (A wait for the game's "drawn" labels before passing a frame was tried and taken out,
     * 2026-11-04: the picture is complete at the hand-over, the labels mark later GPU work, and
     * passing frames later showed textures already being drawn over: 14% of refreshes, the
     * fingerprints in mirror.c measured.) Frames go to Hmd2 at the hand-over. */
    frame++;
    int rc = sceHmd2ReprojectionBeginFrame(frame);
    for (int tries = 0; rc == VR_QUEUE_FULL && tries < 40; tries++) {
      sceKernelUsleep(1000);
      rc = sceHmd2ReprojectionBeginFrame(frame);
    }
    const u32 *t = f.tsharp[0];
    u32 tw = (t[2] & 0x3fff) + 1, th = ((t[2] >> 14) & 0x3fff) + 1;
    /* The game maps its FOV onto the texture: u = tan * scale + offset. Astro Bot: each eye its
     * own texture, scale 1 / (out + in) at full resolution (less under dynamic resolution), the
     * picture from u = 0. Summer Lesson: both eyes in one texture, scale 0.5 / (out + in), the
     * left eye from u = 0 and the right from u = 0.5. Each eye's viewport is where its picture
     * is: from the u of its outer edge (the left eye's left edge is at tan -out, the right eye's
     * left edge at tan -in), as wide as its field of view. */
    const struct hmd_fov *fv = &g_vr.title_fov;
    float tl[4] = {fv->tan_out, fv->tan_in, fv->tan_top, fv->tan_bottom}; /* left, right, up, down */
    float tr[4] = {fv->tan_in, fv->tan_out, fv->tan_top, fv->tan_bottom};
    u32 vx[2], vy[2], vw, vh;
    if (f.flat) {
      /* a flat picture: the whole texture to both eyes, as a screen before the seat, 16:9 at
       * FLAT_TAN half-width (about 77 degrees wide) */
      vx[0] = vx[1] = vy[0] = vy[1] = 0;
      vw = tw, vh = th;
      float ty = FLAT_TAN * (float)th / (float)tw;
      tl[0] = tl[1] = tr[0] = tr[1] = FLAT_TAN;
      tl[2] = tl[3] = tr[2] = tr[3] = ty;
    } else {
      float kx = f.uv[0].scale_x * (fv->tan_out + fv->tan_in);
      float ky = f.uv[0].scale_y * (fv->tan_top + fv->tan_bottom);
      if (kx <= 0.0f || kx > 1.0f)
        kx = 1.0f;
      if (ky <= 0.0f || ky > 1.0f)
        ky = 1.0f;
      vw = (u32)(tw * kx + 0.5f), vh = (u32)(th * ky + 0.5f);
      float x0 = f.uv[0].offset_x - fv->tan_out * f.uv[0].scale_x; /* the left eye's outer edge */
      float x1 = f.uv[1].offset_x - fv->tan_in * f.uv[1].scale_x;  /* the right eye's inner edge */
      float y0 = f.uv[0].offset_y - fv->tan_top * f.uv[0].scale_y;
      float y1 = f.uv[1].offset_y - fv->tan_top * f.uv[1].scale_y;
      vx[0] = x0 > 0.0f && x0 < 1.0f ? (u32)(tw * x0 + 0.5f) : 0;
      vx[1] = x1 > 0.0f && x1 < 1.0f ? (u32)(tw * x1 + 0.5f) : 0;
      vy[0] = y0 > 0.0f && y0 < 1.0f ? (u32)(th * y0 + 0.5f) : 0;
      vy[1] = y1 > 0.0f && y1 < 1.0f ? (u32)(th * y1 + 0.5f) : 0;
      if (vx[0] + vw > tw || vx[1] + vw > tw || vy[0] + vh > th || vy[1] + vh > th) {
        static u32 n;
        if (n++ < 4)
          LOG("vr2: frame %u: viewports %u,%u and %u,%u of %ux%u do not fit %ux%u", f.seq, vx[0],
              vy[0], vx[1], vy[1], vw, vh, tw, th);
        vx[0] = vx[1] = vy[0] = vy[1] = 0;
      }
    }
    /* multi-view step 4 (agent.insetviews=2): the inset views as layer 1, their alpha written
     * first (inset.c), here at the hand-over: Hmd2 reads a frame's texture from the next refresh
     * on, ~13 ms before the game's "drawn" labels (2026-11-07, logs/mv24-timing.log), so the
     * picture is complete by now; the passes made at the hand-over before then failed for
     * another reason (the read-only memory type in the T#: inset.c). A frame without the pass
     * goes as layer 0 alone. */
    s32 mv_rect[2][4];
    float mv_tan[2][4];
    int mv_layer = g_multiview_rows && g_opt_insetviews >= 2 && !f.flat &&
                   multiview_layer(mv_rect, mv_tan, f.render_time) &&
                   inset_alpha_frame(&f, (const s32(*)[4])mv_rect);
#ifndef RELEASE
    if (!f.flat) { /* the debug menu (menu.c) while open: centred 0.12 of tangent below each
                    * eye's axis, at the periphery's own density (layer 0's viewports and angles) */
      float ppx = (float)vw / (tl[0] + tl[1]), ppy = (float)vh / (tl[2] + tl[3]);
      float ax0 = (float)vx[0] + ppx * tl[0], ax1 = (float)vx[1] + ppx * tr[0];
      float ay0 = (float)vy[0] + ppy * tl[2], ay1 = (float)vy[1] + ppy * tr[2];
      /* at about 1.2 m, not at infinity (the same place in both eyes looked flat against the
       * room, worn 2026-11-07): each eye's copy moved in by half the eyes' 64 mm over 1.2 m */
      const float down = 0.12f, half_disp = 0.032f / 1.2f;
      s32 at[2][2] = {{(s32)(ax0 + half_disp * ppx - MENU_W / 2.0f), (s32)(ay0 + down * ppy - MENU_H / 2.0f)},
                      {(s32)(ax1 - half_disp * ppx - MENU_W / 2.0f), (s32)(ay1 + down * ppy - MENU_H / 2.0f)}};
      float hx = MENU_W / 2.0f / ppx, hy = MENU_H / 2.0f / ppy;
      float trect[4] = {-hx, hx, -down - hy, -down + hy};
      inset_menu_frame(&f, (const s32(*)[2])at, trect, half_disp);
    }
#endif
    static u32 said;
    if (said++ < 3 || said % 3600 == 0)
      LOG_DETAIL("vr2: frame %u: %ux%u texture, viewports %ux%u at %u,%u and %u,%u, tangents %f %f %f %f%s",
          f.seq, tw, th, vw, vh, vx[0], vy[0], vx[1], vy[1], tl[0], tl[1], tl[2], tl[3],
          f.flat ? " (flat)" : "");
    float pos[3], rot[4];
    vr_game_to_tracker(&f.render_pose, pos, rot);
    /* Hmd2's layer type 0x10, as Horizon Call of the Mountain fills it */
    memset(lp, 0, sizeof lp);
    u32 *u = (u32 *)lp;
    u[0x00 / 4] = 0x10;
    u[0x08 / 4] = tw, u[0x0c / 4] = th, u[0x10 / 4] = tw, u[0x14 / 4] = th;
    u[0x18 / 4] = 0x92, u[0x1c / 4] = 0x00fff000, u[0x20 / 4] = 0x05500000; /* sampler */
    u[0x28 / 4] = vw, u[0x2c / 4] = vh, u[0x30 / 4] = vx[0], u[0x34 / 4] = vy[0]; /* left: size, offset */
    u[0x38 / 4] = vw, u[0x3c / 4] = vh, u[0x40 / 4] = vx[1], u[0x44 / 4] = vy[1]; /* right */
    { /* multi-view: the texture is agent.multiview rows taller than the eyes (the insets' rows
       * below them). Hmd2 mapped layer 0 wrongly over the taller texture: the top of each eye
       * showed the inset rows, with black wedges (TV captures of its output, 2026-11-07). Layer 0
       * is told the texture ends below the eyes, as without multi-view (Hmd2 sets the T#'s height
       * from this: the same rows, the same layout); layer 1 keeps the whole height. */
      u32 rows = g_multiview_rows;
      if (rows && !f.flat && th > rows + 64 && vy[0] + vh <= th - rows && vy[1] + vh <= th - rows)
        u[0x0c / 4] = u[0x14 / 4] = th - rows;
    }
    memcpy(lp + 0x48, tl, 16);
    memcpy(lp + 0x58, tr, 16);
    memcpy(lp + 0x68, pos, 12);
    memcpy(lp + 0x74, rot, 16);
    *(u64 *)(lp + 0x88) = f.render_time;
    *(u64 *)(lp + 0x90) = frame;
    u[0x98 / 4] = 100;
    u[0xa0 / 4] = vw, u[0xa4 / 4] = vh, u[0xa8 / 4] = vw, u[0xac / 4] = vh;
    param.count = 1;
    if (mv_layer) { /* layer 1: the insets, in straight alpha, reprojected as layer 0 is */
      u8 *l1 = lp + 0x148;
      u32 *u1 = (u32 *)l1;
      static u32 mv_said;
      memcpy(l1, lp, 0x148);
      u1[0x0c / 4] = u1[0x14 / 4] = th; /* the whole texture: the insets are below the eyes */
      /* sampled past the texture's edges: transparent (CLAMP_BORDER, transparent black, word 3 0):
       * with layer 0's clamp a copy of an inset showed in each eye's outer lower corner
       * (2026-11-07, captures mv36), as Hmd2 samples a layer far outside its window */
      u1[0x18 / 4] = (u1[0x18 / 4] & ~0x1ffu) | 0x1b6u, u1[0x24 / 4] = 0;
      /* Each eye's viewport: the inset's own rows and angles, as drawn */
      for (int e = 0; e < 2; e++) {
        u32 *vp = u1 + (e ? 0x38 : 0x28) / 4;
        vp[0] = (u32)(mv_rect[e][2] - mv_rect[e][0]), vp[2] = (u32)mv_rect[e][0];
        vp[1] = (u32)(mv_rect[e][3] - mv_rect[e][1]), vp[3] = (u32)mv_rect[e][1];
        u1[(0xa0 + 8 * e) / 4] = vp[0], u1[(0xa4 + 8 * e) / 4] = vp[1];
      }
      /* agent.insetonly=1|2 (a debug tool): one eye's inset alone, the other's window put 50
       * units of tangent off to the side (nothing of it in view) */
      if (g_opt_insetonly == 1 || g_opt_insetonly == 2) {
        float *off = mv_tan[g_opt_insetonly == 1 ? 1 : 0];
        off[0] = -50.0f, off[1] = 50.1f;
      }
      /* agent.insetwin=2|3 (registered with the window's record): the layer on record 1, Hmd2's
       * window test; +0xa0..+0xac then size its lookup tables (at most theirs, 0x8a720016): the
       * whole table, so it maps its own texel centres. 2: both windows 50 of tangent off view
       * (the test fails everywhere: the tables are never sampled) */
      if (g_win_layers && g_opt_insetwin >= 2) {
        u1[0x04 / 4] = 1;
        u1[0xa0 / 4] = u1[0xa4 / 4] = u1[0xa8 / 4] = u1[0xac / 4] = LUT_N;
        if (g_opt_insetwin == 2)
          for (int e = 0; e < 2; e++)
            mv_tan[e][0] = -50.0f, mv_tan[e][1] = 50.1f;
        static u32 win_said;
        if (win_said++ < 2)
          LOG("vr2: frame %lu: the insets' layer on record 1 (window test)%s", frame,
              g_opt_insetwin == 2 ? ", both windows off view" : "");
      }
      memcpy(l1 + 0x48, mv_tan[0], 16);
      memcpy(l1 + 0x58, mv_tan[1], 16);
      u1[0x98 / 4] = 0; /* straight alpha; +0x9c..0x9f 0: reprojected with its own pose */
      param.count = 2;
      if (g_opt_insetblink) { /* (a measuring tool: the layer off every other N ms) */
        u32 off = (u32)((sceKernelGetProcessTime() / 1000 / g_opt_insetblink) & 1);
        if (off != g_blink_off)
          g_blink_at = sceKernelGetProcessTime(), g_blink_off = off;
        if (off)
          param.count = 1;
      } else {
        g_blink_off = 0;
      }
      if (mv_said++ < 2)
        LOG("vr2: frame %lu: the inset views as layer 1: %ux%u at %d,%d and %d,%d, angles %f %f "
            "%f %f and %f %f %f %f", frame, u1[0x28 / 4], u1[0x2c / 4], mv_rect[0][0],
            mv_rect[0][1], mv_rect[1][0], mv_rect[1][1], mv_tan[0][0], mv_tan[0][1],
            mv_tan[0][2], mv_tan[0][3], mv_tan[1][0], mv_tan[1][1], mv_tan[1][2], mv_tan[1][3]);
    }
    g_vr.shown_count[frame % 64] = f.gpu_count;
    trace_pass((u32)f.gpu_count, (u32)frame, (u32)b);
    fp_note_pass((u32)frame, (u32)f.gpu_count, f.tsharp[0], f.sampler);
    int r1 = sceHmd2ReprojectionSetParam((u32)b + g_buf_off, frame, &param, 0);
    int r2 = sceVideoOutSubmitFlip(g_vh, b, 2, (s64)frame);
    if (rc == 0 && r1 == 0 && r2 == 0) { /* passed: the build after it tells whether in time */
      last_vrf = (u32)frame;
      build_at_pass = gen1_last_built(&built);
      checked = 0;
      last_pass_us = sceKernelGetProcessTime(), last_pass_flip = gen1_last_flip();
      last_after_flip = last_pass_flip && last_pass_us > last_pass_flip &&
                        last_pass_us - last_pass_flip < 4000;
    }
    if ((rc || r1 || r2) && errors++ < 20)
      LOG("vr2: frame %lu: begin 0x%x, set 0x%x, flip 0x%x", frame, rc, r1, r2);
    { /* the insets' layer: Hmd2 refusing the two layers three frames in a row turns it off
       * (until cvars.txt sets it again) */
      static u32 refused;
      if (param.count == 2 && r1 != 0 && ++refused >= 3) {
        LOG("vr2: frame %lu: Hmd2 refused the second layer (0x%x) three frames running: the "
            "insets' layer off (agent.insetviews 2 -> 1)", frame, r1);
        g_opt_insetviews = 1;
        refused = 0;
      } else if (param.count == 2 && r1 == 0) {
        refused = 0;
      }
    }
  }
  LOG("vr2: %lu frames; VR mode off -> 0x%x", frame, sceHmd2ReprojectionDisableVrMode());
  /* the injector's kill waits for this (STOP_DONE in cmd_arg[3]): the hand tracker's stop and
   * finalize run on their thread in the same moment */
  sceKernelUsleep(300000);
  __atomic_store_n(&g_shared.cmd_arg[3], 0x51554954ull, __ATOMIC_RELEASE);
}

static void *vr2_thread(void *arg) {
  (void)arg;
  ps5compat_mark_thread();
  if (!g_gcn.dwords) {
    LOG("vr2: no GCN reprojection shader (the payload could not build it)");
    return 0;
  }
  hooks_log_dmem("as PS VR2 starts");
  hooks_probe_hidden_dmem("as PS VR2 starts");
  /* the reserve once the game has made its own pools and maps (made at its start, its 256 MB
   * mapping would take address space before the game's) */
  vr2_reserve_init();
  hooks_probe_hidden_dmem("with the reserve taken");
  hooks_video_queues(); /* before the PS5 libraries: see hooks.c */
  if (load_stack() != 0 || open_hmd() != 0) {
    LOG("vr2: the PS VR2 libraries could not be started");
    agent_call(CMD_NOTIFY,
               (u64) "SummerLessonVR2: the PS VR2 libraries didn't load in the game. Is kstuff "
                     "loaded?",
               0, 0, 0, 3000, 0, 0);
    return 0;
  }
  wait_headset_ready();
  if (tracker_init() != 0)
    LOG("vr2: no head tracking");
  if (output_init() != 0 || register_game_pairs() != 0) {
    LOG("vr2: the headset output could not be started");
    return 0;
  }
  /* the controller between the player's palms (hand.c, vr.c), as the PS Camera placed it */
  if (g_tracker_ok && g_opt_hands)
    hand_start();
  else
    LOG("vr2: no palm tracker (%s)", g_tracker_ok ? "agent.hands=0" : "no head tracker");
  /* agent.gaze=1 (or inset mode): eye tracking, once the palm tracker's start is over (it never
   * starts with agent.handwatch=1) */
  fov_gaze_start(g_hmd, g_tracker_ok && g_opt_hands && !g_opt_handwatch);
  game_loop();
  return 0;
}

void vr2_start(void) {
  u8 attr[64], thr[16];
  scePthreadAttrInit(attr);
  scePthreadAttrSetstacksize(attr, 512 * 1024);
  int rc = scePthreadCreate(thr, attr, vr2_thread, 0, "SummerLessonVR2");
  scePthreadAttrDestroy(attr);
  if (rc)
    LOG("agent: VR thread -> 0x%x", rc);
  else
    mirror_start();
}
