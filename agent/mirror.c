/* The TV: what the headset shows, the way PS VR's reprojection put it on the main video output.
 *
 * With PS VR the reprojection drew the headset picture into the game's main port and flipped it,
 * and the processor unit showed it on the TV. With PS VR2 that port still goes to the TV, but
 * nothing flips it, so the TV kept the game's last 2D picture. Here every frame the game hands
 * over is cropped from its left eye texture into display buffers of our own on that port (a
 * small GCN compute dispatch on Hmd2's GPU queue, mirror.s) and flipped there.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"
#include "mirror_gcn.h"

#define NBUF 3
#define FIRST_INDEX 4     /* the game registers buffers 0-3 on its main port */
#define HALF_TAN_X 0.85f  /* the crop's horizontal field of view: 2 atan(0.85) = 80 degrees */

struct consts {
  u32 tsharp[8];
  u32 sampler[4];
  u32 vsharp[4];
  float u0, du, v0, dv;
  u32 pitch, pad[3];
};

static struct {
  volatile s32 port; /* the game's main port */
  u32 attr[10];      /* PS4 SceVideoOutBufferAttribute for our buffers */
  u32 w, h, pitch;   /* pitch in pixels */
  u8 *mem;           /* shader, constants, command buffer, label */
  u8 *buf[NBUF];
} g_m = {.port = -1};

#ifndef RELEASE
/* agent.capture=N (a debug tool, live): each new N, the next picture on the TV, 320x180 (every
 * sixth pixel of every sixth row), into the log as "capture N <line> <base64>" lines a few a
 * frame, the bytes as the display buffer holds them (B, G, R of A8R8G8B8): tools/fov/capture.py
 * makes a PNG of it. The copy is a few ms right after a flip (the TV keeps flipping on time);
 * the log lines follow over a couple of seconds. */
#define CAP_W 320
#define CAP_H 360 /* at most: 180 rows of the whole TV, or 360 of its left half (zoom) */
#define CAP_LINE 225 /* bytes a line: 300 characters of base64 */
volatile u32 g_opt_capture;
static struct {
  u32 id, pos, len; /* the capture being logged, its next byte, its size (0: none) */
  u32 req;          /* the agent.capture value it answers */
  u32 base, queued; /* agent.capafter=1000: where in px the one being logged starts, how many follow */
  u8 px[CAP_W * CAP_H * 3];
} g_cap;

