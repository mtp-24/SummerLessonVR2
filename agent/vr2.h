/* PS VR2 inside the PS4 game: shared by vr2.c (output and tracking), gen1.c (running the PS5
 * GPU work in a PS4-mode GPU context) and ps5compat.c (making the PS5 libraries work in a PS4
 * process).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "vr.h"

/* the four libraries, in load order */
enum { LIB_AGCDRIVER, LIB_AGC, LIB_HMD2, LIB_VRTRACKER2, NLIBS };

/* helpers (vr2.c) */
int vr2_override(int handle, const char *nid, void *fn);
int vr2_find_module(const char *name);
u64 vr2_module_base(int handle);
u64 vr2_module_of(u64 addr, const char **name); /* the module an address is in: its base, its name */
void *vr2_dalloc(size_t size, size_t align);
void *vr2_dalloc_type(size_t size, size_t align, int type);
/* the reserve in the container's extra segment (vr2.c; step 1: allocated and checked, unused) */
struct vr2_reserve {
  u8 *va;
  s64 phys;
  u64 size, used;
  s64 spare_lo, spare_hi; /* step 3: the segment below the reserve, for the game's refused blocks */
};
extern struct vr2_reserve g_reserve;
void vr2_reserve_init(void);
void *vr2_ralloc(size_t size, size_t align); /* a piece of the reserve, cleared; 0: none */
void *vr2_dalloc_pref(size_t size, size_t align, const char *what); /* the reserve first */
/* the VR port's buffers (vr2.c): the game's eye texture pairs, up to MAX_PAIRS */
#define MAX_PAIRS 8

/* gen1.c */
void gen1_install(const int handles[NLIBS]);
int gen1_upload_shader(void);
void gen1_note_game_tsharp(const u32 *t);
int gen1_queue_ready(void);
int gen1_hmd_out(u32 *tsharp); /* the headset's last finished picture (agent.tvhmd) */
u64 multiview_boot(u64 n); /* multiview.c */
void multiview_booted(void);          /* the boot returned: the payload applies the table next */
void multiview_native_confirm(void);  /* once after that: native 90/120 from the agent's table */
int multiview_stock_pct(u32 *bits);   /* the stock game's eye size when inset mode was refused */
u32 gen1_refreshes(void);
u64 gen1_last_flip(void); /* when Hmd2 last flipped the headset (us) */
u32 gen1_last_built(u32 *frame); /* Hmd2's builds so far, and the frame the newest took */
/* hooks.c: the hand-off (vr2.c) passed a frame too late for its refresh (the original port's 120 fps
 * timer rule aimed the next frame a refresh later; nothing consumes it in this build) */
extern volatile u32 g_late_pass, g_release_n;
int gen1_submit_acb(const u32 *cb, u32 ndw);
extern volatile u32 g_display_pitch; /* set by ps5compat.c for the linear headset buffers */

/* ps5compat.c */
void ps5compat_install(const int handles[NLIBS]);
void ps5compat_mark_thread(void);
void ps5compat_allow_vr_layout(void);
void ps5compat_install_hand(int handle);

/* hooks.c */
void hooks_wake(void);         /* the vblank thread woken: Hmd2 flipped */
void hooks_flip(u32 flip_arg); /* Hmd2 flipped, showing this frame of vr2.c's */
void vr2_frame_ready(void);    /* vr2.c: the game handed a frame over */
void hooks_note_unpassed(u32 n, u32 why); /* hooks.c: frames that will never be shown (the exact count;
                                          * why: 1 the newest passed, 2 gone from the ring, 3 overwritten,
                                          * 4 a flip skipped a passed frame) */

/* hand.c: PS VR2 hand tracking (libSceVrHand) */
void hand_load(void);  /* load_stack: loaded, stubbed and overridden, not started */
void hand_start(void); /* hand_palm() is in vr.h */
int hand_clear_why(u8 *st, int *src); /* 0: worn, no system screen up (st: 512 bytes) */
extern volatile u32 g_hand_settled; /* the palm tracker's start is over (or won't happen) */
/* hand.c: the one camera start at a time, the palm tracker's (CAMERA_START_PALM) or eye tracking's
 * (CAMERA_START_GAZE): take it (taken, or busy), give it back (the owner only; started: a camera
 * start really happened, which keeps the other kind off for 5 s: camera_start_recent) */
enum { CAMERA_START_PALM = 1, CAMERA_START_GAZE = 2 };
int camera_start_take(u32 who);
void camera_start_give(u32 who, int started);
int camera_start_recent(u32 who); /* held by the other, or the other's start given back less than 5 s ago */
extern volatile u64 g_hand_lib_camera_at; /* ps5compat.c: the last [VrHand][CameraManager]/[ImageLock] line */
/* a PM4 type-3 draw opcode (gfx7/8; 0x2e is SI's DRAW_INDEX_IMMD) */
static inline int pm4_is_draw(u32 op) {
  switch (op) {
  case 0x24: case 0x25: case 0x27: case 0x2c: case 0x2d: case 0x2e: case 0x30: case 0x35: case 0x38:
    return 1;
  }
  return 0;
}
/* hooks.c: when the game's controller was last seen held by a system screen (0: never), and a
 * reading of it now, beside the game's (1 held, 0 not, -1 unknown) */
