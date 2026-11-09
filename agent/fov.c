/* PS VR2's eye tracking, read from Hmd2 (inset mode's windows follow it: multiview.c inset_gaze).
 * agent.gaze=1 (read at the start; inset mode turns it on, fov_preset_gaze): eye tracking started
 * (sceHmd2InternalGazeStart) once the palm tracker's start is over and the headset has then been
 * worn with no system screen for 5 s (the palm tracker's rule, hand.c), then read
 * (sceHmd2GazeGetResult) every 5 ms; in a debug build the first 2 minutes logged twice a second,
 * then a summary every 10 s. (The foveated passes once here, docs/FOVEATION.md, were withdrawn:
 * multi-view replaced them.)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

/* agent.gaze: 1 on; preset: inset mode's (fov_preset_gaze), which holds only while inset mode
 * does (the payload turns it off, g_multiview_rows 0, when the game's bytes aren't Hikari 1.00's) */
static volatile u32 g_gaze_opt, g_gaze_preset;

/* cvars.txt's agent.gaze line at the start (hooks.c cvars_fetch): 1 if it was that line */
int fov_option(const char *key, u32 klen, const char *val, u32 vlen) {
  if (klen != 4 || memcmp(key, "gaze", 4) != 0)
    return 0;
#ifdef RELEASE
  (void)val, (void)vlen;
  LOG("cvars: agent.gaze is a debug option: not in a release build");
  return 1;
#else
  u32 x = 0;
  for (u32 i = 0; i < vlen && val[i] >= '0' && val[i] <= '9'; i++)
    x = x * 10 + (u32)(val[i] - '0');
  g_gaze_opt = x != 0, g_gaze_preset = 0;
  LOG("fov: agent.gaze = %u", g_gaze_opt);
  return 1;
#endif
}

int fov_wants_gaze(void) { return g_gaze_opt && (!g_gaze_preset || g_multiview_rows); }
/* inset mode's default (multiview.c, at the boot: agent.gaze in cvars.txt, read after, still
 * sets it) */
void fov_preset_gaze(void) { g_gaze_opt = 1, g_gaze_preset = 1; }

/* ---- the gaze ----
 * sceHmd2GazeGetResult (libSceHmd2 0x35110 on 12.40): param 0x10 bytes (size, the Hmd2 handle at
 * +4), result 0x38 bytes (size). PS5VR's reading: +4 status (0 ok), +0x10 1 = valid, +0x14 a unit
 * direction in headset space, -z forward; +8 looks like the sample's time. Hmd2 opens the gaze
 * device (/dev/hmd2_gaze) in its own start-up; sceHmd2InternalGazeStart (0x31760) is an ioctl on
 * its main device that starts the samples coming (it returns 0 whatever the ioctl says), without
 * which every result is empty. The newest valid direction is kept as tangents (x right, y up),
 * packed in one u64 so a reader never sees half of one, with the time a new sample came: one that
 * stops changing goes stale, and the window is dropped. */
static int g_hmd = -1, g_after_hands;
static volatile u64 g_gaze_tan; /* two floats: tan x, tan y */
static volatile u64 g_gaze_at;  /* when the newest new valid sample was read (process time, us) */
#define GAZE_SETTLE 5000000 /* us: as the palm tracker, counted once its start is over */
#define GAZE_LOST 300000     /* us: no new valid sample for this long (more than a blink): lost */

static int gaze_read(u8 r[0x38]) {
  u8 param[0x10];
  memset(param, 0, sizeof param);
  *(u32 *)param = sizeof param;
  *(s32 *)(param + 4) = g_hmd;
  memset(r, 0, 0x38);
  *(u32 *)r = 0x38;
  return sceHmd2GazeGetResult(param, r);
}

/* One result read: 1 and its tangents (x right, y up) when it is a new valid sample. New: its
 * time (+8) or its direction differs from the last one's (in case +8 is not a time); a direction
 * is a unit vector forward: no NaN, nothing past about 84 degrees (*odd counts those). */
struct gaze_last {
  u64 stamp;
  float dir[3];
};
static int gaze_accept(int rc, const u8 *r, struct gaze_last *last, float t[2], u32 *fresh, u32 *odd) {
  const float *d = (const float *)(r + 0x14);
  u64 stamp = *(const u64 *)(r + 8);
  int is_new = stamp != last->stamp || memcmp(d, last->dir, sizeof last->dir) != 0;
  last->stamp = stamp;
  memcpy(last->dir, d, sizeof last->dir);
  if (!is_new)
    return 0;
  (*fresh)++;
  if (rc != 0 || *(const s32 *)(r + 4) != 0 || *(const u32 *)(r + 0x10) != 1 || !(d[2] < -0.05f))
    return 0;
  t[0] = d[0] / -d[2], t[1] = d[1] / -d[2];
  if (!(t[0] > -10 && t[0] < 10 && t[1] > -10 && t[1] < 10)) { /* (false for a NaN too) */
    (*odd)++;
    return 0;
  }
  return 1;
}

