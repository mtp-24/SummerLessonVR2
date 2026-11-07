/* SummerLessonVR2 agent: freestanding code that runs inside the PS4 game process.
 * The payload (payload/main.c) maps it, applies its relative relocations,
 * resolves its imports by NID in the game's modules, points the game's GOT
 * slots for the hooked functions at it and saves the old values in the
 * hooks' "orig" pointers.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nids_gen.h"

typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;
typedef uint64_t u64;

#define AGENT_MAGIC 0x32524f5254534100ull /* "\0ASTROR2": header has vr_state */
#define LOG_SIZE (256 * 1024)

struct agent_import {
  const char *module;
  const char *nid;
  void **slot;
};

struct agent_hook {
  const char *nid;
  void *hook;
  void **orig;
  u64 slot; /* game VA of the eboot GOT slot (filled by the injector, 0 = not imported) */
};

/* Shared with the injector, which polls it with kernel reads. */
struct agent_shared {
  volatile u64 log_wpos; /* bytes ever written; the ring holds the last LOG_SIZE */
  volatile u32 log_lock;
  volatile u32 boot_done;
  volatile u32 cmd;      /* written by the injector */
  volatile u32 cmd_seq;
  volatile u64 cmd_arg[4];
  volatile u32 cmd_ack;
  volatile u32 repatch;  /* set by the agent: GOT needs re-pointing (injector does it) */
  char log[LOG_SIZE];
};

/* Hmd2's reprojection shader as GCN code: built on the console by the injector (from the
 * console's own libSceHmd2) and written here before the agent is mapped. */
#define GCN_MAX_DWORDS 0x1400
struct agent_gcn {
  u32 magic, dwords, rsrc1_vgprs, rsrc1_sgprs;
  u32 code[GCN_MAX_DWORDS];
};

struct agent_header {
  u64 magic;
  struct agent_import *imports;
  u32 nimports;
  struct agent_hook *hooks;
  u32 nhooks;
  struct agent_shared *shared;
  u64 (*boot)(u64); /* called by the injector as it loads the agent: agent.multiview=N (only at the
                     * entry point), returns the agent's patch table or 0 (multiview.c) */
  void *vr_state;     /* struct vr_state; its first vr_persist bytes survive a hot swap */
  u32 vr_persist;
  void *unbound_stub; /* what CMD_STUB points unbound imports at */
  struct agent_gcn *gcn;
  u32 ipd_um; /* written by the injector: the eye distance saved by CMD_SAVE_IPD, 0 = none */
  u32 firmware; /* injector-selected profile, formerly padding; header size is unchanged */
  u64 game_base;  /* written by the injector: where the game's eboot is */
  u32 frame_pace; /* written by the injector: refreshes a frame is shown for (2, or 1 for 90 fps
                   * with the game's time step following its frames: framerate.txt) */
  u32 refresh_hz; /* written by the injector: the headset's refresh rate, 90 or 0 for 120 (and
                   * by vr2.c, 0 with frame_pace 2, where PS VR2 refuses its 90 Hz mode) */
  u32 screen_pct; /* written by the injector: the bits of a float, the r.ScreenPercentage the
                   * game is given once its frames run (0: its own 130). Its eye is the half of
                   * its 1920x1080 screen times that: 960x1080 x SP/100 (resolution.txt) */
  u32 viewport; /* written by the injector: the game's PS4 RHI viewport object (a static), from
                 * the eboot's base, for its frames' GPU labels (hooks.c); 0: unknown build */
  /* written by the injector, the build's other statics from the eboot's base (payload k_builds;
   * 0: unknown, not read): Unreal's IConsoleManager singleton (cvars.txt), GEngine, t.MaxFPS's
   * value pointer, the game's Neo-mode answer (its question at +8), r.ScreenPercentage's data as
   * the plugin caches it (resolution.txt), FApp::DeltaTime */
  u32 console_mgr, engine, max_fps, neo, sp_cache, delta_time;
};
extern struct agent_header astro_hdr;

struct size_align {
  u64 size, align;
};

#define CMD_QUIT 1 /* the agent's threads end; vr2.c turns the VR mode off and then writes
                    * STOP_DONE into cmd_arg[3] (the injector kills the game after that) */
