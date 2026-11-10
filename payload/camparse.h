/* The kernel log's lines into the headset cameras' state (common/camera.h): one function, pure,
 * so tools/hand/camparse_test.c runs it on the host; payload/main.c feeds it the kernel message
 * buffer's new lines. The line texts: the kernel's "hmd2_image: ..." (no prefix) and the
 * userland's "<118>[XFD] ...", "<118>[SceVrTracker2]...", "<118>[LoginMgr] ...",
 * "<118>[SceShellUI] ..." (tools/hand/gate_check.py keeps the same rules for the old logs).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdio.h>
#include <string.h>
#include "../common/camera.h"

/* the system's VR screens that come with its camera work (the kernel log's
 * "OnFocusActiveSceneChanged [...] -> [<scene> : ...]" lines from SceShellUI) */
static int cam_system_scene_n(const char *scene, size_t n) { /* (within n bytes) */
  static const char *const k[] = {"SafetyNotice", "PlayArea", "TrackingLost", "OutOfPlayArea",
                                  "VrControllerRecommend", "CanNotFindPlayArea", "CommonScanning",
                                  "ChooseStandingOrSitting", "AdjustHeight", "CameraView",
                                  "FirstCamera"};
  for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
    size_t kl = strlen(k[i]);
    for (size_t at = 0; at + kl <= n; at++)
      if (scene[at] == k[i][0] && memcmp(scene + at, k[i], kl) == 0)
        return 1;
  }
  return 0;
}
static int cam_system_scene(const char *scene) { return cam_system_scene_n(scene, strlen(scene)); }

/* The level states not known: at a new game, and whenever lines were lost (the anchor marker gone
 * from the buffer, the buffer replaced). Only lines parsed after this establish them again. The
 * caller holds the lock. */
static inline void cam_levels_unknown(struct cam_state *c, uint64_t now) {
  c->xfd = CAM_XFD_NONE;
  c->img_known = 0;
  c->img_mode = 0, c->img_pid = 0, c->img_pri = CAM_PRI_NONE, c->img_at = 0, c->img_poll_at = 0;
  c->img_new = 0;
  c->screen_up = 0;
  c->levels_since = now;
}

/* A new game served (its pid): the levels not known, the markers' counts over. */
static inline void cam_new_game(struct cam_state *c, uint32_t pid, uint64_t now) {
  c->game_pid = pid;
  cam_levels_unknown(c, now);
  c->mark_written_seq = c->mark_seen_seq = 0;
  c->mark_written_at = c->mark_seen_at = 0;
}

/* The buffer's history, for the LEVELS only (an adopted game: a payload sent over the one that
 * served it; the system's state had no loss, only our reading): the lines parsed into a copy,
 * the camera job and the stream taken from it, stamped now (the floors apply from here: the
 * quiet floor too), the stream line not as new text. The caller holds the lock. */
static inline void cam_levels_from_history(struct cam_state *c, const struct cam_state *parsed,
                                           uint64_t now) {
  c->xfd = parsed->xfd, c->xfd_at = parsed->xfd == CAM_XFD_NONE ? 0 : now;
  c->img_known = parsed->img_known, c->img_mode = parsed->img_mode, c->img_pid = parsed->img_pid,
  c->img_pri = parsed->img_pri, c->img_at = parsed->img_known ? now : 0, c->img_poll_at = now;
  c->img_new = 0;
  c->screen_up = parsed->screen_up;
  c->mount = parsed->mount;
  c->bringup_at = now;
}

/* one kernel log line (without its newline), into *c (the caller holds its lock).
 * now: the line's time (the poll's, each line a microsecond on); poll_at: the poll's; fresh: the
 * line is new text (not read after a loss) */
