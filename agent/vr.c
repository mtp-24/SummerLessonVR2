/* PS VR side of the head pose: VrTracker2 poses (vr2.c) are moved into the PS VR tracker space
 * the game expects (metres, +X right, +Y up, -Z towards the camera; the player sits 1.5 m in
 * front of it, facing it), the way AstroQuest seats its headset. The controller, which nothing
 * locates, is held in front of the player and turned by its own motion sensors; where PS VR2's
 * cameras see both palms around it (hand.c), it sits between them, as the PS Camera placed it.
 * (AstroBotVR2's view turning, controller placing and R1 swipe are not in this build.)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr.h"
#include "../common/test.h"

static const float k_origin[3]; /* the seat's origin in game space, below */
static u64 now_us(void);

/* The ring's slot for `seq`, taken out of use while it is filled in (seq 0): a reader copying it
 * meanwhile (vr_frame_copy) sees that. */
static struct vr_frame *frame_slot(u32 seq) {
  struct vr_frame *f = &g_vr.frame[seq & 3];
  __atomic_store_n(&f->seq, 0, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return f;
}

/* Frame `seq` copied out of the ring whole, 0 when it is not there (older, or being written over:
 * a reader three frames behind copies the slot the next frame is going into) */
int vr_frame_copy(u32 seq, struct vr_frame *out) {
  const struct vr_frame *f = &g_vr.frame[seq & 3];
  if (__atomic_load_n(&f->seq, __ATOMIC_ACQUIRE) != seq)
    return 0;
  *out = *f;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return out->seq == seq && __atomic_load_n(&f->seq, __ATOMIC_RELAXED) == seq;
}

void vr_submit_frame(const struct hmd_reprojection_param *p, const struct hmd_tracker_state *ts,
                     s64 flip_arg) {
  u32 seq = g_vr.frame_w + 1;
  struct vr_frame *f = frame_slot(seq);
  memcpy(f->tsharp[0], p->texture[0], 32);
  memcpy(f->tsharp[1], p->texture[1], 32);
  if (p->sampler)
    memcpy(f->sampler, p->sampler, 16);
  f->uv[0] = p->uv[0];
  f->uv[1] = p->uv[1];
  { /* multi-view (multiview.c): the texture is agent.multiview rows taller than the eyes, which
     * keep its top; the game's plugin still maps each eye over the whole height: v scaled to it */
    u32 rows = g_multiview_rows, th = ((f->tsharp[0][2] >> 14) & 0x3fff) + 1;
    if (rows && th > rows + 64) {
      float k = (float)(th - rows) / (float)th;
      for (int e = 0; e < 2; e++)
        f->uv[e].scale_y *= k, f->uv[e].offset_y *= k;
    }
  }
  for (int i = 0; i < 3; i++)
    f->render_pose.pos[i] = ts->position[i];
  for (int i = 0; i < 4; i++)
    f->render_pose.rot[i] = ts->orientation[i];
  f->render_time = ts->timestamp;
  f->flip_arg = flip_arg;
  f->label = (volatile u64 *)p->frame_label;
  f->gpu_count = hooks_frame_count(); /* before the frame is published (the mirror waits on it) */
  f->flat = 0;
  __atomic_store_n(&f->seq, seq, __ATOMIC_RELEASE);
  __atomic_store_n(&g_vr.frame_w, seq, __ATOMIC_RELEASE);
}

/* A flat picture (sceHmdReprojectionStart2dVr): the one texture for both eyes, drawn from where
 * the seat is, facing its forward (game space: the origin, no turn), so that it stands still in
 * the room like a screen. vr2.c gives it a screen's tangents. */
void vr_submit_flat(const u32 *tsharp, const u32 *sampler, const struct hmd_eye_uv *uv, u64 *label,
                    s64 flip_arg) {
  u32 seq = g_vr.frame_w + 1;
  struct vr_frame *f = frame_slot(seq);
  memcpy(f->tsharp[0], tsharp, 32);
  memcpy(f->tsharp[1], tsharp, 32);
  if (sampler)
    memcpy(f->sampler, sampler, 16);
  f->uv[0] = f->uv[1] = *uv;
  for (int i = 0; i < 3; i++)
    f->render_pose.pos[i] = k_origin[i];
  f->render_pose.rot[0] = f->render_pose.rot[1] = f->render_pose.rot[2] = 0.0f;
  f->render_pose.rot[3] = 1.0f;
  f->render_time = now_us();
  f->flip_arg = flip_arg;
  f->label = (volatile u64 *)label;
  f->gpu_count = 0; /* a flat picture: not waited for */
  f->flat = 1;
  __atomic_store_n(&f->seq, seq, __ATOMIC_RELEASE);
  __atomic_store_n(&g_vr.frame_w, seq, __ATOMIC_RELEASE);
}

float fsqrt(float x) {
  float r;
  __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
  return r;
}

static void qmul(const float a[4], const float b[4], float o[4]) {
  float r[4] = {
      a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
      a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
      a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
      a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
  };
  memcpy(o, r, sizeof r);
}

static void qconj(const float q[4], float o[4]) {
  o[0] = -q[0], o[1] = -q[1], o[2] = -q[2], o[3] = q[3];
}

static void qnorm(float q[4]) {
  float m = fsqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (m > 1e-6f)
    for (int i = 0; i < 4; i++)
      q[i] /= m;
}

/* v' = q v q* */
static void qrot(const float q[4], const float v[3], float o[3]) {
  float x = q[0], y = q[1], z = q[2], w = q[3];
  float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
  o[0] = v[0] + w * tx + (y * tz - z * ty);
  o[1] = v[1] + w * ty + (z * tx - x * tz);
  o[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* yaw-only rotation taking -Z to the forward (fx, fz), and its inverse */
static void yaw_to(float fx, float fz, float yaw[4], float inv[4]) {
  float n = fsqrt(fx * fx + fz * fz), s = 0.0f, c = 1.0f;
  if (n > 1e-4f)
    s = -fx / n, c = -fz / n; /* R_y(a) (0,0,-1) = (-sin a, 0, -cos a) = forward */
  /* quaternion for angle a about +Y: normalize(0, sin a, 0, 1 + cos a) */
  float y = s, w = 1 + c, m = fsqrt(y * y + w * w);
  if (m < 1e-6f)
    y = 1, w = 0, m = 1;
  yaw[0] = 0, yaw[1] = y / m, yaw[2] = 0, yaw[3] = w / m;
  inv[0] = 0, inv[1] = -y / m, inv[2] = 0, inv[3] = w / m;
}

/* ... to q's forward */
static void yaw_of(const float q[4], float yaw[4], float inv[4]) {
  yaw_to(-2 * (q[0] * q[2] + q[1] * q[3]), -(1 - 2 * (q[0] * q[0] + q[1] * q[1])), yaw, inv);
}

static u64 now_us(void) { return sceKernelGetProcessTime(); }

/* ---------------------------------------------------------------- the seat */
/* Where the head was and which way it faced (yaw only) at the last reset, in VrTracker2 space,
 * and the one before it: frames drawn just before a change were counted from that one. */
static struct {
  volatile u32 ok, had;
  float pos[3];
  float yaw[4];     /* rotation about +Y taking -Z to the seat's forward */
  float yaw_inv[4]; /* its inverse: tracker -> game */
  float prev_pos[3], prev_yaw[4];
  u64 changed;
  volatile u32 gen; /* bumps on every change */
  volatile u32 want; /* a recentre asked for: the next live head pose takes the seat (vr_recenter) */
} g_seat;
static const float k_origin[3] = {0.0f, 0.0f, 1.5f}; /* declared above for vr_submit_flat */

/* The head: in VrTracker2 space as last read, and in game space as last given to the game. */
static struct {
  float pos[3], rot[4];
  float game_rot[4];
  u32 valid, game_valid;
} g_head;

/* What a console sends the game when the player resets the view (ResetVrPosition): the game then
 * counts positions from where the head is (its origin). Every seat reset owes the game one, as in
 * AstroQuest, or the game's origin stays where the head was in the old seat: its calibration then
 * draws the controller's outline away from the controller. It goes out once the game has read
 * the head in the new seat. */
static volatile u32 g_reset_owed, g_reset_after, g_hmd_reads;

int vr_reset_event(int take) {
  if (!g_reset_owed || (s32)(g_hmd_reads - g_reset_after) < 0)
    return 0;
  return take ? __atomic_exchange_n(&g_reset_owed, 0, __ATOMIC_ACQ_REL) : 1;
}

/* ---- the controller ---- */
static struct {
  volatile u32 ok, realign;
  float q[4];      /* libScePad orientation, latest sample */
  float w[3];      /* its angular velocity, in the controller's frame */
  float align[4];  /* libScePad's frame -> game space (heading) */
  float anchor[3]; /* smoothed head position (game space) */
  float shown[3];  /* where it is reported, gliding to where it is held */
  u64 anchor_t, shown_t;
  u32 anchor_ok, shown_ok;
} g_pad = {.realign = 1};
/* AstroQuest's place for a controller nothing sees: 17 cm below and 50 cm in front of the head.
 * It is also where the game's calibration draws the controller's outline (calibration.xml
 * Position), counted from its origin, which a recentre puts at the head. */
static const float k_pad_offset[3] = {0.0f, -0.17f, -0.50f};

extern volatile u32 g_rec_reseats;
static void seat_reset(const float p[3], const float q[4]) {
  g_rec_reseats++;
  float yaw[4], inv[4];
  yaw_of(q, yaw, inv);
  if (g_seat.had) {
    memcpy(g_seat.prev_pos, g_seat.pos, 12);
    memcpy(g_seat.prev_yaw, g_seat.yaw, 16);
    /* The controller keeps pointing where it points: its heading counts from the new straight
     * ahead, which is the way the head faced in the old seat. */
    if (g_head.game_valid && !g_pad.realign) {
      float d[4], dinv[4];
      yaw_of(g_head.game_rot, d, dinv);
      qmul(dinv, g_pad.align, g_pad.align);
      qnorm(g_pad.align);
    }
  } else {
    memcpy(g_seat.prev_pos, p, 12);
    memcpy(g_seat.prev_yaw, yaw, 16);
  }
  memcpy(g_seat.yaw, yaw, 16);
  memcpy(g_seat.yaw_inv, inv, 16);
  memcpy(g_seat.pos, p, 12);
  g_seat.changed = now_us();
  g_seat.had = 1;
  __atomic_fetch_add(&g_seat.gen, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&g_seat.ok, 1, __ATOMIC_RELEASE);
  /* the controller is held in front of the new seat at once */
  g_pad.anchor_ok = g_pad.shown_ok = 0;
  g_reset_after = g_hmd_reads + 2;
  __atomic_store_n(&g_reset_owed, 1, __ATOMIC_RELEASE);
  LOG("vr: seat reset at %f %f %f, facing %f %f", p[0], p[1], p[2], -2 * (q[0] * q[2] + q[1] * q[3]),
      -(1 - 2 * (q[0] * q[0] + q[1] * q[1])));
}

static void to_game(const float seat_pos[3], const float seat_inv[4], const float p[3],
                    const float q[4], struct pose *out) {
  float d[3] = {p[0] - seat_pos[0], p[1] - seat_pos[1], p[2] - seat_pos[2]}, r[3];
  qrot(seat_inv, d, r);
  for (int i = 0; i < 3; i++)
    out->pos[i] = r[i] + k_origin[i];
  qmul(seat_inv, q, out->rot);
}

static void from_game(const float seat_pos[3], const float seat_yaw[4], const struct pose *g,
                      float p[3], float q[4]) {
  float d[3] = {g->pos[0] - k_origin[0], g->pos[1] - k_origin[1], g->pos[2] - k_origin[2]}, r[3];
  qrot(seat_yaw, d, r);
  for (int i = 0; i < 3; i++)
    p[i] = r[i] + seat_pos[i];
  qmul(seat_yaw, g->rot, q);
}

/* A render pose back to VrTracker2 space, for Hmd2. A pose the game drew a frame from just
 * before the seat changed was counted from the old seat; the head turns little in a frame, so of
 * the two seats the one that puts the pose nearer to the head now is the one it came from
 * (AstroQuest's ToHostSpace). */
void vr_game_to_tracker(const struct pose *g, float p[3], float q[4]) {
  if (!g_seat.ok) {
    memcpy(p, g->pos, 12);
    memcpy(q, g->rot, 16);
    return;
  }
  from_game(g_seat.pos, g_seat.yaw, g, p, q);
  if (!g_head.valid || now_us() - g_seat.changed >= 250000)
    return;
  float pp[3], pq[4];
  from_game(g_seat.prev_pos, g_seat.prev_yaw, g, pp, pq);
  const float *h = g_head.rot;
  float a = q[0] * h[0] + q[1] * h[1] + q[2] * h[2] + q[3] * h[3];
  float b = pq[0] * h[0] + pq[1] * h[1] + pq[2] * h[2] + pq[3] * h[3];
  if ((b < 0 ? -b : b) > (a < 0 ? -a : a)) {
    memcpy(p, pp, 12);
    memcpy(q, pq, 16);
  }
}

/* ---- the controller in the player's hands (PS VR2 hand tracking, hand.c) ----
 * AstroQuest's rule (UpdatePad): both palms seen 5 to 32 cm apart hold it at their midpoint, 1.5 cm
 * up. When the palm line (left to right) is near level (its level part over 0.6 of it), it is the
 * controller's sideways axis: the heading is pulled towards facing straight out of it at 1.5/s,
 * and the middle sits 3.5 cm ahead along it (the palms close round the grips, behind it). One
 * hand alone is not used: libSceVrHand seldom keeps track of a single hand round a controller.
 * goal: where it is held, in game space. */
static int pad_in_hands(const float q[4], float dt, float goal[3]) {
  static const float id[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  static int held = -1;
  float l[3], r[3];
  int ok = g_seat.ok && hand_palm(0, l) && hand_palm(1, r);
  struct pose gl, gr;
  float apart = 0.0f;
  if (ok) {
    float d[3] = {r[0] - l[0], r[1] - l[1], r[2] - l[2]};
    apart = fsqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    ok = apart > 0.05f && apart < 0.32f;
  }
  if (ok != held) {
    static u32 logged;
    if (held >= 0 && logged++ < 20)
      LOG("vr: controller %s", ok ? "in your hands" : "back at its place (hands not seen)");
    held = ok;
  }
  if (!ok)
    return 0;
  to_game(g_seat.pos, g_seat.yaw_inv, l, id, &gl);
  to_game(g_seat.pos, g_seat.yaw_inv, r, id, &gr);
  float dx = gr.pos[0] - gl.pos[0], dz = gr.pos[2] - gl.pos[2];
  for (int i = 0; i < 3; i++)
    goal[i] = (gl.pos[i] + gr.pos[i]) * 0.5f;
  goal[1] += 0.015f;
  if (fsqrt(dx * dx + dz * dz) <= 0.6f * apart)
    return 1; /* hands one above the other: no heading from them */
  /* the heading: forward is the palm line turned 90 degrees, (dx, dz) -> (dz, -dx) */
  float want[4], want_inv[4], have[4], have_inv[4], rot[4];
  yaw_to(dz, -dx, want, want_inv);
  qmul(g_pad.align, q, rot);
  yaw_of(rot, have, have_inv);
  float delta[4], k = dt * 1.5f;
  qmul(want, have_inv, delta);
  if (delta[3] < 0)
    for (int i = 0; i < 4; i++)
      delta[i] = -delta[i];
  if (k > 1.0f)
    k = 1.0f;
  float step[4] = {delta[0] * k, delta[1] * k, delta[2] * k, 1.0f - k + delta[3] * k};
  qnorm(step);
  qmul(step, g_pad.align, g_pad.align);
  qnorm(g_pad.align);
  /* 3.5 cm ahead along the palm line's own heading (forward is -z) */
  static const float ahead[3] = {0.0f, 0.0f, -0.035f};
  float o[3];
  qrot(want, ahead, o);
  for (int i = 0; i < 3; i++)
    goal[i] += o[i];
  return 1;
}

/* Hands lost (out of the cameras' view, or one of them unseen a moment), the controller stays where
 * they last had it, for PAD_KEEP or as long as it is held still (barely turning), before it glides
 * to its place; seen again, it glides back into them over PAD_REGAIN rather than snapping. At the
 * edge of the cameras' view, where the palms come and go, it stays in the hands. */
#define PAD_KEEP 2000000  /* us */
#define PAD_STILL_W 0.35f /* rad/s */
#define PAD_REGAIN 150000 /* us */
static struct {
  float at[3]; /* where the hands last had it */
  u64 lost, back; /* when they were lost (0: they have it) and seen again */
  u32 had, gen;
} g_kept;

/* ---- the controller: orientation from libScePad, place in front of the player ---- */
static void pad_pose(struct tracked *out) {
  float q[4];
  memcpy(q, g_pad.q, 16);
  if (g_pad.realign) {
    /* straight ahead is where it points now */
    float yaw[4], inv[4];
    yaw_of(q, yaw, inv);
    memcpy(g_pad.align, inv, 16);
    g_pad.realign = 0;
    LOG("vr: controller heading aligned");
  }
  u64 t = now_us();
  float dt = g_pad.shown_ok ? (float)(t - g_pad.shown_t) * 1e-6f : 0.0f, goal[3];
  int hands = pad_in_hands(q, dt, goal), kept = 0;
  if (g_kept.gen != g_seat.gen) /* a place counted from the seat before */
    g_kept.had = 0, g_kept.gen = g_seat.gen;
  if (hands) {
    if (g_kept.lost)
      g_kept.back = t;
    memcpy(g_kept.at, goal, 12);
    g_kept.had = 1, g_kept.lost = 0;
  } else if (g_kept.had) {
    float w2 = g_pad.w[0] * g_pad.w[0] + g_pad.w[1] * g_pad.w[1] + g_pad.w[2] * g_pad.w[2];
    if (!g_kept.lost || w2 < PAD_STILL_W * PAD_STILL_W)
      g_kept.lost = t; /* held still: kept there */
    if ((t > g_kept.lost ? t - g_kept.lost : 0) < PAD_KEEP) {
      memcpy(goal, g_kept.at, 12);
      kept = 1;
    } else {
      g_kept.had = 0;
      static u32 logged;
      if (logged++ < 20)
        LOG("vr: controller unseen %u s: to its place", PAD_KEEP / 1000000);
    }
  }
  qmul(g_pad.align, q, out->pose.rot);
  qrot(out->pose.rot, g_pad.w, out->ang_vel);
  /* Unseen, it hangs off the head's position, not its turn (hands do not swing round when the
   * player looks about), at the standard place; and glides there rather than jumping
   * (AstroQuest's GetPad). */
  if (!hands && !kept)
    for (int i = 0; i < 3; i++)
      goal[i] = g_pad.anchor[i] + k_pad_offset[i];
  if (!g_pad.shown_ok) {
    memcpy(g_pad.shown, goal, 12);
    g_pad.shown_ok = 1;
  } else {
    /* just back in the hands 80 ms, kept 35 ms (it doesn't move), to its place 0.3 s; in the
     * hands as a One Euro filter (Casiez, Roussel, Vogel 2012): 35 ms while it is still or slow,
     * less as it moves faster (to 8 ms), so a move trails it less and a still one doesn't jitter;
     * its speed taken over about 80 ms (2 Hz), the palms' jitter barely counting */
    static float e_dx[3];
    int euro = hands && (t > g_kept.back ? t - g_kept.back : 0) >= PAD_REGAIN;
    float tc = !hands && !kept ? 0.30f : hands ? 0.08f : 0.035f;
    for (int i = 0; i < 3; i++) {
      if (euro && dt > 0) {
        float raw = (goal[i] - g_pad.shown[i]) / dt;
        e_dx[i] += (raw - e_dx[i]) * (dt / (dt + 1.0f / (6.2831853f * 2.0f)));
        float f = 4.55f + 25.0f * (e_dx[i] < 0 ? -e_dx[i] : e_dx[i]); /* Hz: 35 ms still */
        tc = 1.0f / (6.2831853f * (f > 20.0f ? 20.0f : f));
      } else {
        e_dx[i] = 0;
      }
      g_pad.shown[i] += (goal[i] - g_pad.shown[i]) * (dt / (tc + dt));
    }
  }
  g_pad.shown_t = t;
  memcpy(out->pose.pos, g_pad.shown, 12);
}

static void pad_follow_head(const struct pose *head) {
  u64 now = now_us();
  if (!g_pad.anchor_ok) {
    memcpy(g_pad.anchor, head->pos, 12);
    g_pad.anchor_ok = 1;
  } else {
    float dt = (float)(now - g_pad.anchor_t) * 1e-6f;
    float a = dt / (0.25f + dt);
    for (int i = 0; i < 3; i++)
      g_pad.anchor[i] += (head->pos[i] - g_pad.anchor[i]) * a;
  }
  g_pad.anchor_t = now;
}

/* ---------------------------------------------------------------- the controller's buttons */
/* What the player does with the controller about the view itself (AstroQuest's view gestures),
 * which a console's system would see to:
 *  - with agent.recentre=1, the first press of circle takes the seat where the player is (they
 *    have settled by then); off by default (it was the first X, from Astro Bot);
 *  - holding OPTIONS for a second resets the view, and the controller's heading with it.
 * Every button reaches the game. */
#define PAD_OPTIONS 0x0008
#define PAD_CIRCLE 0x2000

static struct {
  u32 prev, circle_seen, options_fired;
  u64 options_since;
} g_btn;

void vr_pad_read(u8 *d) {
  u32 b = *(u32 *)d;
  u64 now = now_us();
  int finite = 1; /* orientation, acceleration and turn (0x0c..0x34): all numbers, or none taken */
  for (int i = 0; i < 10; i++)
    finite &= __builtin_isfinite(((const float *)(d + 0x0c))[i]);
  if (d[0x4c] && finite) {
    memcpy(g_pad.q, d + 0x0c, 16);
    memcpy(g_pad.w, d + 0x28, 12);
    g_pad.ok = 1;
  }
  u32 pressed = b & ~g_btn.prev;
  if (g_opt_recentre && (pressed & PAD_CIRCLE) && !g_btn.circle_seen) {
    g_btn.circle_seen = 1;
    vr_request_recenter("first press of circle (agent.recentre)");
  }
  if (!(b & PAD_OPTIONS))
    g_btn.options_since = 0;
  else if (!g_btn.options_since)
    g_btn.options_since = now, g_btn.options_fired = 0;
  else if (!g_btn.options_fired && now - g_btn.options_since >= 1000000) {
    g_btn.options_fired = g_btn.circle_seen = 1;
    vr_request_recenter("OPTIONS held");
    g_pad.realign = 1;
  }
  g_btn.prev = b;
}

static void set_pose(struct pose *p, float x, float y, float z) {
  p->pos[0] = x;
  p->pos[1] = y;
  p->pos[2] = z;
  p->rot[0] = p->rot[1] = p->rot[2] = 0.0f;
  p->rot[3] = 1.0f;
}

/* Eye = device pose moved half the IPD along its own x axis. */
static void eye_pose(const struct pose *dev, float dx, struct pose *eye) {
  float v[3] = {dx, 0, 0}, r[3];
  qrot(dev->rot, v, r);
  for (int i = 0; i < 3; i++)
    eye->pos[i] = dev->pos[i] + r[i];
  memcpy(eye->rot, dev->rot, 16);
}

/* How fast the head moves and turns, from one pose to the next (AstroQuest's UpdateHead): the
 * game takes a head that pushes into something for a push (a headbutt) only when it comes at
 * some speed. Game space; starts again whenever the seat changes. */
static struct {
  float pos[3], rot[4], v[3], w[3];
  u64 t;
  u32 gen, ok;
} g_vel;

static void head_velocity(const struct pose *p, u64 t, float v[3], float w[3]) {
  u32 gen = g_seat.gen;
  if (!g_vel.ok || g_vel.gen != gen) {
    memset(&g_vel, 0, sizeof g_vel);
    g_vel.ok = 1, g_vel.gen = gen;
  } else {
    float dt = (float)(s64)(t - g_vel.t) * 1e-6f;
    if (dt < 0.002f && dt > -0.002f)
      goto out; /* the same moment again */
    if (dt > 0.1f || dt < 0) {
      memset(g_vel.v, 0, 12);
      memset(g_vel.w, 0, 12);
    } else {
      /* the turn over dt, in game space: q_now q_prev* = (sin(a/2) n, cos(a/2)); small angles */
      float c[4], dq[4];
      qconj(g_vel.rot, c);
      qmul(p->rot, c, dq);
      float sgn = dq[3] < 0 ? -1.0f : 1.0f;
      for (int i = 0; i < 3; i++) {
        g_vel.v[i] += ((p->pos[i] - g_vel.pos[i]) / dt - g_vel.v[i]) * 0.5f;
        g_vel.w[i] += (2.0f * sgn * dq[i] / dt - g_vel.w[i]) * 0.5f;
      }
    }
  }
  memcpy(g_vel.pos, p->pos, 12);
  memcpy(g_vel.rot, p->rot, 16);
  g_vel.t = t;
out:
  memcpy(v, g_vel.v, 12);
  memcpy(w, g_vel.w, 12);
}

/* The head pose's motion between consecutive asks, in degrees: of the pose given to the game
 * (predicted as far as asked, or capped: agent.predict) and of the tracker's unpredicted pose.
 * With the headset still, both are noise; the predicted one shows what the horizon amplifies. */
static u64 g_jit_pred_sum, g_jit_raw_sum; /* micro-degrees */
static u32 g_jit_n, g_jit_pred_max, g_jit_raw_max;
static u32 quat_angle_udeg(const float *a, const float *b) {
  float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (d < 0)
    d = -d;
  if (d > 1.0f)
    d = 1.0f;
  float s = 1.0f - d * d; /* sin^2(theta/2) */
  float th = 2.0f * fsqrt(s > 0 ? s : 0) * 57.29578f; /* small angles: theta = 2 sin(theta/2) */
  return (u32)(th * 1000000.0f);
}
volatile u32 g_rec_pose_max, g_rec_reseats; /* the flight recorder (hooks.c): the largest per-ask
                                             * step of the pose given to the game, re-seats */
static void jitter_note(const float *pred_q, const float *raw_q) {
  static float lp[4], lr[4];
  static int have;
  if (have) {
    u32 a = quat_angle_udeg(pred_q, lp), b = quat_angle_udeg(raw_q, lr);
    g_jit_pred_sum += a, g_jit_raw_sum += b, g_jit_n++;
    if (a > g_jit_pred_max)
      g_jit_pred_max = a;
    if (a > g_rec_pose_max)
      g_rec_pose_max = a;
    if (b > g_jit_raw_max)
      g_jit_raw_max = b;
  }
  memcpy(lp, pred_q, 16);
  memcpy(lr, raw_q, 16);
  have = 1;
}
void vr_jitter_report(void) {
  if (!g_jit_n)
    return;
  u32 p = (u32)(g_jit_pred_sum / g_jit_n), r = (u32)(g_jit_raw_sum / g_jit_n);
  LOG_DETAIL("pose: moved %u.%03u deg an ask on average as predicted (%u.%03u at the most), %u.%03u raw "
      "(%u.%03u at the most), %u asks",
      p / 1000000, p / 1000 % 1000, g_jit_pred_max / 1000000, g_jit_pred_max / 1000 % 1000,
      r / 1000000, r / 1000 % 1000, g_jit_raw_max / 1000000, g_jit_raw_max / 1000 % 1000, g_jit_n);
  g_jit_pred_sum = g_jit_raw_sum = 0, g_jit_n = 0, g_jit_pred_max = g_jit_raw_max = 0;
}

void vr_get_pose(int hmd, u64 prediction_time, struct tracked *out) {
  memset(out, 0, sizeof *out);
  if (!hmd) {
    if (g_pad.ok && g_seat.ok && g_pad.anchor_ok)
      pad_pose(out);
    else
      set_pose(&out->pose, k_origin[0] + k_pad_offset[0], k_origin[1] + k_pad_offset[1],
               k_origin[2] + k_pad_offset[2]);
    return;
  }
  float p[3], q[4];
  if (vr2_head_pose(prediction_time, p, q)) {
    /* The tracker starts in a provisional frame (IMU only) and moves to its map frame when it
     * relocalises: a jump no head makes within 100 ms re-seats the player. */
    static u64 lt;
    u64 now = now_us();
    if (g_seat.ok && lt && now - lt < 100000) {
      const float *lp = g_head.pos, *lq = g_head.rot;
      float dx = p[0] - lp[0], dy = p[1] - lp[1], dz = p[2] - lp[2];
      float dot = q[0] * lq[0] + q[1] * lq[1] + q[2] * lq[2] + q[3] * lq[3];
      if (dot < 0)
        dot = -dot;
      if (dx * dx + dy * dy + dz * dz > 0.25f * 0.25f || dot < 0.966f) {
        LOG("vr: tracker jumped (%f m, cos %f): re-seat", fsqrt(dx * dx + dy * dy + dz * dz), dot);
        vr_recenter();
      }
    }
    memcpy(g_head.pos, p, 12);
    memcpy(g_head.rot, q, 16);
    g_head.valid = 1;
    lt = now;
    u32 want = __atomic_exchange_n(&g_seat.want, 0, __ATOMIC_ACQ_REL);
    if (!g_seat.ok || want)
      seat_reset(p, q);
    to_game(g_seat.pos, g_seat.yaw_inv, p, q, &out->pose);
    if (g_test & (TEST_RECORD | TEST_REPLAY))
      test_head(&out->pose);
    head_velocity(&out->pose, prediction_time ? prediction_time : now, out->vel, out->ang_vel);
    memcpy(g_head.game_rot, out->pose.rot, 16);
    g_head.game_valid = 1;
    if (prediction_time) { /* the raw pose beside the predicted one, for the jitter figures */
      float rp[3], rq[4];
      if (vr2_head_pose(0, rp, rq))
        jitter_note(q, rq);
    }
    static u32 n;
    if (n++ % 600 == 0)
      LOG_DETAIL("vr: head tracker %f %f %f q %f %f %f %f -> game %f %f %f q %f %f %f %f, %f m/s",
          p[0], p[1], p[2], q[0], q[1], q[2], q[3], out->pose.pos[0], out->pose.pos[1],
          out->pose.pos[2], out->pose.rot[0], out->pose.rot[1], out->pose.rot[2],
          out->pose.rot[3],
          fsqrt(out->vel[0] * out->vel[0] + out->vel[1] * out->vel[1] + out->vel[2] * out->vel[2]));
  } else {
    /* Seated 1.5 m in front of the camera, facing it (-z). */
    set_pose(&out->pose, 0.0f, 0.0f, 1.5f);
    if (g_test & TEST_REPLAY)
      test_head(&out->pose);
  }
  out->head = out->pose;
  __atomic_fetch_add(&g_hmd_reads, 1, __ATOMIC_RELAXED);
  pad_follow_head(&out->pose);
  eye_pose(&out->pose, -g_vr.ipd * 0.5f, &out->eye[0]);
  eye_pose(&out->pose, g_vr.ipd * 0.5f, &out->eye[1]);
}

/* Test mode (test.txt, pads.c): the head's pose as the game gets it, recorded to the log as
 * `head:` lines (ms of process time, position, rotation; one at most every 50 ms, when it has
 * moved), or taken from head.txt (the same lines, read into g_head_script by the injector),
 * since the game's menus are chosen by looking at them. */
struct head_step {
  u32 t_ms;
  float pos[3], rot[4];
};
_Static_assert(sizeof(struct head_step) == 32, "payload/main.c fills it as 32 bytes a step");
struct head_step g_head_script[TEST_HEAD_STEPS];
u32 g_head_script_n;

void test_head(struct pose *p) {
  u64 now = now_us();
  u32 t = (u32)(now / 1000);
  if (g_test & TEST_RECORD) {
    static struct head_step last;
    static u64 next;
    if (now < next)
      return;
    float dx = p->pos[0] - last.pos[0], dy = p->pos[1] - last.pos[1], dz = p->pos[2] - last.pos[2];
    float dot = p->rot[0] * last.rot[0] + p->rot[1] * last.rot[1] + p->rot[2] * last.rot[2] +
                p->rot[3] * last.rot[3];
    if (dx * dx + dy * dy + dz * dz < 0.01f * 0.01f && (dot > 0.99995f || dot < -0.99995f))
      return;
    last.t_ms = t;
    memcpy(last.pos, p->pos, 12);
    memcpy(last.rot, p->rot, 16);
    next = now + 50000;
    LOG("head: %u %f %f %f %f %f %f %f", t, p->pos[0], p->pos[1], p->pos[2], p->rot[0], p->rot[1],
        p->rot[2], p->rot[3]);
    return;
  }
  static u32 i;
  while (i < g_head_script_n && g_head_script[i].t_ms <= t)
    i++;
  if (!i)
    return;
  const struct head_step *a = &g_head_script[i - 1];
  memcpy(p->pos, a->pos, 12);
  memcpy(p->rot, a->rot, 16);
  if (i < g_head_script_n) {
    /* between two samples (50 ms apart at most): the pose moved on, else the view turns in
     * steps (seen on the TV mirror, which shows the frames as drawn) */
    const struct head_step *b = &g_head_script[i];
    float f = b->t_ms > a->t_ms ? (float)(t - a->t_ms) / (float)(b->t_ms - a->t_ms) : 0.0f;
    if (f > 1.0f)
      f = 1.0f;
    float dot = a->rot[0] * b->rot[0] + a->rot[1] * b->rot[1] + a->rot[2] * b->rot[2] +
                a->rot[3] * b->rot[3];
    float sgn = dot < 0.0f ? -1.0f : 1.0f, n2 = 0.0f;
    for (int k = 0; k < 3; k++)
      p->pos[k] += (b->pos[k] - a->pos[k]) * f;
    for (int k = 0; k < 4; k++) {
      p->rot[k] += (sgn * b->rot[k] - a->rot[k]) * f;
      n2 += p->rot[k] * p->rot[k];
    }
    if (n2 > 0.0f) {
      float inv = 1.0f / fsqrt(n2);
      for (int k = 0; k < 4; k++)
        p->rot[k] *= inv;
    }
  }
}

/* The seat follows the head again at the next live pose; the game's origin follows it there
 * (ResetVrPosition, above). The old seat stays in force until then: cleared at once (as it was),
 * a frame handed over in that moment went to PS VR2 with its game-space pose taken for a tracker
 * one, and the view jolted for a frame at every recentre. */
void vr_recenter(void) { __atomic_store_n(&g_seat.want, 1, __ATOMIC_RELEASE); }

void vr_request_recenter(const char *why) {
  LOG("vr: recentre (%s)", why);
  vr_recenter();
}