#define STOP_DONE 0x51554954ull
#define CMD_RESOLVE 2 /* injector: resolve every import slot that is still zero */
#define CMD_STUB 5    /* injector: unbound imports of modules cmd_arg[0..3] -> unbound_stub */
#define CMD_OVERRIDE 6 /* injector: module cmd_arg[0]'s import of NID (char *)cmd_arg[1] -> cmd_arg[2] */
#define CMD_STAGE 7 /* injector: make PS5 lib cmd_arg[0] (0-3) visible in the game's sandbox as
                     * /summerlessonvr2/<name>; cmd_arg[1] = 1: done with it */
#define CMD_SAVE_IPD 8 /* injector: keep cmd_arg[0] (micrometres) for the next start */
#define CMD_NOTIFY 9 /* injector: show (char *)cmd_arg[0] as a PS5 notification */
#define CMD_MODEL 10 /* injector: libSceVrHand model cmd_arg[0] into cmd_arg[1] (cmd_arg[2] bytes);
                      * cmd_arg[3] = bytes read, -1 failed, -2 buffer too small */
#define CMD_CVARS 12 /* injector: cvars.txt into (char *)cmd_arg[0] (cmd_arg[1] bytes, NUL-ended) */
#define CMD_CAMERA 11 /* injector: cmd_arg[0] = 1: let the game open the headset cameras (raise its
                       * credentials), 0: done, its own back; cmd_arg[0] = 0 or -1 failed */
/* a request to the injector, from any thread (core.c) */
int agent_call(u32 cmd, u64 a0, u64 a1, u64 a2, u64 a3, u32 wait_ms, u32 k, u64 *out);

/* Imports: one function pointer per entry of imports.def. */
#define IMP(ret, name, args) extern ret(*name) args;
#include "imports.inc"
#undef IMP

void agent_log(const char *fmt, ...);
int agent_vsnprintf(char *out, size_t n, const char *fmt, __builtin_va_list ap);
int agent_snprintf(char *out, size_t n, const char *fmt, ...); /* returns what it wrote */
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

extern struct agent_shared g_shared;
extern char g_cvars_text[2048]; /* cvars.txt, fetched from the injector (CMD_CVARS) and applied by hooks.c */
extern u32 g_cvars_len;
extern volatile u32 g_refresh_hz; /* the headset's refresh as Hmd2 runs it (vr2.c): 90 or 120; 0 before */
/* the agent's own options, "agent.<name>=<value>" lines of cvars.txt, read before PS VR2 starts */
extern u32 g_opt_hmdrate;    /* experiment: agent.hmdrate=60|120, Hmd2's half or full rate whatever the game's */
extern u32 g_opt_cap;        /* agent.cap=<fps>: the engine's cap in the native modes (default: the refresh,
                              * 120 or 90, which keeps the time step regular) */