static void *gaze_thread(void *arg) {
  (void)arg;
  ps5compat_mark_thread();
  if (!sceHmd2GazeGetResult || !sceHmd2InternalGazeStart) {
    LOG("fov: Hmd2's gaze calls not found: no eye tracking");
    return 0;
  }
  while (gen1_refreshes() < 240 && g_shared.cmd != CMD_QUIT)
    sceKernelUsleep(100000);
  u8 r[0x38];
  int rc = gaze_read(r);
  LOG("fov: gaze before the start: rc 0x%x, status %d, valid %u (expected: not valid)", rc,
      *(s32 *)(r + 4), *(u32 *)(r + 0x10));
  /* after the palm tracker's start (its cameras and credentials), never beside it */
  while (g_after_hands && !g_hand_settled && g_shared.cmd != CMD_QUIT)
    sceKernelUsleep(250000);
  u64 since = 0;
  while (g_shared.cmd != CMD_QUIT) {
    u64 now = sceKernelGetProcessTime();
    u8 st[512];
    int src;
    if (hand_clear_why(st, &src) != 0)
      since = 0;
    else if (!since)
      since = now;
    else if (now - since >= GAZE_SETTLE)
      break;
    sceKernelUsleep(250000);
  }
  if (g_shared.cmd == CMD_QUIT)
    return 0;
  rc = sceHmd2InternalGazeStart();
  LOG("fov: eye tracking started (the palm tracker's start over, worn, no system screen for 5 s) "
      "-> 0x%x", rc);
  struct gaze_last last;
  memset(&last, 0, sizeof last);
  u32 lines = 0, n = 0, valid = 0, stamps = 0, odd = 0, raw_rc = 0;
  u64 next_line = 0, next_sum = sceKernelGetProcessTime() + 10000000,
      last_valid = sceKernelGetProcessTime();
  while (g_shared.cmd != CMD_QUIT) {
    rc = gaze_read(r);
    u64 now = sceKernelGetProcessTime();
    s32 status = *(s32 *)(r + 4);
    u64 stamp = *(u64 *)(r + 8);
    const float *d = (const float *)(r + 0x14);
    n++;
    if (rc)
      raw_rc = (u32)rc;
    union { float f[2]; u64 u; } t;
    if (gaze_accept(rc, r, &last, t.f, &stamps, &odd)) {
      valid++;
      last_valid = now;
      __atomic_store_n(&g_gaze_tan, t.u, __ATOMIC_RELEASE);
      __atomic_store_n(&g_gaze_at, now, __ATOMIC_RELEASE);
    }
    if (lines < 240 && now >= next_line) { /* the first 2 minutes, raw: what each field does */
      next_line = now + 500000;
      lines++;
      const float *f = (const float *)r;
      LOG_DETAIL("fov: gaze rc 0x%x status %d time %lu valid %u dir %f %f %f; +0x20 %f %f %f %f, +0x30 %lx",
          rc, status, stamp, *(u32 *)(r + 0x10), d[0], d[1], d[2], f[8], f[9], f[10], f[11],
          *(u64 *)(r + 0x30));
    }
    if (now >= next_sum) {
      next_sum = now + 10000000;
      t.u = __atomic_load_n(&g_gaze_tan, __ATOMIC_ACQUIRE);
      LOG_DETAIL("fov: gaze 10 s: %u reads, %u new samples, %u valid, %u out of range, last error "
          "0x%x; at %f %f (tan)", n, stamps, valid, odd, raw_rc, t.f[0], t.f[1]);
      n = valid = stamps = odd = raw_rc = 0;
    }
    /* every 5 ms; every 100 ms after 30 s with nothing valid (eye tracking off in the settings) */
    sceKernelUsleep(now - last_valid > 30000000 ? 100000 : 5000);
  }
  if (sceHmd2InternalGazeStop)
    LOG("fov: eye tracking stop -> 0x%x", sceHmd2InternalGazeStop());
  return 0;
}

/* the newest valid gaze as tangents (x right, y up), 1 while it is fresh (multiview.c's windows,
 * agent.gazedot in inset.c) */
int fov_gaze(float t[2]) {
  u64 at = __atomic_load_n(&g_gaze_at, __ATOMIC_ACQUIRE);
  if (!at || sceKernelGetProcessTime() - at > GAZE_LOST)
    return 0;
  union { float f[2]; u64 u; } g;
  g.u = __atomic_load_n(&g_gaze_tan, __ATOMIC_ACQUIRE);
  t[0] = g.f[0], t[1] = g.f[1];
  return 1;
}

void fov_gaze_start(int hmd, int after_hands) {
  if (!fov_wants_gaze()) /* (a release build: only for inset mode, fov_preset_gaze) */
    return;
  g_hmd = hmd, g_after_hands = after_hands;
  u8 attr[64], thr[16];
  scePthreadAttrInit(attr);
  scePthreadAttrSetstacksize(attr, 64 * 1024);
  int rc = scePthreadCreate(thr, attr, gaze_thread, 0, "SummerLessonVR2 gaze");
  scePthreadAttrDestroy(attr);
  if (rc)
    LOG("fov: gaze thread -> 0x%x", rc);
}
