/* The kernel log parser (payload/camparse.h) on the host: the userland mark only for <118> lines
 * or the known userland tags; the two log-derived levels (the camera job, the image stream),
 * unknown at a new game and after lost lines, known again from any VrCamera or hmd2_image line;
 * the system's screens as events only (the quiet floor); the tracker's lines; an adopted game.
 *   cc -O2 -o /tmp/camparse_test tools/hand/camparse_test.c && /tmp/camparse_test
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include "../../payload/camparse.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
/* a line as new text of a poll begun at its time */
static void LINE(struct cam_state *c, const char *l, uint64_t t) { cam_parse_line(c, l, t, t, 1); }

int main(void) {
  struct cam_state c = {.version = CAM_STATE_VERSION, .size = sizeof c, .img_pri = CAM_PRI_NONE, .mount = 0xff,
                        .game_pid = 4189};
  uint64_t t = 1000;
  /* the kernel's own lines: no userland mark */
  LINE(&c, "<6>[KERNEL] WARNING: called fdescfree(), but remain BUDGET_FD_FILE (ret=426)", t);
  LINE(&c, "hmd2_image0: <PS VR2 Data 4> on usbus2", t);
  CHECK(c.userland == 0 && c.lines == 2);
  /* the image stream: the system's job, the game's own, closed */
  LINE(&c, "hmd2_image: pid 61, pri 0, ts 244220965, mode 16", t += 100);
  CHECK(c.img_pid == 61 && c.img_pri == 0 && c.img_mode == 16 && c.img_at == t && c.sys_cam_at == t && c.userland == 0);
  CHECK(c.img_known == 1 && c.img_new == 1 && c.img_poll_at == t && c.bringup_at == t);
  LINE(&c, "hmd2_image: pid 4189, pri -1, ts 73177604405, mode 4", t += 100);
  CHECK(c.img_pid == 4189 && c.img_pri == -1 && c.img_mode == 4 && c.sys_cam_at == t - 100 && c.bringup_at == t - 100);
  LINE(&c, "hmd2_image: pid 994, pri -1, ts 3381761490, mode 4", t += 100); /* an earlier game's */
  CHECK(c.img_pid == 4189 && c.img_at == t - 100);
  c.game_pid = 0; /* no game served: no game's own line counts */
  LINE(&c, "hmd2_image: pid 4189, pri -1, ts 73177604405, mode 4", t += 100);
  CHECK(c.img_at == t - 200);
  c.game_pid = 4189;
  LINE(&c, "hmd2_image: mode 1", t += 100);
  CHECK(c.img_mode == 1 && c.img_pri == CAM_PRI_NONE && c.bringup_at == t);
  LINE(&c, "hmd2_image: mode 16", t += 100); /* (not a line the kernel writes: ignored) */
  CHECK(c.img_mode == 1);
  /* userland lines: <118>, or a known tag without a prefix */
  LINE(&c, "<118>[XFD] VrCamera StateChanged: StartJobRunning", t += 100);
  CHECK(c.userland == 1 && c.xfd == CAM_XFD_BUSY && c.xfd_at == t && c.bringup_at == t);
  c.userland = 0;
  LINE(&c, "[SceVrTracker2][Ins] Detect SLAM Reset (cnt 7)", t += 100);
  CHECK(c.userland == 1 && c.slam_at == t && c.relocal_at == t && c.restart_at == t);
  c.userland = 0;
  LINE(&c, "<7>[LoginMgr] [Gen2VrSafetyNotice] Start", t += 100); /* an odd prefix, a known tag */
  CHECK(c.userland == 1 && c.screen_at == t && c.bringup_at == t);
  /* the camera job's states, the stream with it */
  LINE(&c, "<118>[XFD] VrCamera StateChanged: Running", t += 100);
  CHECK(c.xfd == CAM_XFD_BUSY && c.img_mode == 1); /* (the stream known already: left as it is) */
  LINE(&c, "<118>[XFD] VrCamera StateChanged: state Stop", t += 100);
  CHECK(c.xfd == CAM_XFD_IDLE && c.bringup_at == t && c.img_mode == 1 && c.img_pri == CAM_PRI_NONE);
  LINE(&c, "hmd2_image: pid 61, pri 0, ts 244220965, mode 16", t += 100);
  LINE(&c, "<118>[XFD] VrCamera StateChanged: NonVR", t += 100); /* (1) an idle line never overrides a parsed open */
  CHECK(c.xfd == CAM_XFD_IDLE && c.bringup_at == t && c.img_mode == 16 && c.img_pri == 0);
  LINE(&c, "hmd2_image: mode 1", t += 100); /* only the kernel's close does */
  CHECK(c.img_mode == 1 && c.img_pri == CAM_PRI_NONE);
  LINE(&c, "hmd2_image: pid 4189, pri -1, ts 1, mode 4", t += 100);
  LINE(&c, "<118>[XFD] VrCamera StateChanged: state Stop", t += 100); /* the game's own stream stays */
  CHECK(c.img_pri == -1 && c.img_mode == 4);
  /* the system's screens: events only, each one the quiet floor */
  LINE(&c, "<118>[LoginMgr] [Gen2VrSafetyNotice] SafetyNoticeStart", t += 100);
  CHECK(c.screen_at == t && c.bringup_at == t);
  LINE(&c, "<118>[LoginMgr] [Gen2VrSafetyNotice] SafetyNoticeFinished", t += 100);
  CHECK(c.notice_end_at == t && c.bringup_at == t);
  LINE(&c, "<118>[LoginMgr] [Gen2VrSafetyNotice] Don't need show SafetyNotice.", t += 100);
  CHECK(c.bringup_at == t && c.restart_at != t);
  LINE(&c, "<118>[SceShellUI] I/PSM.UI : OnFocusActiveSceneChanged [AppScreen : ApplicationScreenScene] -> [TrackingLostModalScene : TrackingLostModalScene]", t += 100);
  CHECK(c.screen_at == t && c.bringup_at == t && c.relocal_at == t && c.screen_up == 1); /* (3) the flag */
  LINE(&c, "<118>[XFD] PlayAreaEdit State Changed Closing -> Loading", t += 100);
  CHECK(c.screen_at == t && c.bringup_at == t);
  LINE(&c, "<118>[SceShellUI] I/PSM.UI : OnFocusActiveSceneChanged [TrackingLostModalScene : X] -> [CanNotFindPlayAreaModalScene : Y]", t += 100);
  CHECK(c.screen_up == 1); /* one system screen to another: still up */
  LINE(&c, "<118>[SceShellUI] I/PSM.UI : OnFocusActiveSceneChanged [CanNotFindPlayAreaModalScene : X] -> [AppScreen : ApplicationScreenScene]", t += 100);
  CHECK(c.screen_at == t && c.bringup_at == t && c.screen_up == 0); /* (from a system screen: an event too, the flag off) */
  LINE(&c, "<118>[SceShellUI] I/PSM.UI : OnFocusActiveSceneChanged [AppScreen : X] -> [ReactOptionsMenu : Y]", t += 100);
  CHECK(c.screen_at == t - 100 && c.bringup_at == t - 100 && c.screen_up == 0); /* (the PS menu: not a VR screen) */
  LINE(&c, "<118>[SceShellUI] I/PSM.UI : OnFocusActiveSceneChanged [AppScreen : X] -> [OutOfPlayAreaModalScene : Y]", t += 100);
  CHECK(c.screen_up == 1);
  LINE(&c, "<118>[SceShellUI] I/PSM.UI : OnFocusActiveSceneChanged [AppScreen : ApplicationScreenScene] -> [Render.NPXS40002 : AppScene]", t += 100);
  CHECK(c.screen_up == 1 && c.screen_at == t - 100); /* (9) neither side a system screen: no event, the flag as it was */
  cam_levels_unknown(&c, t += 100); /* a loss or a new game: the flag off, never unknown */
  CHECK(c.screen_up == 0);
  LINE(&c, "<118>[ScePlayAreaEditor] success to set playarea!", t += 100);
  CHECK(c.screen_at == t && c.bringup_at == t);
  /* the tracker's lines */
  LINE(&c, "<118>[SceVrTracker2][Ins] Map Latch 38f90089", t += 100);
  CHECK(c.relocal_at == t);
  LINE(&c, "<118>[SceVrTracker2][Ins] INS latched Slam delta pose", t += 100);
  CHECK(c.relocal_at == t);
  LINE(&c, "<118>[SceVrTracker2][Ins] IMU: detected 15680 samples drop.", t += 100);
  LINE(&c, "<118>[SceVrTracker2] ResetLocalCoordinate at MapLatch and 6DoF", t += 100);
  LINE(&c, "<118>[SceVrTracker2][Ins] Ins instance is null", t += 100);
  CHECK(c.relocal_at == t - 300); /* not relocalising signs */
  LINE(&c, "<118>[XFD] First time tracking stable. vr:2.561 mount:2.561 detect:42931.234", t += 100);
  CHECK(c.stable_at == t);
  /* the headset on and off */
  LINE(&c, "<118>#LOGIN MGR# [PsvrMgr] SetHmdMountStatus 1", t += 100);
  CHECK(c.mount == 1 && c.mount_at == t);
  LINE(&c, "<118>#LOGIN MGR# [PsvrMgr] SetHmdMountStatus 1", t += 100);
  CHECK(c.mount == 1 && c.mount_at == t - 100);
  LINE(&c, "<118>#LOGIN MGR# [PsvrMgr] SetHmdMountStatus 0", t += 100);
  CHECK(c.mount == 0 && c.mount_at == t);
  /* a SLAM reset and "tracking stable" in one poll, in order: stable after the reset counts,
   * a reset after stable clears it */
  LINE(&c, "<118>[SceVrTracker2][Ins] Detect SLAM Reset (cnt 8)", t += 100);
  CHECK(c.stable_after_reset == 0);
  LINE(&c, "<118>[XFD] First time tracking stable. vr:1.2 mount:1.2 detect:3.4", t + 1);
  CHECK(c.stable_after_reset == 1 && c.stable_at == t + 1);
  LINE(&c, "<118>[SceVrTracker2][Ins] Detect SLAM Reset (cnt 9)", t + 2);
  CHECK(c.stable_after_reset == 0);
  /* a new game: the two levels not known, the game's own stream line gone, the markers' counts
   * over; lines that say nothing about them leave them unknown; any VrCamera line or hmd2_image
   * line makes them known */
  c.mark_written_seq = 40, c.mark_seen_seq = 40, c.mark_seen_at = t;
  cam_new_game(&c, 5000, t += 1000);
  CHECK(c.game_pid == 5000 && c.img_pri == CAM_PRI_NONE && c.img_mode == 0 && c.xfd == CAM_XFD_NONE && c.img_known == 0);
  CHECK(c.levels_since == t && c.mark_written_seq == 0 && c.mark_seen_seq == 0 && c.mark_seen_at == 0);
  LINE(&c, "<118>[SceShellCore] FMEM  13.9/  27.7   0.0/   0.0 payload.elf", t += 100);
  LINE(&c, "<118>#LOGIN MGR# [PsvrMgr] SetHmdMountStatus 1", t += 100);
  LINE(&c, "<118>[LoginMgr] [Gen2VrSafetyNotice] Don't need show SafetyNotice.", t += 100);
  LINE(&c, "<118>[SceShellUI] I/PSM.UI : OnFocusActiveSceneChanged [X : Y] -> [AppScreen : ApplicationScreenScene]", t += 100);
  CHECK(c.xfd == CAM_XFD_NONE && c.img_known == 0);
  LINE(&c, "<118>[XFD] VrCamera StateChanged: Stop", t += 100);
  CHECK(c.xfd == CAM_XFD_IDLE && c.img_known == 1 && c.img_mode == 1 && c.img_pri == CAM_PRI_NONE && c.bringup_at == t);
  /* lost lines mid-game past a StartJobRunning line: unknown again, never known by time; a busy
   * line makes the job and the stream known the system's, an idle line nobody's; a lost open line
   * of the system's stream is cleared by the idle line */
  LINE(&c, "<118>[XFD] VrCamera StateChanged: StartJobRunning", t += 100);
  cam_levels_unknown(&c, t += 100);
  CHECK(c.xfd == CAM_XFD_NONE && c.img_known == 0 && c.levels_since == t);
  LINE(&c, "<118>[SceShellCore] FMEM  13.9/  27.7   0.0/   0.0 payload.elf", t += 100);
  CHECK(c.xfd == CAM_XFD_NONE);
  LINE(&c, "<118>[XFD] VrCamera StateChanged: Running", t += 100);
  CHECK(c.xfd == CAM_XFD_BUSY && c.img_known == 1 && c.img_mode == 16 && c.img_pri == 0);
  LINE(&c, "<118>[XFD] VrCamera StateChanged: state Stop", t += 100);
  CHECK(c.xfd == CAM_XFD_IDLE && c.img_known == 1 && c.img_mode == 16 && c.img_pri == 0 && c.bringup_at == t); /* (1) the parsed-as-busy open stays until mode 1 */
  LINE(&c, "hmd2_image: mode 1", t += 100);
  CHECK(c.img_mode == 1 && c.img_pri == CAM_PRI_NONE);
  cam_levels_unknown(&c, t += 100);
  LINE(&c, "hmd2_image: pid 61, pri 0, ts 2, mode 16", t += 100);
  CHECK(c.xfd == CAM_XFD_NONE && c.img_known == 1 && c.img_pri == 0); /* the stream known, the job not */
  LINE(&c, "<118>[XFD] VrCamera StateChanged: NonVR", t += 100);
  CHECK(c.xfd == CAM_XFD_IDLE && c.img_pri == 0); /* (1) known: the idle line leaves it */
  LINE(&c, "hmd2_image: mode 1", t += 100);
  CHECK(c.img_pri == CAM_PRI_NONE);
  /* a stream line read after a loss is not new text */
  t += 100;
  cam_parse_line(&c, "hmd2_image: pid 5000, pri -1, ts 1, mode 4", t, t, 0);
  CHECK(c.img_known == 1 && c.img_new == 0 && c.img_poll_at == t);
  /* an adopted game: the two levels from the buffer's history, stamped now, the floor from now,
   * the stream line not new text; the history's events not taken */
  cam_levels_unknown(&c, t += 100);
  {
    struct cam_state parsed = c;
    uint64_t r1 = c.relocal_at, s1 = c.slam_at;
    cam_parse_line(&parsed, "<118>[XFD] VrCamera StateChanged: StartJobRunning", t, t, 0);
    cam_parse_line(&parsed, "<118>[XFD] VrCamera StateChanged: state Stop", t, t, 0);
    cam_parse_line(&parsed, "<118>[SceVrTracker2][Ins] Detect SLAM Reset (cnt 11)", t, t, 0);
    cam_parse_line(&parsed, "hmd2_image: pid 5000, pri -1, ts 1, mode 4", t, t, 0);
    cam_parse_line(&parsed, "<118>[SceShellUI] I/PSM.UI : OnFocusActiveSceneChanged [AppScreen : X] -> [TrackingLostModalScene : Y]", t, t, 0);
    cam_levels_from_history(&c, &parsed, t += 500);
    CHECK(c.xfd == CAM_XFD_IDLE && c.xfd_at == t && c.img_known == 1 && c.img_mode == 4 && c.img_new == 0 && c.img_at == t);
    CHECK(c.bringup_at == t && c.relocal_at == r1 && c.slam_at == s1 && c.screen_up == 1); /* (3) the flag too */
  }
  printf("%s: %d failures (%u lines)\n", fails ? "FAILED" : "ok", fails, c.lines);
  return fails != 0;
}