static void cam_parse_line(struct cam_state *c, const char *l, uint64_t now, uint64_t poll_at,
                           int fresh) {
  if (l[0] == '<') { /* a syslog priority: <118> is the userland's (SceShellCore, XFD, VrTracker2,
                      * LoginMgr...), <6> and the like the kernel's own */
    const char *e = strchr(l, '>');
    if (e && e - l <= 4) {
      if (strncmp(l, "<118>", 5) == 0)
        c->userland = 1;
      l = e + 1;
    }
  }
  if (!c->userland && (strstr(l, "[XFD]") || strstr(l, "[SceShellCore]") ||
                       strstr(l, "[SceVrTracker2]") || strstr(l, "[LoginMgr]")))
    c->userland = 1;
  const char *p;
  c->lines++;
  c->line_at = now;
  if (strncmp(l, "hmd2_image: ", 12) == 0) {
    unsigned pid = 0, mode = 0;
    int pri = CAM_PRI_NONE;
    unsigned long ts = 0;
    if (sscanf(l + 12, "pid %u, pri %d, ts %lu, mode %u", &pid, &pri, &ts, &mode) == 4) {
      /* a game's own stream (pri -1) counts only as this game's: an earlier game's line in the
       * buffer's history is nobody's now */
      if (pri == -1 && pid != c->game_pid)
        return;
      c->img_mode = mode, c->img_pid = pid, c->img_pri = pri, c->img_at = now;
      c->img_poll_at = poll_at, c->img_new = (uint32_t)fresh, c->img_known = 1;
      if (pri != -1)
        c->sys_cam_at = now, c->bringup_at = now;
    } else if (strncmp(l + 12, "mode 1", 6) == 0 && (l[18] == 0 || l[18] == ' ')) {
      c->img_mode = 1, c->img_pid = 0, c->img_pri = CAM_PRI_NONE, c->img_at = now;
      c->img_poll_at = poll_at, c->img_new = (uint32_t)fresh, c->img_known = 1;
      c->sys_cam_at = now;
      c->bringup_at = now;
    }
  } else if ((p = strstr(l, "[XFD] VrCamera StateChanged: "))) {
    p += sizeof "[XFD] VrCamera StateChanged: " - 1;
    int idle = strcmp(p, "Stop") == 0 || strcmp(p, "state Stop") == 0 || strcmp(p, "NonVR") == 0;
    c->xfd = idle ? CAM_XFD_IDLE : CAM_XFD_BUSY;
    c->xfd_at = now;
    c->bringup_at = now;
    /* the stream, when not known, known with the job: nobody's once the job is over, the
     * system's while it runs. A known stream is the kernel's word: its hmd2_image lines alone
     * change it, the latest deciding (every system open is followed by "mode 1" or a new owner's
     * line in every log: it cannot stick); an idle job line never overrides a parsed open. The
     * residual (common/camera.h): after a real loss of lines, an idle line may call nobody's a
     * stream whose open line was among the lost */
    if (!c->img_known) {
      if (idle)
        c->img_known = 1, c->img_mode = 1, c->img_pid = 0, c->img_pri = CAM_PRI_NONE;
      else
        c->img_known = 1, c->img_mode = 16, c->img_pid = 0, c->img_pri = 0;
    }
  } else if ((p = strstr(l, "[Gen2VrSafetyNotice] "))) {
    p += sizeof "[Gen2VrSafetyNotice] " - 1;
    c->screen_at = now, c->bringup_at = now;
    if (strcmp(p, "SafetyNoticeFinished") == 0)
      c->notice_end_at = now;
  } else if (strstr(l, "[ScePlayAreaEditor] success to set playarea") ||
             strstr(l, "[XFD] PlayAreaEdit State Changed ")) {
    c->screen_at = now, c->bringup_at = now;
  } else if ((p = strstr(l, "OnFocusActiveSceneChanged "))) {
    /* a system VR screen taking or giving back the focus: an event either way, and the flag
     * (set by a change to one, cleared by a change from one to another kind) */
    const char *to = strstr(p, "-> [");
    int from_sys = cam_system_scene_n(p, to ? (size_t)(to - p) : strlen(p));
    int to_sys = to && cam_system_scene(to + 4);
    if (from_sys || to_sys) {
      c->screen_at = now, c->bringup_at = now;
      c->screen_up = (uint32_t)to_sys;
      if (to_sys && strstr(to + 4, "TrackingLost"))
        c->relocal_at = now;
    }
  } else if (strstr(l, "[XFD] First time tracking stable")) {
    c->stable_at = now, c->stable_after_reset = 1;
  } else if (strstr(l, "[SceVrTracker2][Ins] Detect SLAM Reset")) {
    c->slam_at = now, c->relocal_at = now, c->restart_at = now, c->stable_after_reset = 0;
  } else if (strstr(l, "[SceVrTracker2][Ins] Map Latch") ||
             strstr(l, "[SceVrTracker2][Ins] INS latched Slam delta pose")) {
    c->relocal_at = now;
  } else if ((p = strstr(l, "[PsvrMgr] SetHmdMountStatus "))) {
    unsigned m = (unsigned)(p[sizeof "[PsvrMgr] SetHmdMountStatus " - 1] - '0');
    if (m != c->mount)
      c->mount_at = now;
    c->mount = m;
  }
}