extern volatile u64 g_pad_intercepted_at;
int hooks_pad_held(void);

/* fov.c: PS VR2's eye tracking (agent.gaze) */
int fov_option(const char *key, u32 klen, const char *val, u32 vlen); /* 1: agent.gaze's line */
/* rtime.c (agent.rtime=1): a game thread's CPU time sampled at a hooked call of this kind */
enum { RT_SUBMIT = 1, RT_HANDOVER = 2, RT_PAD = 4, RT_WAIT = 8 };
void rtime_note(u32 kind, u32 n, const u32 *bytes);
int fov_wants_gaze(void);
void fov_preset_gaze(void); /* inset mode turns eye tracking on (multiview.c) */
void fov_gaze_start(int hmd, int after_hands); /* after_hands: wait for g_hand_settled */
int fov_gaze(float t[2]); /* the newest gaze as tangents (x right, y up), 1 while fresh */
u64 hooks_drawn_label(void); /* the game's newest drawn label (hooks.c) */
int hooks_cvar_int(const char *name, int *get, const int *set); /* a game variable by name (hooks.c) */
int hooks_cvar_float(const char *name, float *get, const float *set);
/* menu.c, the menu (the settings menu in a release build, the debug menu in a debug one) */
#define MENU_W 256u /* its picture: 32 characters of 8 by 16 lines of 12 */
#define MENU_H 208u
void menu_pad(u8 *sample);  /* the player's controller sample: L1+R1+touchpad opens and closes it */
const u8 *menu_picture(void); /* its picture (RGBA, MENU_W x MENU_H, GPU-readable) while open, else 0 */
/* the settings menu's file (release builds; the debug build's do nothing): hooks.c calls them */
void menu_boot(void);        /* at the end of cvars_fetch: the file fetched, its agent.* lines applied */
void menu_cvars_apply(void); /* at the end of apply_cvars: its lines for the game's variables, over cvars.txt's */
void menu_save(void);        /* the cvars thread, every 2 s: the file written when a choice changed */
void inset_menu_frame(const struct vr_frame *f, const s32 at[2][2], const float tan_rect[4], float dx);

/* acb.c: command-buffer pieces for compute work on Hmd2's queue (gen1_submit_acb) */
u32 acb_pm4(u32 op, u32 body);
u32 acb_release(u32 *cb, u32 n, u64 addr, u32 data_sel, u32 v);
u32 acb_timestamp(u32 *cb, u32 n, u64 addr); /* the GPU's clock, no cache actions */
u32 acb_acquire(u32 *cb, u32 n, int l2);
u32 acb_barrier(u32 *cb, u32 n, u64 label, u32 v);
/* inset.c: the alpha that makes the inset views Hmd2's second layer (multi-view step 4) */
int inset_alpha_frame(const struct vr_frame *f, const s32 rect[2][4]); /* once it is drawn */
int vr2_window_test(void); /* layer 1 kept to the insets' windows by Hmd2 (vr2.c) */
/* multiview.c: the inset views' rects in the texture and angles (left, right, up, down) as the
 * game drew them, when both were made lately */
/* (at: the frame's head pose time, the vr_frame's render_time; 0 the newest made) */
int multiview_layer(s32 rect[2][4], float tan[2][4], u64 at);
void multiview_axis(s32 axis[2][2], float ppt[4], u64 at); /* each eye's axis in the periphery and
                                                             * the pixels a unit of tangent */
void multiview_centres(float ctan[2][2], u64 at); /* each inset window's centre in tangent from its eye's axis */
void multiview_note_pose(u64 t); /* hooks.c: the tracker's head pose time given to this thread */
void multiview_hist_report(void); /* the frames' insets found by pose, logged */
extern volatile u32 g_blink_off; /* vr2.c (agent.insetblink): the newest frame passed without the insets' layer */
extern volatile u64 g_blink_at;

/* mirror.c: the headset picture on the TV */
void mirror_start(void);
void mirror_note_port(s32 port, const void *attr);

/* hooks.c: the game's own VideoOut buffer registration (PS4 attribute) */
void hooks_log_audio(void);
void hooks_log_dmem(const char *when); /* hooks.c: the game's direct memory, and what is free */
void hooks_probe_hidden_dmem(const char *when); /* hooks.c: the container's extra segment (probe) */
void hooks_video_queues(void);
s32 hooks_register_buffers(s32 port, s32 start, void *const *addrs, s32 n, const void *attr);