static void cap_b64(char *out, const u8 *in, u32 n) {
  static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  u32 o = 0;
  for (u32 i = 0; i < n; i += 3) {
    u32 v = (u32)in[i] << 16 | (i + 1 < n ? (u32)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out[o++] = k[v >> 18], out[o++] = k[(v >> 12) & 63];
    out[o++] = i + 1 < n ? k[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? k[v & 63] : '=';
  }
  out[o] = 0;
}

/* agent.probe=1 (a debug tool, live, with agent.mirrorboth=1, agent.tvhmd=0 and multi-view): four
 * times a second, each eye's brightness where its inset looks against its inset's, from the TV's
 * picture of the whole texture: the same directions, the inset's 40-texel fade edge left out (its
 * inner 70%), red, green, blue and luma means and the inset's ratio to the eye's. A repeatable
 * measure of the brightness step at the inset's edge (tools/fov/probe.py sums a run up). The
 * frame's own window (at: its pose), any texture size (tw, th: the frame's T#). */
static void probe_step(const u8 *buf, u32 tw, u32 th, u64 at) {
  static u32 n;
  if (!g_opt_probe || g_opt_probe == 3 || g_opt_mirrorboth != 1 || g_opt_tvhmd || !g_multiview_rows || n++ % 15 || !tw || !th)
    return;
  s32 axis[2][2], ir[2][4];
  float ppt[4], tan[2][4], ctan[2][2];
  if (!multiview_layer(ir, tan, at))
    return;
  multiview_axis(axis, ppt, at);
  multiview_centres(ctan, at);
  if (!(ppt[0] > 1.0f && ppt[1] > 1.0f && ppt[2] > 1.0f && ppt[3] > 1.0f))
    return;
  float m[2][2][4]; /* eye, (periphery, inset), b g r luma */
  for (int e = 0; e < 2; e++) {
    float hxi = 0.35f * (float)(ir[e][2] - ir[e][0]), hyi = 0.35f * (float)(ir[e][3] - ir[e][1]);
    float cxi = 0.5f * (float)(ir[e][0] + ir[e][2]), cyi = 0.5f * (float)(ir[e][1] + ir[e][3]);
    /* the same directions in the eye: its window's centre, the half-sizes at the eye's density */
    float cxp = (float)axis[e][0] + ctan[e][0] * ppt[0], cyp = (float)axis[e][1] - ctan[e][1] * ppt[1];
    float hxp = hxi / ppt[2] * ppt[0], hyp = hyi / ppt[3] * ppt[1];
    for (int w = 0; w < 2; w++) {
      float cx = w ? cxi : cxp, cy = w ? cyi : cyp, hx = w ? hxi : hxp, hy = w ? hyi : hyp;
      u32 s[3] = {0, 0, 0}, c = 0;
      for (u32 j = 0; j < 24; j++)
        for (u32 i = 0; i < 24; i++) {
          float fx = cx + hx * ((float)(2 * i + 1) / 24.0f - 1.0f);
          float fy = cy + hy * ((float)(2 * j + 1) / 24.0f - 1.0f);
          if (fx < 0.0f || fy < 0.0f || fx >= (float)tw || fy >= (float)th)
            continue;
          u32 x = (u32)(fx * (float)g_m.w / (float)tw), y = (u32)(fy * (float)g_m.h / (float)th);
          u32 v = *(const volatile u32 *)(buf + ((u64)y * g_m.pitch + x) * 4);
          s[0] += v & 0xff, s[1] += (v >> 8) & 0xff, s[2] += (v >> 16) & 0xff, c++;
        }
      for (int k = 0; k < 3; k++)
        m[e][w][k] = c ? (float)s[k] / (float)c : 0.0f;
      m[e][w][3] = 0.0722f * m[e][w][0] + 0.7152f * m[e][w][1] + 0.2126f * m[e][w][2];
    }
  }
  /* agent.probe=2: also each inset's four edge bands (from 70% of its size out to its fade edge,
   * 40 texels in: 0.35-0.46 of its width or height from the middle, a 24x6 grid each) against
   * the eye at the same directions: where a light outside the inset showed as a cutout of its
   * glow (docs/MULTIVIEW.md 22). Luma ratios: left, right, top, bottom. */
  if (g_opt_probe == 2) {
    float rb[2][4];
    for (int e = 0; e < 2; e++) {
      float wi = (float)(ir[e][2] - ir[e][0]), hi = (float)(ir[e][3] - ir[e][1]);
      float cxi = 0.5f * (float)(ir[e][0] + ir[e][2]), cyi = 0.5f * (float)(ir[e][1] + ir[e][3]);
      float cxp = (float)axis[e][0] + ctan[e][0] * ppt[0], cyp = (float)axis[e][1] - ctan[e][1] * ppt[1];
      float sx = ppt[0] / ppt[2], sy = ppt[1] / ppt[3]; /* inset texels to the eye's */
      for (int b = 0; b < 4; b++) {
        float lum[2];
        for (int w = 0; w < 2; w++) {
          u32 s3[3] = {0, 0, 0}, c = 0;
          for (u32 j = 0; j < 6; j++)
            for (u32 i = 0; i < 24; i++) {
              float a = 0.35f + 0.11f * ((float)j + 0.5f) / 6.0f, t = ((float)(2 * i + 1) / 24.0f - 1.0f) * 0.46f;
              float ox = b < 2 ? (b ? a : -a) * wi : t * wi, oy = b < 2 ? t * hi : (b == 2 ? -a : a) * hi;
              float fx = w ? cxi + ox : cxp + ox * sx, fy = w ? cyi + oy : cyp + oy * sy;
              if (fx < 0.0f || fy < 0.0f || fx >= (float)tw || fy >= (float)th)
                continue;
              u32 x = (u32)(fx * (float)g_m.w / (float)tw), y = (u32)(fy * (float)g_m.h / (float)th);
              u32 v = *(const volatile u32 *)(buf + ((u64)y * g_m.pitch + x) * 4);
              s3[0] += v & 0xff, s3[1] += (v >> 8) & 0xff, s3[2] += (v >> 16) & 0xff, c++;
            }
          lum[w] = c ? (0.0722f * (float)s3[0] + 0.7152f * (float)s3[1] + 0.2126f * (float)s3[2]) / (float)c : 0.0f;
        }
        rb[e][b] = lum[0] > 0.5f ? lum[1] / lum[0] : 0.0f;
      }
    }
    LOG("probe2: insetpp %u edges inset/eye L %.3f %.3f %.3f %.3f, R %.3f %.3f %.3f %.3f (left right "
        "top bottom)", g_opt_insetpp, (double)rb[0][0], (double)rb[0][1], (double)rb[0][2], (double)rb[0][3], (double)rb[1][0],
        (double)rb[1][1], (double)rb[1][2], (double)rb[1][3]);
  }
#define PR(e, k) (m[e][0][k] > 0.5f ? m[e][1][k] / m[e][0][k] : 0.0f)
  LOG("probe: inset/eye L r %.3f g %.3f b %.3f luma %.3f (eye %.1f), R r %.3f g %.3f b %.3f luma "
      "%.3f (eye %.1f)", (double)PR(0, 2), (double)PR(0, 1), (double)PR(0, 0), (double)PR(0, 3),
      (double)m[0][0][3], (double)PR(1, 2), (double)PR(1, 1), (double)PR(1, 0), (double)PR(1, 3),
      (double)m[1][0][3]);
#undef PR
}

/* agent.probe=3 (with agent.mirrorboth=1, agent.tvhmd=0): each eye's change from one picture to the
 * next, every picture: a 48x48 grid over each eye (the rows above the insets), the mean absolute
 * difference of its luma from the last picture's at the same place, and of the grid's bright
 * samples (luma 200 or more: the lights and their glow); logged once a second (60 pictures) with
 * each eye's mean luma. A flicker in one eye (the 1.02 cafe's right-eye bloom, to-fix) shows as
 * that eye's change well above the other's with the head held. */
#define FL_N 48
static void flicker_step(const u8 *buf, u32 th) {
  static u8 last[2][FL_N * FL_N];
  static u32 have, n;
  static float sd[2], sb[2], sl[2], mx[2];
  static u32 nb[2];
  if (g_opt_probe != 3 || g_opt_mirrorboth != 1 || g_opt_tvhmd || !th || g_multiview_rows >= th) {
    if (have) { /* (a second begun before the probe went off not carried into the next one) */
      have = 0, n = 0;
      for (int e = 0; e < 2; e++)
        sd[e] = sb[e] = sl[e] = mx[e] = 0.0f, nb[e] = 0;
    }
    return;
  }
  u32 eh = (u32)((u64)g_m.h * (th - g_multiview_rows) / th), ew = g_m.w / 2;
  static u32 at_eh, at_ew, at_pitch, at_th; /* (the grid moved, or the eye texture's size, with which the
                                             * TV's crop of it changes: the next picture its first again) */
  if (eh != at_eh || ew != at_ew || g_m.pitch != at_pitch || th != at_th) {
    at_eh = eh, at_ew = ew, at_pitch = g_m.pitch, at_th = th;
    have = 0, n = 0;
    for (int e = 0; e < 2; e++)
      sd[e] = sb[e] = sl[e] = mx[e] = 0.0f, nb[e] = 0;
  }
  for (int e = 0; e < 2; e++) {
    float d = 0.0f, l = 0.0f, b = 0.0f;
    u32 c = 0;
    for (u32 j = 0; j < FL_N; j++)
      for (u32 i = 0; i < FL_N; i++) {
        u32 x = (u32)e * ew + (2 * i + 1) * ew / (2 * FL_N), y = (2 * j + 1) * eh / (2 * FL_N);
        u32 v = *(const volatile u32 *)(buf + ((u64)y * g_m.pitch + x) * 4);
        u32 lu = (18 * (v & 0xff) + 183 * ((v >> 8) & 0xff) + 54 * ((v >> 16) & 0xff)) >> 8;
        u8 *p = &last[e][j * FL_N + i];
        if (have) {
          float a = (float)(lu > *p ? lu - *p : *p - lu);
          d += a;
          if (lu >= 200 || *p >= 200)
            b += a, c++;
        }
        l += (float)lu;
        *p = (u8)lu;
      }
    if (have) {
      d /= (float)(FL_N * FL_N);
      sd[e] += d, sb[e] += b, nb[e] += c, sl[e] += l / (float)(FL_N * FL_N);
      if (d > mx[e])
        mx[e] = d;
    }
  }
  if (!have) {
    have = 1;
    return;
  }
  if (++n < 60)
    return;
  LOG("flicker: picture-to-picture change L %.2f (at most %.2f, bright %.1f of %u), R %.2f (at most %.2f, "
      "bright %.1f of %u); luma L %.1f R %.1f",
      (double)(sd[0] / (float)n), (double)mx[0], (double)(nb[0] ? sb[0] / (float)nb[0] : 0.0f), nb[0] / n,
      (double)(sd[1] / (float)n), (double)mx[1], (double)(nb[1] ? sb[1] / (float)nb[1] : 0.0f), nb[1] / n,
      (double)(sl[0] / (float)n), (double)(sl[1] / (float)n));
  n = 0;
  for (int e = 0; e < 2; e++)
    sd[e] = sb[e] = sl[e] = mx[e] = 0.0f, nb[e] = 0;
}

/* agent.probe=1 with agent.tvhmd=1: the headset's own picture (Hmd2's composition, as the TV
 * shows it) at the middle of each eye, where the insets cover it with the window centred
 * (agent.insetgaze=0; at 1440 the insets sat around TV 531,540 and 1386,540, captures mv9y-t1440),
 * four times a second, with whether the newest frame went with the insets' layer
 * (agent.insetblink) and how long since that changed: the brightness the layer itself makes on
 * the headset (tools/fov/probe.py, the probeh lines) */
static void probe_hmd(const u8 *buf) {
  static u32 n;
  if (!g_opt_probe || g_opt_probe == 3 || !g_opt_tvhmd || !g_multiview_rows || n++ % 15)
    return;
  static const float k_cx[2] = {531.0f / 1920.0f, 1386.0f / 1920.0f};
  float m[2][4];
  s32 h = (s32)(40.0f * (float)g_m.w / 1920.0f);
  for (int e = 0; e < 2; e++) {
    s32 cx = (s32)(k_cx[e] * (float)g_m.w), cy = (s32)(g_m.h / 2);
    u32 s[3] = {0, 0, 0}, c = 0;
    for (s32 y = cy - h; y <= cy + h; y += 2)
      for (s32 x = cx - h; x <= cx + h; x += 2) {
        if (x < 0 || y < 0 || (u32)x >= g_m.w || (u32)y >= g_m.h)
          continue;
        u32 v = *(const volatile u32 *)(buf + ((u64)y * g_m.pitch + (u32)x) * 4);
        s[0] += v & 0xff, s[1] += (v >> 8) & 0xff, s[2] += (v >> 16) & 0xff, c++;
      }
    for (int k = 0; k < 3; k++)
      m[e][k] = c ? (float)s[k] / (float)c : 0.0f;
    m[e][3] = 0.0722f * m[e][0] + 0.7152f * m[e][1] + 0.2126f * m[e][2];
  }
  u64 since = (sceKernelGetProcessTime() - g_blink_at) / 1000;
  LOG("probeh: layer %s (%lu ms since) L r %.2f g %.2f b %.2f luma %.2f, R r %.2f g %.2f b %.2f luma %.2f",
      g_blink_off ? "off" : "on", since, (double)m[0][2], (double)m[0][1], (double)m[0][0],
      (double)m[0][3], (double)m[1][2], (double)m[1][1], (double)m[1][0], (double)m[1][3]);
}

/* after a flip of `buf` (its picture finished): a new capture taken, and some lines logged */
static s32 g_mirror_dy; /* agent.mirrorboth=2: the texture's row on the TV's row 0 */
volatile u32 g_opt_capafter; /* agent.capafter=D (a debug tool, live): each capture held until D pictures
                              * after agent.insetgaze=11's look comes back (the pop-in, docs/MULTIVIEW.md 30);
                              * 1000: a burst, BURST_N crops of BURST_W x BURST_H of the left inset 1:1 (with
                              * agent.mirrorboth=2) from one look back, at k_burst pictures after it, logged as
                              * captures N, N + 1, ... (one look back: the scene's own motion left out) */
#define BURST_N 6
#define BURST_W 128
#define BURST_H 144
_Static_assert(BURST_N * BURST_W * BURST_H <= CAP_W * CAP_H, "the burst in the capture's buffer");
/* 1, 2, 3, 10, 30 pictures after the look back (the frame-to-frame steps the eye sees, from 2: the
 * TV shows the window back only from the second picture, the first is still the look away's, so
 * leave crop 0 out of the steps), 135 just before the next look away (at 150: the settled picture) */
static const u32 k_burst[BURST_N] = {1, 2, 3, 10, 30, 135};
volatile s32 g_opt_capdx, g_opt_capdy; /* agent.capdx/capdy: the burst's crop centred this far from the inset's centre
                                        * (1:1 pixels, right and down; the cafe's "火" card at -76, -78), live */
static void capture_step(const u8 *buf) {
  u32 want = g_opt_capture;
  int ready = 1;
  if (g_opt_capafter && !g_cap.len && want && want != g_cap.req) {
    /* (a new N waits for the next look back, then counts this thread's flips, one a picture) */
    static u32 wait_id, ret0, flips, counting, got;
    static float c0[2];
    u32 ret = __atomic_load_n(&g_gaze_returns, __ATOMIC_ACQUIRE);
    if (wait_id != want)
      wait_id = want, ret0 = ret, counting = 0, got = 0;
    if (counting && ret != ret0 && g_opt_capafter >= 1000 && g_opt_capafter < 2000 && got < BURST_N) {
      /* (another look back before the burst was whole, frames dropped: from this one again) */
      LOG("capture %u: another look back after %u of the burst's crops: again from it", want, got);
      got = 0, counting = 0;
    }
    if (!counting && (ret != ret0 || g_opt_capafter >= 2000))
      counting = 1, flips = 0, ret0 = ret; /* (2000: the burst from this request on, no look back waited for) */
    if (counting)
      flips++;
    if (g_opt_capafter >= 1000) { /* the burst */
      ready = 0;
      s32 rect[2][4];
      float tan[2][4];
      /* (2000: BURST_N pictures in a row, for what changes from one to the next: the TAA's shimmer) */
      u32 at = g_opt_capafter >= 2000 ? got + 1 : k_burst[got];
      if (counting && got < BURST_N && flips >= at && g_opt_mirrorboth == 2 && multiview_layer(rect, tan, 0)) {
        /* (the first flip at or past each crop's picture; the window where the first crop had it,
         * else the burst again from the next look back: a crop taken with the window away again
         * is no settled picture) */
        float ct[2] = {(tan[0][1] - tan[0][0]) * 0.5f, (tan[0][2] - tan[0][3]) * 0.5f};
        if (!got)
          c0[0] = ct[0], c0[1] = ct[1];
        else if (__builtin_fabsf(ct[0] - c0[0]) > 0.01f || __builtin_fabsf(ct[1] - c0[1]) > 0.01f) {
          LOG("capture %u: the window moved after %u crops (%.4f %.4f from %.4f %.4f): again from the next look back",
              want, got, (double)ct[0], (double)ct[1], (double)c0[0], (double)c0[1]);
          got = 0, counting = 0;
          goto burst_done;
        }
        s32 cx = (rect[0][0] + rect[0][2]) / 2 + g_opt_capdx - BURST_W / 2,
            cy = (rect[0][1] + rect[0][3]) / 2 - g_mirror_dy + g_opt_capdy - BURST_H / 2;
        cx = cx < 0 ? 0 : cx > (s32)(g_m.w - BURST_W) ? (s32)(g_m.w - BURST_W) : cx;
        cy = cy < 0 ? 0 : cy > (s32)(g_m.h - BURST_H) ? (s32)(g_m.h - BURST_H) : cy;
        for (u32 y = 0; y < BURST_H; y++) {
          const u8 *row = buf + (u64)((u32)cy + y) * g_m.pitch * 4 + (u64)cx * 4;
          u8 *o = g_cap.px + (got * BURST_H + y) * BURST_W * 3;
          for (u32 x = 0; x < BURST_W; x++, o += 3) {
            u32 v = *(const volatile u32 *)(row + (u64)x * 4);
            o[0] = (u8)v, o[1] = (u8)(v >> 8), o[2] = (u8)(v >> 16);
          }
        }
        LOG("capture %u: crop %u %u pictures after the look back, inset 0's window centre %.5f %.5f, from %d,%d",
            want + got, got, flips, (double)((tan[0][1] - tan[0][0]) * 0.5f),
            (double)((tan[0][2] - tan[0][3]) * 0.5f), cx, cy);
        got++;
      }
    burst_done:
      if (got == BURST_N) {
        g_cap.req = want, g_cap.id = want, g_cap.base = 0, g_cap.pos = 0, g_cap.len = BURST_W * BURST_H * 3;
        g_cap.queued = BURST_N - 1;
        LOG("capture %u begin %ux%u bgr (a burst's crop)", g_cap.id, BURST_W, BURST_H);
      }
    } else {
      ready = counting && flips >= g_opt_capafter;
      if (ready)
        LOG("capture %u: %u pictures after the look back", want, flips);
    }
  }
  if (ready && !g_cap.len && want && want != g_cap.req && g_m.w >= 2 * CAP_W && g_m.h >= CAP_H) {
    /* N of 100 or more: the TV's left half (the left eye) at every third pixel, 320x360; of
     * 200 or more, its right half. Of 300 or more: 320x360 of the TV's pixels as they are,
     * around the left inset's centre with agent.mirrorboth=2 (the texture 1:1), of 400 or more
     * the right inset's (else the TV's centre) */
    u32 zoom = want >= 100, ch = zoom ? CAP_H : CAP_H / 2, x0 = want >= 200 ? g_m.w / 2 : 0, y0 = 0;
    u32 sx = zoom ? g_m.w / 2 / CAP_W : g_m.w / CAP_W, sy = g_m.h / ch;
    if (want >= 300) {
      s32 cx = (s32)g_m.w / 2, cy = (s32)g_m.h / 2, rect[2][4];
      float tan[2][4];
      int k = want >= 400;
      if (g_opt_mirrorboth == 2 && multiview_layer(rect, tan, 0)) {
        cx = (rect[k][0] + rect[k][2]) / 2, cy = (rect[k][1] + rect[k][3]) / 2 - g_mirror_dy;
        /* (the newest window's centre, tangent from the eye's axis: to register captures of a
         * moving window against each other, docs/MULTIVIEW.md 30) */
        LOG("capture %u: inset %d's window centre %.5f %.5f (the newest made)", want, k,
            (double)((tan[k][1] - tan[k][0]) * 0.5f), (double)((tan[k][2] - tan[k][3]) * 0.5f));
      }
      cx -= CAP_W / 2, cy -= CAP_H / 2;
      cx = cx < 0 ? 0 : cx > (s32)(g_m.w - CAP_W) ? (s32)(g_m.w - CAP_W) : cx;
      cy = cy < 0 ? 0 : cy > (s32)(g_m.h - CAP_H) ? (s32)(g_m.h - CAP_H) : cy;
      x0 = (u32)cx, y0 = (u32)cy, sx = sy = 1;
    }
    for (u32 y = 0; y < ch; y++) {
      const u8 *row = buf + (u64)(y0 + y * sy) * g_m.pitch * 4 + (u64)x0 * 4;
      u8 *o = g_cap.px + y * CAP_W * 3;
      for (u32 x = 0; x < CAP_W; x++, o += 3) {
        u32 v = *(const volatile u32 *)(row + (u64)(x * sx) * 4);
        o[0] = (u8)v, o[1] = (u8)(v >> 8), o[2] = (u8)(v >> 16);
      }
    }
    g_cap.req = g_cap.id = want, g_cap.pos = 0, g_cap.len = CAP_W * ch * 3, g_cap.base = 0, g_cap.queued = 0;
    LOG("capture %u begin %ux%u bgr (the TV's %ux%u%s, every %u,%u, from %u,%u)", want, CAP_W, ch,
        g_m.w, g_m.h, want >= 300 ? ", its pixels" : x0 ? ", its right half" : zoom ? ", its left half" : "",
        sx, sy, x0, y0);
  }
  for (int i = 0; i < 8 && g_cap.len; i++) {
    char line[CAP_LINE / 3 * 4 + 1];
    u32 n = g_cap.len - g_cap.pos < CAP_LINE ? g_cap.len - g_cap.pos : CAP_LINE;
    cap_b64(line, g_cap.px + g_cap.base + g_cap.pos, n);
    LOG("capture %u %u %s", g_cap.id, g_cap.pos / CAP_LINE, line);
    g_cap.pos += n;
    if (g_cap.pos >= g_cap.len) {
      LOG("capture %u end", g_cap.id);
      if (g_cap.queued) { /* (the burst's next crop) */
        g_cap.queued--, g_cap.base += g_cap.len, g_cap.id++, g_cap.pos = 0;
        LOG("capture %u begin %ux%u bgr (a burst's crop)", g_cap.id, BURST_W, BURST_H);
      } else
        g_cap.len = 0;
    }
  }
}
#endif

void mirror_note_port(s32 port, const void *attr) {
  const u32 *a = attr;
  if (port < 0 || !a || g_m.port >= 0)
    return;
  memcpy(g_m.attr, a, sizeof g_m.attr);
  g_m.attr[1] = 1;               /* linear */
  g_m.attr[6] = 0;               /* no options */
  g_m.attr[5] = (a[3] + 63) & ~63u; /* pitch */
  g_m.w = a[3], g_m.h = a[4], g_m.pitch = g_m.attr[5];
  __atomic_store_n(&g_m.port, port, __ATOMIC_RELEASE);
  LOG("mirror: main port 0x%x, %ux%u format 0x%x", port, g_m.w, g_m.h, a[0]);
}

/* ---------------------------------------------------------------- GPU */
static u32 pm4(u32 op, u32 body) { return 0xc0000000u | ((body - 1) << 16) | (op << 8); }

/* compute command buffer: one dispatch of 8x8 thread groups, one pixel each, then the label
 * written once its stores are in memory */
static u32 build_acb(u32 *cb, u64 shader, u64 consts, u64 label, u32 seq, u32 w, u32 h) {
  u32 n = 0;
  /* ACQUIRE_MEM (gfx7 compute form): the constants were written by the CPU */
  cb[n++] = pm4(0x58, 6);
  cb[n++] = (1u << 22) | (1u << 23) | (1u << 27) | (1u << 29);
  cb[n++] = 0xffffffffu, cb[n++] = 0xff, cb[n++] = 0, cb[n++] = 0, cb[n++] = 0x0a;
  cb[n++] = pm4(0x76, 4), cb[n++] = 0x204, cb[n++] = 0, cb[n++] = 0, cb[n++] = 0; /* START */
  cb[n++] = pm4(0x76, 4), cb[n++] = 0x207, cb[n++] = 8, cb[n++] = 8, cb[n++] = 1; /* NUM_THREAD */
  cb[n++] = pm4(0x76, 3), cb[n++] = 0x20c, cb[n++] = (u32)(shader >> 8), cb[n++] = (u32)(shader >> 40);
  /* RSRC1: VGPR and SGPR blocks, float mode 0xc0, DX10 clamp; RSRC2: 2 user SGPRs, group x
   * and y, thread x and y */
  u32 rsrc1 = ((K_MIRROR_GCN_VGPRS - 1) / 4) | (((K_MIRROR_GCN_SGPRS - 1) / 8) << 6) |
              (0xc0u << 12) | (1u << 21);
  u32 rsrc2 = (2u << 1) | (1u << 7) | (1u << 8) | (1u << 11);
  cb[n++] = pm4(0x76, 3), cb[n++] = 0x212, cb[n++] = rsrc1, cb[n++] = rsrc2;
  cb[n++] = pm4(0x76, 3), cb[n++] = 0x240, cb[n++] = (u32)consts, cb[n++] = (u32)(consts >> 32);
  cb[n++] = pm4(0x15, 4);
  cb[n++] = (w + 7) / 8, cb[n++] = (h + 7) / 8, cb[n++] = 1, cb[n++] = 0x41;
  /* RELEASE_MEM (gfx7 compute form): caches written back, then 32 bits of label */
  cb[n++] = pm4(0x49, 6);
  cb[n++] = (1u << 16) | (1u << 17) | (1u << 15) | (5u << 8) | 0x14;
  cb[n++] = 1u << 29;
  cb[n++] = (u32)label, cb[n++] = (u32)(label >> 32), cb[n++] = seq, cb[n++] = 0;
  return n;
}

static int setup(void) {
  if (g_m.w < 640 || g_m.w > 3840 || g_m.h < 360 || g_m.h > 2160) {
    LOG("mirror: unexpected main port size, no TV mirror");
    return -1;
  }
  /* step 2d: the mirror's memory from the reserve. Its buffers are the main port's (VideoOut scans
   * them out): refused there, they are made again in the game's range and registered again, so the
   * port always has the mirror's flips (a port left unflipped brought the console down) */
  g_m.mem = vr2_dalloc_pref(0x10000, 0x10000, "the TV mirror's shader and commands");
  if (!g_m.mem)
    return -1;
  memcpy(g_m.mem, k_mirror_gcn, sizeof k_mirror_gcn);
  size_t sz = (size_t)g_m.pitch * ((g_m.h + 7) & ~7u) * 4; /* rows rounded to 8: whole 8x8 groups */
  for (int pass = 0; pass < 2; pass++) {
    void *addrs[NBUF];
    for (int i = 0; i < NBUF; i++) {
      g_m.buf[i] = pass == 0 ? vr2_dalloc_pref(sz, 0x10000, "a TV mirror buffer") : vr2_dalloc(sz, 0x10000);
      if (!g_m.buf[i])
        return -1;
      addrs[i] = g_m.buf[i];
    }
    int rc = hooks_register_buffers(g_m.port, FIRST_INDEX, addrs, NBUF, g_m.attr);
    LOG("mirror: %d buffers on the main port (pitch %u) -> 0x%x%s", NBUF, g_m.pitch, rc,
        pass ? " (in the game's range)" : "");
    if (rc >= 0)
      return 0;
  }
  return -1;
}

/* ---------------------------------------------------------------- the loop */
static u32 g_m_overrun; /* copies with the game's black fill in them, not shown (mirror_thread) */
/* The share of exact black (0,0,0) in a 48x27 grid of a TV buffer, per mille. A copy that caught
 * the game drawing that texture again holds its black 1920x1080 canvas fill (the box on the TV,
 * 2026-11-07: exact zeros over part of the picture, the rest as before); the game's own black
 * comes as a fade (values falling to 0 over the whole picture) or a whole black picture. */
static u32 black_share(const u8 *buf) {
  u32 n = 0, t = 0;
  for (u32 j = 0; j < 27; j++)
    for (u32 i = 0; i < 48; i++, t++) {
      u32 x = (2 * i + 1) * g_m.w / 96, y = (2 * j + 1) * g_m.h / 54;
      n += (*(const volatile u32 *)(buf + ((u64)y * g_m.pitch + x) * 4) & 0xffffff) == 0;
    }
  return n * 1000 / t;
}
static void *mirror_thread(void *arg) {
  (void)arg;
  /* the game's main port, and Hmd2's GPU queue (which the dispatch goes on) */
  u64 vr_since = 0;
  while (g_shared.cmd != CMD_QUIT && (g_m.port < 0 || !gen1_queue_ready())) {
    /* The safety net: once the game is in VR mode and handing VR frames over (it flips its main
     * port no more), the main port needs flips (a mirror that stopped flipping brought the console
     * down, 2026-11-01). If Hmd2's queue is not up 20 s after that (PS VR2's output failed to
     * start, or waits for the headset), the port is flipped blank until it is. Before VR mode the
     * game flips its own 2D screens there, and is left alone. */
    if (g_m.port >= 0 && g_vr.vr_mode_vo && g_vr.frames_in > 0) {
      if (!vr_since)
        vr_since = sceKernelGetProcessTime();
      else if (sceKernelGetProcessTime() - vr_since >= 20000000)
        break;
    }
    sceKernelUsleep(100000);
  }
  if (g_shared.cmd == CMD_QUIT || setup() != 0)
    return 0;
  if (!gen1_queue_ready()) {
    LOG("mirror: PS VR2's GPU queue not up 20 s into VR mode: the main port flipped blank until it is");
    u32 n = 0, i = 0;
    while (g_shared.cmd != CMD_QUIT && !gen1_queue_ready()) {
      u32 w = g_vr.frame_w;
      int rc = sceVideoOutSubmitFlip(g_m.port, FIRST_INDEX + (s32)i, 1, w ? g_vr.frame[w & 3].flip_arg : 0);
      if (rc != 0 && n < 8)
        LOG("mirror: blank flip -> 0x%x", rc);
      i = (i + 1) % NBUF, n++;
      sceKernelUsleep(16667);
    }
    LOG("mirror: PS VR2's GPU queue up after %u blank flips: headset -> TV", n);
  }
  u8 *mem = g_m.mem;
  struct consts *k = (struct consts *)(mem + 0x1000);
  u32 *cb = (u32 *)(mem + 0x2000);
  volatile u32 *label = (volatile u32 *)(mem + 0x3000);
  /* errors: the log's (copies and flips); copy_errors: the copies that failed, which alone stop them */
  u32 last = g_vr.frame_w, seq = 0, cur = 0, errors = 0, copy_errors = 0, pending = 0, copy_off = 0;
  u32 blanked = 0; /* the buffers black since the last copy (agent.tvblank set while running: the menu) */
  LOG("mirror: headset -> TV");
  while (g_shared.cmd != CMD_QUIT) {
    u32 w = g_vr.frame_w;
    if (w == last) {
      sceKernelUsleep(2000);
      continue;
    }
    last = w;
    /* The newest frame, once the game's GPU has drawn it (its "drawn" labels reach the frame's
     * count: hooks_frame_drawn), up to 30 ms (a load: then as it is). The game hands a frame over
     * as soon as its GPU work is submitted, ~10 ms before it is drawn at 1920; the frame two
     * hand-overs back, copied at once, still tore at times (black lines on the TV, 2026-11-03).
     * Its eye textures are reused three frames on, long after the copy. Not drawn in 30 ms (a
     * load), it is skipped and the TV keeps the last finished frame (copied unfinished, the TV
     * tore in a load, 2026-11-03), unless the TV has not been flipped for 1 s: then it goes as it
     * is, so the main port never goes long without a flip (it needs them: a mirror that stopped
     * flipping brought the console down, 2026-11-01 00:42; the game's own start already leaves it
     * 2 s without one, every run). */
    static u64 flipped_at; /* the last flip (below) */
    struct vr_frame f;
    if (!vr_frame_copy(w, &f))
      continue; /* overwritten: a burst of frames */
    if ((f.tsharp[0][2] & 0x3fff) + 1 < 256)
      continue; /* the game's 1x1 black "paused" picture: the TV keeps the last frame */
    {
      static u32 waits, late, skipped, tv_waits, tv_busy;
      static u64 waited, report_at;
      /* (the TV's buffer first, then the frame: copied the moment it is drawn. Waited for after
       * the frame, up to 20 ms, the copy came late enough in a slow beat between the game's
       * frames and the TV's refresh to catch the game already drawing that texture again: its
       * black 1920x1080 canvas fill first, black on the TV in the late part of the beat,
       * 2026-11-07, scratchpad blackbox-report) */
      /* The buffer about to be written must be off the TV: not on screen, and no more than one flip
       * pending (with three buffers, the one written now was flipped three flips ago; it is still
       * on screen while the two flips after it wait, as when frames come close together: written
       * then, the TV showed it half old, half new). Up to 20 ms, a vblank and more; then as it is. */
      s32 tv_pending = 0, shown = -1; /* (pending, below: our copy still queued) */
      if (hooks_flip_state(g_m.port, &tv_pending, &shown) == 0 &&
          (tv_pending > 1 || shown == FIRST_INDEX + (s32)cur)) {
        tv_waits++;
        u64 t2 = sceKernelGetProcessTime();
        while ((tv_pending > 1 || shown == FIRST_INDEX + (s32)cur) &&
               sceKernelGetProcessTime() - t2 < 20000 && g_shared.cmd != CMD_QUIT) {
          sceKernelUsleep(500);
          if (hooks_flip_state(g_m.port, &tv_pending, &shown) != 0)
            break;
        }
        tv_busy += tv_pending > 1 || shown == FIRST_INDEX + (s32)cur;
        /* (the newest frame now: the one picked before the wait is a frame or more old by its
         * end, its texture soon drawn again; logs/tv-fix3: 600 of 600 frames waited, the label
         * long past) */
        u32 w2 = g_vr.frame_w;
        struct vr_frame f2;
        if (w2 != w && vr_frame_copy(w2, &f2) && (f2.tsharp[0][2] & 0x3fff) + 1 >= 256)
          f = f2, w = w2, last = w2;
      }
      /* The picture handed over with count c is complete once the drawn labels reach c - 1 and
       * drawn over soon after c + 1 (docs/FRAME-COPY.md, by the fingerprints, 2026-11-04): the
       * wait for c was a frame late, and in the late part of a slow beat the copy caught the game
       * already drawing that texture again (its black 1920x1080 fill: black on the TV, the copy's
       * labels at c, 2026-11-07 logs/tv-fix2) */
      u64 need = f.gpu_count > 1 ? f.gpu_count - 1 : f.gpu_count;
      u64 t0 = sceKernelGetProcessTime();
      while (!hooks_frame_drawn(need) && sceKernelGetProcessTime() - t0 < 30000 &&
             g_shared.cmd != CMD_QUIT)
        sceKernelUsleep(200);
      u64 t1 = sceKernelGetProcessTime();
      int drawn = hooks_frame_drawn(need);
      int skip = !drawn && flipped_at && t1 - flipped_at < 1000000;
      waits++, waited += t1 - t0, late += !drawn, skipped += skip;
      if (!report_at)
        report_at = t1 + 10000000;
      else if (t1 >= report_at) {
        LOG_DETAIL("mirror: %u frames waited for the game's GPU (%lu us on average), %u not drawn in 30 ms "
            "(%u of them skipped); %u waited for the TV to let go of their buffer, %u still busy "
            "after 20 ms; %u copies with the game's black fill (the last one shown again)",
            waits, waits ? waited / waits : 0, late, skipped, tv_waits, tv_busy, g_m_overrun);
        waits = late = skipped = tv_waits = tv_busy = 0, waited = 0, report_at = t1 + 10000000;
        g_m_overrun = 0;
      }
      if (skip)
        continue;
    }
    /* The TV refreshes 60 times a second: at a frame a refresh of the headset, every other
     * frame goes (flipped 120 times a second, the port's queue filled and the TV showed
     * whichever it caught: uneven), and the GPU does half the copies. (At 90 fps the same:
     * 45 on the TV.) */
    u64 t = sceKernelGetProcessTime();
    if (astro_hdr.frame_pace == 1 && flipped_at && t - flipped_at < 14000)
      continue;
    flipped_at = t;
    /* agent.tvblank=1: no copy, the buffers stay as vr2_dalloc cleared them (black); flipped as
     * every frame is, at the same pace and with the same frame: the main port needs its flips (a
     * mirror that stopped flipping brought the console down, 2026-11-01 00:42) */
    /* A copy not done yet (the queue held up) is never written over: its command buffer and
     * constants may still be queued. Until it is, the last finished buffer is flipped again: the
     * port keeps its flips, the TV its picture. */
    if (pending && *label != pending) {
      int rc = sceVideoOutSubmitFlip(g_m.port, FIRST_INDEX + (s32)((cur + NBUF - 1) % NBUF), 1,
                                     f.flip_arg);
      if (rc != 0 && errors++ < 8)
        LOG("mirror: flip (copy pending) -> 0x%x", rc);
      continue;
    }
    pending = 0;
    if (g_opt_tvblank || copy_off) {
      if (g_opt_tvblank && !blanked) { /* set while running (the settings menu): the buffers hold the
                                        * last pictures, black again (none has a copy pending) */
        for (int i = 0; i < NBUF; i++)
          memset(g_m.buf[i], 0, (size_t)g_m.pitch * ((g_m.h + 7) & ~7u) * 4);
        blanked = 1;
      }
      seq++;
      int rc = sceVideoOutSubmitFlip(g_m.port, FIRST_INDEX + (s32)cur, 1, f.flip_arg);
      if (rc != 0 && errors++ < 8)
        LOG("mirror: flip -> 0x%x", rc);
      if (seq == 1 || seq % 7200 == 0)
        LOG("mirror: frame %u on the TV, blank (agent.tvblank)", seq);
      cur = (cur + 1) % NBUF;
      continue;
    }
    /* the left eye maps tan(x) to u = offset + tan * scale (v likewise, down = +) */
    float tx = HALF_TAN_X, ty = HALF_TAN_X * (float)g_m.h / (float)g_m.w;
    float sx = f.uv[0].scale_x, sy = f.uv[0].scale_y;
    float dtx = 2 * tx / (float)g_m.w, dty = 2 * ty / (float)g_m.h;
    memcpy(k->tsharp, f.tsharp[0], 32);
    k->tsharp[1] &= ~(0xfu << 26); /* sRGB -> UNORM: the display buffer is sRGB-encoded too */
    memcpy(k->sampler, f.sampler, 16);
    u64 out = (u64)g_m.buf[cur];
    k->vsharp[0] = (u32)out, k->vsharp[1] = (u32)(out >> 32) & 0xffff;
    k->vsharp[2] = g_m.pitch * ((g_m.h + 7) & ~7u) * 4;
    k->vsharp[3] = 0x24fac; /* 32-bit UINT, x y z w */
    k->u0 = f.uv[0].offset_x + sx * (-tx + 0.5f * dtx), k->du = sx * dtx;
    k->v0 = f.uv[0].offset_y + sy * (-ty + 0.5f * dty), k->dv = sy * dty;
    if (f.flat || g_opt_mirrorboth) { /* a 2D picture, or agent.mirrorboth: the whole texture, as it is */
      k->u0 = 0.5f / (float)g_m.w, k->du = 1.0f / (float)g_m.w;
      k->v0 = 0.5f / (float)g_m.h, k->dv = 1.0f / (float)g_m.h;
    }
#ifndef RELEASE
    if (!f.flat && g_opt_mirrorboth == 2) { /* a debug tool: the texture 1:1, its bottom rows (the
                                             * insets', with multi-view) at the TV's bottom */
      u32 tw = (k->tsharp[2] & 0x3fff) + 1, th = ((k->tsharp[2] >> 14) & 0x3fff) + 1;
      s32 dy = (s32)th - (s32)g_m.h;
      g_mirror_dy = dy < 0 ? 0 : dy;
      k->u0 = 0.5f / (float)tw, k->du = 1.0f / (float)tw;
      k->v0 = ((float)g_mirror_dy + 0.5f) / (float)th, k->dv = 1.0f / (float)th;
    }
#endif
    /* agent.tvhmd=1 (a debug tool): what Hmd2 drew for the headset, both eyes as the panels show
     * them (the lenses' warp in it), in place of the game's picture: to see Hmd2's composition
     * (its layers) with nobody wearing the headset. Until Hmd2 has drawn twice, the game's. */
    static u32 tvhmd_said;
    u32 ht[8];
    if (g_opt_tvhmd && !f.flat && gen1_hmd_out(ht)) {
      memcpy(k->tsharp, ht, 32);
      k->u0 = 0.5f / (float)g_m.w, k->du = 1.0f / (float)g_m.w;
      k->v0 = 0.5f / (float)g_m.h, k->dv = 1.0f / (float)g_m.h;
      if (!tvhmd_said++)
        LOG("mirror: the headset's picture on the TV (agent.tvhmd): base 0x%lx, %ux%u",
            ((u64)(ht[1] & 0xff) << 40) | ((u64)ht[0] << 8), (ht[2] & 0x3fff) + 1,
            ((ht[2] >> 14) & 0x3fff) + 1);
    }
    k->pitch = g_m.pitch * 4;
    seq++, blanked = 0;
    u32 ndw = build_acb(cb, (u64)mem, (u64)k, (u64)label, seq, g_m.w, g_m.h);
    __asm__ volatile("mfence" ::: "memory");
    int rc = gen1_submit_acb(cb, ndw);
    int done = 0;
    for (int i = 0; rc == 0 && i < 300 && !(done = *label == seq); i++)
      sceKernelUsleep(100);
    if (rc != 0 || !done) {
      if (errors++ < 8)
        LOG("mirror: frame %u: submit 0x%x, %s", seq, rc, done ? "done" : "GPU timed out");
      if (rc == 0)
        pending = seq; /* submitted, not done: left alone until it is (above) */
      /* never given up on: the port needs its flips (a mirror that stopped flipping brought the
       * console down, 2026-11-01). After 30 failed copies (refused flips don't count) the copies
       * stop and the buffers are flipped as they are, as agent.tvblank does. */
      if (++copy_errors > 30 && !copy_off) {
        copy_off = 1;
        LOG("mirror: 30 copies failed: no more copies, the main port flipped as it is");
      }
      rc = sceVideoOutSubmitFlip(g_m.port, FIRST_INDEX + (s32)((cur + NBUF - 1) % NBUF), 1,
                                 f.flip_arg);
      continue;
    }
    /* A copy holding the game's black fill all the same (much more exact black than the last good
     * copy, the picture not all black): the last one is shown again and this buffer written next
     * time; thirty running (half a second) are taken as they are (a black panel of the game's own:
     * the TV never held for long) */
    {
      static u32 good = 0, run = 0, said = 0;
      u32 bs = black_share(g_m.buf[cur]);
      if (bs < 950 && bs > good + 150 && run < 30) {
        run++, g_m_overrun++;
        if (said++ < 30)
          LOG("mirror: frame %u: the copy %u per mille exact black against %u: not shown (the "
              "game's count %lu, its drawn label %lu, %u frames handed over since)", seq, bs, good,
              f.gpu_count, hooks_drawn_label(), g_vr.frame_w - w);
        rc = sceVideoOutSubmitFlip(g_m.port, FIRST_INDEX + (s32)((cur + NBUF - 1) % NBUF), 1,
                                   f.flip_arg);
        if (rc != 0 && errors++ < 8)
          LOG("mirror: flip (copy not shown) -> 0x%x", rc);
        continue;
      }
      run = 0, good = bs;
    }
    /* flip_arg as PS VR's reprojection gave it: the game's frame */
    rc = sceVideoOutSubmitFlip(g_m.port, FIRST_INDEX + (s32)cur, 1, f.flip_arg);
    if (rc != 0 && errors++ < 8)
      LOG("mirror: flip -> 0x%x", rc);
    if (seq == 1 || seq % 7200 == 0)
      LOG("mirror: frame %u on the TV (u %f+%f v %f+%f)", seq, k->u0, k->du, k->v0, k->dv);
#ifndef RELEASE
    if (rc == 0)
      capture_step(g_m.buf[cur]),
          probe_step(g_m.buf[cur], (k->tsharp[2] & 0x3fff) + 1, ((k->tsharp[2] >> 14) & 0x3fff) + 1,
                     f.render_time),
          probe_hmd(g_m.buf[cur]), flicker_step(g_m.buf[cur], ((k->tsharp[2] >> 14) & 0x3fff) + 1);
#endif
    cur = (cur + 1) % NBUF;
  }
  return 0;
}

void mirror_start(void) {
  u8 attr[64], thr[16];
  scePthreadAttrInit(attr);
  scePthreadAttrSetstacksize(attr, 128 * 1024);
  int rc = scePthreadCreate(thr, attr, mirror_thread, 0, "SummerLessonVR2 TV");
  scePthreadAttrDestroy(attr);
  if (rc)
    LOG("mirror: thread -> 0x%x", rc);
}
