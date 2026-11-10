/* The game's controller in the test mode (test.txt, tools/testrun.sh): recorded to the log, or
 * played back from pad.txt. (The original port's trigger gadgets, adaptive triggers and rumble watch
 * were its game's play mechanics and are not in this build.)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"
#include "../common/test.h"

/* a pad sample (ScePadData, 0x78 bytes): buttons at +0x00, sticks +0x04, triggers +0x08 (L2) and
 * +0x09 (R2), the touch count at +0x34 and its first touch from +0x3c */
#define PAD_SAMPLE 0x78

static u64 now_us(void) { return sceKernelGetProcessTime(); }

/* ---------------------------------------------------------------- test mode (test.txt)
 * The console tested without a player (tools/testrun.sh). Recording: the controller goes to the
 * log as `pad:` lines (ms of the game's process time, buttons, left and right stick, L2, R2;
 * one at most every 16 ms, when something changed, the sticks and triggers in sixteenths).
 * Replaying: the same lines from pad.txt, read into g_pad_script by the injector, overwrite
 * every sample the game reads, and a sample is made up when no controller answers. */
u32 g_test;
struct pad_step {
  u32 t_ms, buttons;
  u8 lx, ly, rx, ry, l2, r2, pad[2];
};
_Static_assert(sizeof(struct pad_step) == 16, "payload/main.c fills it as 16 bytes a step");
struct pad_step g_pad_script[TEST_PAD_STEPS];
u32 g_pad_script_n;

static u8 sixteenth(u8 v) { return v >= 0xf0 ? 0xff : v & 0xf0; }

static void test_record(const u8 *d, u64 now) {
  static struct pad_step last = {0, 0, 0x80, 0x80, 0x80, 0x80, 0, 0, {0}};
  static u64 next;
  struct pad_step s = {(u32)(now / 1000), *(const u32 *)d,   sixteenth(d[4]), sixteenth(d[5]),
                       sixteenth(d[6]),    sixteenth(d[7]),   sixteenth(d[8]), sixteenth(d[9]),
                       {0}};
  if (s.buttons == last.buttons && s.lx == last.lx && s.ly == last.ly && s.rx == last.rx &&
      s.ry == last.ry && s.l2 == last.l2 && s.r2 == last.r2)
    return;
  if (now < next)
    return; /* within 16 ms of the last line: still different at the next read, logged then */
  last = s;
  next = now + 16000;
  LOG("pad: %u %08x %02x %02x %02x %02x %02x %02x", s.t_ms, s.buttons, s.lx, s.ly, s.rx, s.ry,
      s.l2, s.r2);
}

static void test_replay(u8 *d, u64 now) {
  static u32 i;
  u32 t = (u32)(now / 1000);
  while (i < g_pad_script_n && g_pad_script[i].t_ms <= t) {
    i++;
    LOG("pad: step %u of %u at %u ms", i, g_pad_script_n, t);
  }
  struct pad_step s = {0, 0, 0x80, 0x80, 0x80, 0x80, 0, 0, {0}}; /* before the script: still */
  if (i)
    s = g_pad_script[i - 1];
  *(u32 *)d = s.buttons;
  d[4] = s.lx, d[5] = s.ly, d[6] = s.rx, d[7] = s.ry, d[8] = s.l2, d[9] = s.r2;
}

/* Every read of the game's controller (hooks.c): recorded, or replaced by the script. Returns
 * the samples there are. */
s32 pads_test_read(s32 handle, u8 *data, s32 num, s32 n) {
  (void)handle;
  if (!data || !(g_test & (TEST_RECORD | TEST_REPLAY)))
    return n;
  u64 now = now_us();
  if (g_test & TEST_RECORD) {
    if (n > 0)
      test_record(data + (u64)(n - 1) * PAD_SAMPLE, now);
    return n;
  }
  if (num >= 1) {
    /* The sample is made up whether a controller answers or not: connected, held level and
     * still, the script's buttons and sticks. (With a real one, left lying any way on the desk,
     * its orientation went into the samples and the game's world select, which goes by where
     * the controller points, never took the script's presses: three replays stuck there.) */
    static u32 said;
    if (!said++)
      LOG("pad: the controller made up for the script (%d samples answered)", n);
    memset(data, 0, PAD_SAMPLE);
    *(float *)(data + 0x18) = 1.0f; /* the orientation quaternion's w */
    data[0x4c] = 1;                 /* connected */
    *(u64 *)(data + 0x50) = now;    /* its timestamp */
    data[0x68] = 1;                 /* connectedCount */
    n = 1;
  }
  for (s32 k = 0; k < n; k++)
    test_replay(data + (u64)k * PAD_SAMPLE, now);
  return n;
}
