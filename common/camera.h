/* The headset cameras as the kernel log tells it: the injector polls the kernel message buffer
 * (sysctl kern.msgbuf, which consumes nothing: /dev/klog hands each line to one reader only, and
 * klogsrv may be one; payload/main.c msgbuf_thread, payload/msgbuf.h, with the injector's own
 * marker lines in the log as the anchors) and keeps this record,
 * written into the agent's shared block (struct agent_shared .cam, after the log ring) on every
 * tick of its serve loop; the agent's palm-tracker gate reads it (agent/hand.c gate_eval). A start of the palm tracker that overlaps the system's own
 * headset-camera work (the safety notice, a play-area setup, the tracking-lost screen's camera job)
 * has taken the console down; a running tracker survives all of it (docs/HOW-IT-WORKS.md 9).
 * Times are the injector's CLOCK_MONOTONIC in microseconds, 0 = never seen; `now` is when the
 * block was written, so an age is now - t, and the agent lines the clock up with the game's process
 * time by the block's `now` against its own. An older injector leaves the block zero (version 0):
 * no kernel log, the gate never clears.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

#define CAM_STATE_VERSION 9 /* 3: seq as a seqlock (odd while written), game_pid; 4: instance,
                             * the marker lines; 5: the levels unknown at a new game, levels_since,
                             * stable_after_reset; 6: img_known, img_poll_at, img_new, the levels
                             * unknown after every loss; 7: xfd_job_seen; 8: the screens, the
                             * notice and a setup as events only (screen_at), the levels only the
                             * camera job and the stream; 9: screen_up */

/* xfd: the system's camera job ([XFD] VrCamera StateChanged) */
enum { CAM_XFD_NONE = 0, CAM_XFD_IDLE = 1, CAM_XFD_BUSY = 2 }; /* NONE: not known yet */
#define CAM_PRI_NONE 0x7fffffff

