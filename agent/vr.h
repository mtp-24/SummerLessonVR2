/* State shared between the PS VR emulation (hooks.c) and the PS VR2 side (vr.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "agent.h"
#include "psvr1.h"

struct pose {
  float pos[3];
  float rot[4]; /* x y z w */
};

struct tracked {
  struct pose pose; /* device pose */
  struct pose head;
  struct pose eye[2];
  float vel[3], ang_vel[3];
};

/* One frame as the game handed it to sceHmdReprojectionStart. */
struct vr_frame {
  u32 tsharp[2][8]; /* GCN T# per eye */
  u32 sampler[4];
  struct hmd_eye_uv uv[2];
  struct pose render_pose;
  u64 render_time;
  s64 flip_arg;
  u32 seq;
  volatile u64 *label; /* the game's GPU writes it when the frame is drawn */
  u32 flat;            /* a 2D picture (sceHmdReprojectionStart2dVr): one texture, both eyes, a
                        * screen fixed before the seat */
  u32 pad;
  u64 gpu_count;       /* the game's frame count for it (viewport +0x360 at the hand-over): drawn
                        * once the game's GPU labels (viewport +0x2a8) reach it; 0 unknown */
};

struct vr_state {
  volatile u64 shown_count[64]; /* vr2.c: the game's frame count of each frame passed to Hmd2, by
                                 * its flip argument (hooks_flip checks it was drawn) */
  /* PS VR emulation */
  s32 user_id;
  float ipd;
  struct hmd_fov title_fov;
  volatile u32 reproj_initialized, tracker_init;
  s32 display_vo, display_index[2];
  void *end_eq;
  s32 end_id;
  volatile u32 end_set;
  volatile u64 vblank_period_us;
  volatile u32 vblanks;
  volatile u32 frames_in;
  s32 hmd_tracker_handle, pad_tracker_handle;
  void *vo_buffers[16];
  u32 force_modes;
  u32 vr2_started;
  s32 vr_mode_vo;
  s32 reproj_prio;    /* what the game gave the PS VR reprojection thread */
  u64 reproj_affinity;

  /* ---- everything above survives a hot swap (append new persistent fields here) ---- */
  volatile u32 tracker_reads;
  /* latest frame */
  struct vr_frame frame[4];
  volatile u32 frame_w; /* sequence of the newest frame; frame[seq & 3] */
  s32 pad_led; /* light bar colour the game gave the controller (0..4) */
  /* The game's main VideoOut port as the PS VR reprojection drove it: flipped every 120 Hz
   * refresh. Its vblank events on the VR queue are replaced by ours. */
  void *vbl_eq;
  s32 main_vo;
  volatile u64 vbl_count, vbl_time, vbl_tsc;
  volatile s64 vbl_flip_arg;
  volatile u32 hmd_off; /* PS VR2's attachment sensor: the headset is not on a head */
  u32 vr_mode_refresh;  /* the PS VR refresh code the game asked for (0x0d 119.88 Hz, 0x23 90 Hz) */
};

extern struct vr_state g_vr;

void vr_init_defaults(void);
void vr_submit_frame(const struct hmd_reprojection_param *p, const struct hmd_tracker_state *ts,
                     s64 flip_arg);
int vr_frame_copy(u32 seq, struct vr_frame *out);
void vr_submit_flat(const u32 *tsharp, const u32 *sampler, const struct hmd_eye_uv *uv,
                    u64 *label, s64 flip_arg); /* a 2D picture (Start2dVr) */
void vr_get_pose(int hmd, u64 prediction_time, struct tracked *out);
void vr_jitter_report(void);
float fsqrt(float x); /* vr.c */
/* (vr_jitter_report, above: the head pose's frame-to-frame angle, predicted and raw, in the health line) */
void vr_recenter(void);
void vr_request_recenter(const char *why);
int vr_reset_event(int take); /* a ResetVrPosition owed to the game (take: and hand it over) */
void vr_pad_read(u8 *sample); /* one 0x78-byte scePadRead sample; may hold back buttons */
/* Test mode (test.txt, payload main.c, tools/testrun.sh): the controller recorded to the log or
 * played back from pad.txt (pads.c), the headset counted as worn (vr2.c). */
#define TEST_RECORD 1u
#define TEST_REPLAY 2u
#define TEST_MOUNTED 4u
extern u32 g_test;
s32 pads_test_read(s32 handle, u8 *data, s32 num, s32 n);
void test_head(struct pose *p); /* vr.c: the head's game pose recorded, or the script's */
void vr_game_to_tracker(const struct pose *g, float pos[3], float rot[4]);
int vr2_head_pose(u64 time, float pos[3], float quat[4]);
int hand_palm(int hand, float pos[3]); /* hand.c: 0 left, 1 right; VrTracker2 space; 1 = seen */
void agent_tick(void);
void vr2_start(void);
/* Bump to make a hot-swapped agent run the PS VR2 bring-up again in the same game. */
#define VR2_BUILD 11