extern u32 g_opt_mirrorboth, g_opt_tvhmd; /* agent.mirrorboth=1: the TV shows both eyes as drawn */
extern u32 g_multiview_rows; /* multiview.c: rows below the eyes for the insets, 0 = off */
extern volatile u32 g_opt_probe; /* agent.probe=1: the eyes' and insets' brightness in the log, 2 also the insets' edges (mirror.c), live */
extern volatile u32 g_opt_capture; /* agent.capture=N: a TV capture into the log each new N (mirror.c), live */
extern volatile u32 g_opt_insetdebug; /* agent.insetdebug=1 (inset.c), live */
extern volatile u32 g_opt_gazedot; /* agent.gazedot=1: a ring at the gaze (multiview.c), live */
extern volatile u32 g_opt_insetaam; /* agent.insetaam: the insets' own AA method 0 none, 1 FXAA, 2 the engine's (multiview.c), live */
extern volatile u32 g_opt_gazefilter, g_opt_gazemincut, g_opt_gazebeta; /* the gaze window's smoothing (multiview.c), live */
extern volatile u32 g_opt_vignette; /* agent.vignette: 0 none (default), 1 the eyes', 2 every view's (multiview.c), live */
extern volatile u32 g_opt_gazehold; /* agent.gazehold=N: the gaze window held within N/1000 of tangent (multiview.c), live */
extern volatile u32 g_opt_insetblink; /* agent.insetblink=N: the insets' layer off every other N ms (vr2.c), live */
extern volatile u32 g_opt_insetpp; /* agent.insetpp: the eye's bloom (1) and AO (2) in the insets (insetpp.c), live */
void insetpp_boot(u64 game_base); /* insetpp.c */
void insetpp_report(void);
void mv_tm(u64 pass, u64 ctx);
void mv_tmf(u64 pass, u64 ctx);
void mv_ao(u64 pass, u64 ctx);
extern const u8 k_insetpp_tm[14], k_insetpp_ao[14];
extern const u32 k_insetpp_at[3];
extern volatile u32 g_opt_insetwin; /* agent.insetwin: 1 registered, 2 windows off view, 3 in use (vr2.c) */
extern volatile u32 g_opt_insetonly; /* agent.insetonly=1|2: one eye's inset layer only (multiview.c), live */
extern volatile u32 g_opt_insetgaze; /* agent.insetgaze=1|2: the insets follow the gaze / a sweep (multiview.c), live */
extern volatile u32 g_opt_insetppt; /* agent.insetppt=D: the insets' pixels a unit of tangent (multiview.c), live */
extern volatile u32 g_opt_insetsize; /* agent.insetsize=S: the insets' size (multiview.c), live */
extern volatile u32 g_opt_insetwidth; /* agent.insetwidth=W: the insets' width (0: their size; multiview.c), live */
extern volatile u32 g_opt_insetpos; /* agent.insetpos=0: the insets from the head's centre (multiview.c), live */
extern volatile u32 g_opt_insetea; /* agent.insetea=0: the insets' own eye adaptation passes run (multiview.c), live */
extern volatile u32 g_opt_insettlv; /* agent.insettlv=0: the insets' own translucency volumes (multiview.c), live */
extern volatile u32 g_opt_inseteye; /* agent.inseteye=0: the insets without the eyes' exposure (multiview.c), live */
extern volatile u32 g_opt_insetstate; /* agent.insetstate=1: the insets' own view states (multiview.c), live */
extern volatile u32 g_opt_insetviews; /* agent.insetviews=1: the two inset views (multiview.c), live */
u64 hooks_frame_count(void);   /* the game's count for the frame being handed over (hooks.c) */
int hooks_frame_drawn(u64 count); /* the game's GPU has finished the frame of this count */
int hooks_flip_state(s32 vo, s32 *pending, s32 *shown); /* a port's real flips pending, buffer shown */
extern u32 g_opt_tvblank;    /* agent.tvblank=1: the TV shows black; the mirror still flips its (black)
                              * buffers at the same pace, which the display path needs (off: a hang) */
/* debug tools, off unless cvars.txt says agent.<name>=1: fingerprint (what each headset refresh
 * read, mirror.c), trace (each frame's way to the headset, trace.c), waits (the game's long
 * waits, hooks.c) */
extern u32 g_opt_fingerprint, g_opt_trace, g_opt_waits, g_opt_rtime;
extern u32 g_opt_recentre;   /* agent.recentre=1: the first press of circle recentres (off by default) */
extern u32 g_opt_newest, g_opt_predict; /* agent.newest=0: the native modes' frames passed in
                                         * order, not the newest (vr2.c game_loop) */
extern u32 g_opt_hands; /* the palm tracker: 60, 30, or 0 (the controller stays where the game put
                         * it). (A "mirror off" option brought the console down on 2026-11-01 00:42,
                         * measurement 30: the mirror also keeps the game's main port flipping) */
extern u32 g_opt_handwatch; /* agent.handwatch=1: the palm tracker's start rule watched and logged
                             * all session, the palm tracker never started (hand.c) */
void cvars_fetch(void);   /* the text from the injector (CMD_CVARS), once */
extern struct agent_gcn g_gcn;

#define LOG(...) agent_log(__VA_ARGS__)
/* the periodic detail (timings, pose figures, censuses, per-10 s counts beyond the frames' line):
 * left out of a release build (make RELEASE=1), which logs start-up, errors and one frames line
 * every 10 s */
#ifdef RELEASE
#define LOG_DETAIL(...) do { if (0) agent_log(__VA_ARGS__); } while (0) /* compiled away */
#else
#define LOG_DETAIL(...) agent_log(__VA_ARGS__)
#endif