struct cam_state {
  uint32_t version; /* CAM_STATE_VERSION */
  uint32_t size;    /* sizeof(struct cam_state) */
  uint64_t seq;     /* a seqlock: +2 a write, odd while the injector writes the block (it writes
                     * seq odd, the body, seq even, three copyins in that order; the agent takes a
                     * copy only with the same even seq before and after it) */
  uint64_t now;     /* when written */
  uint64_t instance;  /* the injector's instance (random at its start): a newer injector adopting
                       * the game restarts seq, and the agent starts over with it */
  uint64_t feed_at;   /* the last kern.msgbuf snapshot read: the reader is alive */
  /* the injector's own marker lines in the kernel log (MSGBUF_MARK "<seq> <nonce>", once a second
   * the whole time a game is served): the anchors of the reading, and the proof that the buffer
   * is read live end to end. The agent is not clear while the newest written one has not been
   * seen within 3 s (GATE_MARK_US), or none was ever seen */
  uint32_t mark_written_seq, mark_seen_seq;
  uint64_t mark_written_at, mark_seen_at;
  uint64_t line_at;   /* the last kernel log line parsed */
  uint64_t missed_at; /* the last snapshot whose lines were possibly missed: the first (a history
                       * of unknown age), the anchor marker gone (the buffer overwritten faster than
                       * the polls), the text after it shorter than what was parsed */
  uint32_t userland;  /* 1 once a userland line (<118>[SceShellCore], [XFD], [SceVrTracker2]...)
                       * was seen in the buffer: it carries what the gate needs */
  uint32_t msgbuf_size; /* kern.msgbuf's size (bytes); 0 before the first read */
  uint32_t game_pid;    /* the game the block is written into: its own "hmd2_image: pid N, pri -1"
                         * line is the palm tracker's stream (an earlier game's is not) */
  uint32_t pad0;
  /* the image stream (/dev/hmd2_image): the kernel's "hmd2_image: pid N, pri P, ts T, mode M"
   * (pri -1 the game's own tracker, 0 the system's camera process; mode 16 the system's job, 4
   * the palm tracker's) and "hmd2_image: mode 1" (closed, idle) */
  uint64_t img_at;
  uint64_t img_poll_at; /* when the poll that parsed the line began (its `now`, no line offset) */
  uint32_t img_known;   /* 0: not known since a new game or lost lines: no hmd2_image line, and no
                         * VrCamera Stop/state Stop/NonVR (the system's job over) parsed since */
  uint32_t img_new;     /* 1: the line was parsed as new text (never from a re-reading) */
  uint32_t img_mode, img_pid;
  int32_t img_pri; /* CAM_PRI_NONE: none (mode 1, or never seen) */
  uint32_t xfd;            /* CAM_XFD_* */
  uint32_t pad2;
  uint64_t xfd_at;         /* the last state change */
  uint64_t sys_cam_at;     /* the last system-side open or mode change of the stream (not the game's) */
  /* The system's screens (the safety notice, a play-area setup, the tracking-lost and
   * out-of-play-area screens) are no levels here: the game's own system status says whether one
   * is up (sceSystemServiceGetStatus: overlaid, in the background, out of the play area; the
   * controller held by the system), straight from the system, never unknown. Their log lines are
   * events: screen_at, the last of any, which feeds the quiet floor (bringup_at). The levels are
   * the camera job (xfd) and the image stream (img_*): no API tells those; they are "not known"
   * at each new game and after every loss of lines. The job is known from any VrCamera line. The
   * stream, once known, is the kernel's word alone: its hmd2_image lines (an open with its owner,
   * "mode 1" closed), the latest one deciding; a VrCamera line never changes a known stream. A
   * stream NOT known is established by the next line of either kind: an hmd2_image line as it
   * says, a VrCamera idle line as nobody's, a busy one as the system's. The agent never starts
   * with them unknown. levels_since: when they were last set unknown.
   * Two residuals, both needing a real loss of lines (which play never comes near: the worst
   * 200 ms of kernel log in any run is 41 KB, 98 KB at a boot, against this 128 KB buffer, and
   * the markers are 1 s apart): after a loss, a VrCamera idle line may establish the stream as
   * nobody's while the system's open line was among the lost (the system's job then ends with
   * its own lines, and the 12 s floor runs from each); and screen_up is cleared by a loss while
   * a system screen is up (the game's own status, the authority, still holds the start). A known
   * system stream never sticks: in every kernel log here each system open (mode 16) is followed
   * by "hmd2_image: mode 1" (47) or a new owner's line (40), never by silence. */
  uint64_t screen_at;
  uint32_t screen_up;      /* a system VR screen has the focus, by the focus lines alone: set by a
                            * change to one, cleared by a change away from one to another kind,
                            * and cleared (never unknown) at a new game or lost lines: the second
                            * signal beside the game's status; the gate is not clear while set */
  uint32_t pad3;
  uint64_t levels_since;
  uint32_t stable_after_reset; /* "First time tracking stable" seen after the last SLAM reset (a
                                * reset clears it): the order within one poll kept */
  uint32_t pad1;
  uint32_t mount;          /* SetHmdMountStatus: 0 off, 1 worn, 0xff unknown */
  uint64_t notice_end_at;  /* the last SafetyNoticeFinished */
  uint64_t bringup_at;     /* the quiet floor: the last line of any system camera or VR-screen
                            * kind (a VrCamera line, the system's hmd2_image lines, the notice's,
                            * a play-area setup's, a focus change to or from a system VR screen) */
  uint64_t stable_at;      /* the last "[XFD] First time tracking stable" */
  uint64_t slam_at;        /* the last "[SceVrTracker2][Ins] Detect SLAM Reset" */
  uint64_t restart_at;     /* the last SLAM reset: the tracker starts over, "tracking stable" is
                            * expected after it (not after "Don't need show": a headset-off replay
                            * logs neither a reset nor stable after it, c-stock's hang the same) */
  uint64_t relocal_at;     /* the last sign of the tracker relocalising, of the kinds seen 0-3 s
                            * before a tracking-lost camera job: SLAM Reset, Map Latch, INS latched
                            * Slam delta pose, the tracking-lost screen (ResetLocalCoordinate, Ins
                            * instance is null and IMU samples drop never led one: not counted) */
  uint64_t mount_at;       /* the last SetHmdMountStatus change */
  uint64_t scene_at;       /* the last system VR screen change */
  uint32_t lines;          /* kernel log lines parsed */
  uint32_t polls;          /* kern.msgbuf snapshots taken */
  uint32_t fails;          /* kern.msgbuf reads that failed */
  uint32_t misses;         /* snapshots with lines possibly missed (the first one included) */
};
