#!/usr/bin/env python3 -I
"""The palm tracker's camera gate (agent/hand.c gate_eval, fed by payload/main.c cam_parse_line)
replayed over the old kernel logs: when it would have cleared against when the tracker actually
opened the cameras (the kernel's "hmd2_image: pid N, pri -1, ts T, mode 4" line of each session,
matched to the agent log's sessions as the root-cause census did). For every session that reached
a start: held at the actual start or not, the reason, and the extra wait the gate would have added
(the first moment at or after the actual start when the gate is clear). The known hangs, without a
start line in their kernel log, are judged by the agent log's clear moment (its process time put
on the kernel log's clock with the launch line and the survivors' calibration) or at the end of
their kernel log. Also the residual window: system camera work that began within 3 s AFTER an
actual start, and whether that session survived.

    tools/hand/gate_check.py <logs dir> [-v] [floor=<s>] [relocal=<s>] [relocalset=all|lead] [stable=<s>] [loss=<s>]
      loss: a loss of lines simulated this long after each session's first bring-up end (the levels
      unknown then, as after a lost anchor): per session, whether and when its lines made them
      known again, and the start's extra wait with it
      (the defaults are agent/hand.c's constants: floor 12, relocal 3, relocalset lead, stable 10)
      floor: the bring-up floor (s); relocal: the relocalising window (s); relocalset: the lines
      that count as relocalising, all of them or only those seen 0-3 s before a tracking-lost
      camera job (Map Latch, INS latched, SLAM Reset, the tracking-lost screen); stable: how long
      "tracking stable" is awaited after a SLAM reset (s)

The parser and the thresholds here mirror the C (keep them the same)."""
import os, re, sys, glob, bisect
from collections import Counter

LOGS = sys.argv[1]
VERBOSE = "-v" in sys.argv

# ---- thresholds (agent/hand.c)
BRINGUP = float(next((a[6:] for a in sys.argv if a.startswith("floor=")), 12.0))
RELOCAL = float(next((a[8:] for a in sys.argv if a.startswith("relocal=")), 3.0))
RELOCALSET = next((a[11:] for a in sys.argv if a.startswith("relocalset=")), "lead")
MOUNT = 5.0
STABLE = float(next((a[7:] for a in sys.argv if a.startswith("stable=")), 10.0))
CHECK = 10.0
LOSS = next((float(a[5:]) for a in sys.argv if a.startswith("loss=")), None)
RESIDUAL = 3.0

SYSTEM_SCENES = ("SafetyNotice", "PlayArea", "TrackingLost", "OutOfPlayArea",
                 "VrControllerRecommend", "CanNotFindPlayArea", "CommonScanning",
                 "ChooseStandingOrSitting", "AdjustHeight", "CameraView", "FirstCamera")
IMG = re.compile(r"^hmd2_image: pid (\d+), pri (-?\d+), ts (\d+), mode (\d+)")


class Cam:
    """the injector's record (common/camera.h), with times on the kernel log's clock: the two
    log-derived levels (the camera job, the image stream), the system's screens as events only
    (the quiet floor; the game's own status holds the live start while one is up, which the kernel
    log does not show)"""
    def __init__(self):
        self.img_mode = 0; self.img_pid = 0; self.img_pri = None; self.img_at = None
        self.xfd = None; self.xfd_at = None
        self.notice_end_at = None; self.screen_at = None; self.mount = None; self.mount_at = None
        self.bringup_at = None; self.stable_at = None; self.slam_at = None; self.relocal_at = None
        self.restart_at = None; self.stable_after_reset = True
        self.relocal_kind = None
        self.known = dict(xfd=True, img=True); self.screen_up = False
        self.loss_due = None; self.loss_at = None; self.reknown_at = None; self.reknown_by = None
        self.events = []  # (t, kind, text) for the residual window and -v

    def unknown(self, t):
        """a new game (or lost lines): only lines establish the levels again"""
        self.xfd = None; self.img_mode = 0; self.img_pri = None
        self.known = dict(xfd=False, img=False); self.screen_up = False
        self.levels_since = t

    def note_known(self, t, l):
        if self.loss_at is not None and self.reknown_at is None and all(self.known.values()):
            self.reknown_at, self.reknown_by = t, l[:70]

    def line(self, l, t):
        if "[SceLncService] launchApp(CUSA" in l:
            self.unknown(t)
            self.loss_due = None; self.loss_at = None; self.reknown_at = None; self.reknown_by = None
        if LOSS is not None and self.loss_due is not None and t is not None and t >= self.loss_due and self.loss_at is None:
            self.unknown(t); self.loss_at = t
        was = all(self.known.values())
        self._line(l, t)
        if LOSS is not None and self.loss_due is None and self.bringup_at is not None and self.loss_at is None and t is not None:
            self.loss_due = self.bringup_at + LOSS
        if not was:
            self.note_known(t, l)

    def _line(self, l, t):
        if l.startswith("<"):
            e = l.find(">")
            if 0 < e <= 4:
                l = l[e + 1:]
        if l.startswith("hmd2_image: "):
            m = IMG.match(l)
            if m:
                self.img_pid, self.img_pri, self.img_mode = int(m.group(1)), int(m.group(2)), int(m.group(4))
                self.img_at = t; self.known["img"] = True
                if self.img_pri != -1:
                    self.bringup_at = t
                    self.events.append((t, "sys-open", l))
            elif l[12:18] == "mode 1" and (len(l) == 18 or l[18] == " "):
                self.img_mode, self.img_pid, self.img_pri, self.img_at = 1, 0, None, t
                self.bringup_at = t; self.known["img"] = True
            return
        p = l.find("[XFD] VrCamera StateChanged: ")
        if p >= 0:
            st = l[p + len("[XFD] VrCamera StateChanged: "):]
            idle = st in ("Stop", "state Stop", "NonVR")
            self.xfd = "idle" if idle else "busy"
            self.xfd_at = t; self.known["xfd"] = True
            self.bringup_at = t
            if not self.known["img"]:
                self.known["img"] = True
                self.img_mode, self.img_pri = (1, None) if idle else (16, 0)
            if st == "StartJobRunning":
                self.events.append((t, "camera-job", l))
            return
        p = l.find("[Gen2VrSafetyNotice] ")
        if p >= 0:
            st = l[p + len("[Gen2VrSafetyNotice] "):]
            self.screen_at = t; self.bringup_at = t
            if st == "Start":
                self.events.append((t, "notice-check", l))
            elif st == "SafetyNoticeStart":
                self.events.append((t, "notice", l))
            elif st == "SafetyNoticeFinished":
                self.notice_end_at = t
            return
        if "[ScePlayAreaEditor] success to set playarea" in l or "[XFD] PlayAreaEdit State Changed " in l:
            self.screen_at = self.bringup_at = t
            if "PlayAreaEdit" in l:
                self.events.append((t, "playarea-edit", l))
            return
        p = l.find("OnFocusActiveSceneChanged ")
        if p >= 0:
            q = l.find("-> [", p)
            frm = l[p:q] if q >= 0 else l[p:]
            to = l[q + 4:] if q >= 0 else ""
            if any(k in frm for k in SYSTEM_SCENES) or any(k in to for k in SYSTEM_SCENES):
                self.screen_at = self.bringup_at = t
                self.screen_up = any(k in to for k in SYSTEM_SCENES)
                if self.screen_up:
                    self.events.append((t, "system-screen", l))
                if "TrackingLost" in to:
                    self.relocal_at = t
            return
        if "[XFD] First time tracking stable" in l:
            self.stable_at = t; self.stable_after_reset = True
        elif "[SceVrTracker2][Ins] Detect SLAM Reset" in l:
            self.slam_at = self.relocal_at = self.restart_at = t
            self.relocal_kind = "SLAM Reset"; self.stable_after_reset = False
        elif "[SceVrTracker2][Ins] Map Latch" in l:
            self.relocal_at, self.relocal_kind = t, "Map Latch"
        elif "[SceVrTracker2][Ins] INS latched Slam delta pose" in l:
            self.relocal_at, self.relocal_kind = t, "INS latched"
        elif RELOCALSET == "all" and "[SceVrTracker2] ResetLocalCoordinate" in l:
            self.relocal_at, self.relocal_kind = t, "ResetLocalCoordinate"
        elif RELOCALSET == "all" and "[SceVrTracker2][Ins] Ins instance is null" in l:
            self.relocal_at, self.relocal_kind = t, "Ins instance is null"
        elif RELOCALSET == "all" and "[SceVrTracker2][Ins] IMU: detected" in l:
            self.relocal_at, self.relocal_kind = t, "IMU samples drop"
        else:
            p = l.find("[PsvrMgr] SetHmdMountStatus ")
            if p >= 0:
                m = int(l[p + len("[PsvrMgr] SetHmdMountStatus ")])
                if m != self.mount:
                    self.mount_at = t
                self.mount = m

    def verdict(self, t):
        """(reason or 'clear', when a time-based reason expires or None)"""
        c = self
        if not all(c.known.values()):
            return "camera state not known yet (%s)" % ",".join(k for k, v in c.known.items() if not v), None
        if c.img_pri is not None and c.img_pri != -1 and c.img_mode != 1:
            return "system has the image stream", None
        if c.xfd == "busy":
            return "camera job running", None
        if c.screen_up:
            return "system VR screen focused", None
        if c.mount == 1 and c.mount_at is not None and t - c.mount_at < MOUNT:
            return "headset just put on", c.mount_at + MOUNT
        if c.relocal_at is not None and t - c.relocal_at < RELOCAL:
            return "relocalising (%s %.1f s ago)" % (c.relocal_kind, t - c.relocal_at), c.relocal_at + RELOCAL
        if c.restart_at is not None and not c.stable_after_reset and t - c.restart_at < STABLE:
            return "not stable since restart", c.restart_at + STABLE
        if c.bringup_at is not None and t - c.bringup_at < BRINGUP:
            return "bring-up < %.0f s" % BRINGUP, c.bringup_at + BRINGUP
        return "clear", None


def load_klog(path):
    """lines with their time (the '--- <epoch>' stamps; None before the first)"""
    out = []
    t = None
    with open(path, "rb") as f:
        for i, raw in enumerate(f, 1):
            line = raw.decode("utf-8", "replace").rstrip("\n")
            if line.startswith("--- "):
                try:
                    t = float(line[4:])
                except ValueError:
                    pass
                continue
            out.append((i, t, line))
    return out


class Replay:
    """the gate over one kernel log file, in order (the injector is resident: state carries
    across the file's sessions)"""
    def __init__(self, lines):
        self.lines = lines
        self.cam = Cam()
        self.pos = 0
        self.times = [t if t is not None else -1.0 for _, t, _ in lines]

    def advance_to_line(self, idx):
        """apply lines [pos, idx)"""
        while self.pos < idx and self.pos < len(self.lines):
            _, t, l = self.lines[self.pos]
            self.cam.line(l, t)
            self.pos += 1

    def advance_to_time(self, t):
        while self.pos < len(self.lines) and (self.times[self.pos] < 0 or self.times[self.pos] <= t):
            _, lt, l = self.lines[self.pos]
            self.cam.line(l, lt)
            self.pos += 1

    def clear_at_or_after(self, t0, limit=600.0):
        """the first time >= t0 at which the gate is clear, or None within limit; and the verdict at t0"""
        self.advance_to_time(t0)
        why0, _ = self.cam.verdict(t0)
        t = t0
        while t - t0 <= limit:
            self.advance_to_time(t)
            why, expires = self.cam.verdict(t)
            if why == "clear":
                return t, why0
            nxt = self.times[self.pos] if self.pos < len(self.lines) else None
            cands = [x for x in (expires, nxt) if x is not None and x > t]
            if not cands:
                return None, why0
            t = min(cands) + 1e-3
        return None, why0


def klog_for(log):
    d = os.path.dirname(log)
    cands = [log + ".klog", log[:-4] + ".klog", log[:-4] + ".klog.txt", log + ".klog.new"]
    if d != LOGS:
        cands += [os.path.join(d, "klog.txt"), os.path.join(d, "console-klog.txt"), os.path.join(d, "klog.stream")]
    for c in cands:
        if os.path.exists(c) and os.path.getsize(c) > 0:
            return c
    return None


def sessions(log):
    """the agent log's sessions (census2.py), with the clear moment's process time"""
    with open(log, "rb") as f:
        lines = [l.decode("utf-8", "replace").rstrip("\n") for l in f]
    starts = [i for i, l in enumerate(lines) if "summerlessonvr2: entry: from" in l] or [0]
    starts.append(len(lines))
    out = []
    for k, (s, e) in enumerate(zip(starts, starts[1:])):
        seg = lines[s:e]
        game = next((re.search(r"CUSA\d+", l).group(0) for l in seg if re.search(r"CUSA\d+", l)), "?")
        hand = [l for l in seg if l.startswith("hand: ")]
        clear = next((re.search(r"\((\d+\.\d+) s\): starting in 5 s", l) for l in hand if "starting in 5 s" in l), None)
        first_status = next((re.search(r"at (\d+\.\d+) s", l) for l in hand if l.startswith("hand: system status at")), None)
        out.append(dict(k=k, game=game, started=any(l.startswith("hand: starting:") for l in hand),
                        tracking=any(l.startswith("hand: tracking started") for l in hand),
                        stop=any(l.startswith("hand: VrHandStop") for l in hand),
                        gone=any("game gone" in l for l in seg),
                        clear_at=float(clear.group(1)) if clear else None,
                        first_status=float(first_status.group(1)) if first_status else None,
                        hands_off=any("no palm tracker" in l for l in seg)))
    return out


def fmt(t):
    return "-" if t is None else "%.1f" % t


rows = []
residual = []
logs = sorted(set(glob.glob(os.path.join(LOGS, "*.log")) + glob.glob(os.path.join(LOGS, "*", "*.log"))))
klogs = {}
for log in logs:
    ss = sessions(log)
    if not any(s["started"] for s in ss):
        continue
    kl = klog_for(log)
    if not kl:
        continue
    if kl not in klogs:
        klogs[kl] = load_klog(kl)
    lines = klogs[kl]
    launches = [i for i, (_, t, l) in enumerate(lines) if "[SceLncService] launchApp(CUSA" in l]
    offs = max(0, len(launches) - len(ss))
    for s in ss:
        if not s["started"]:
            continue
        li = s["k"] + offs
        if li >= len(launches):
            continue
        l0 = launches[li]
        l1 = launches[li + 1] if li + 1 < len(launches) else len(lines)
        t_launch = lines[l0][1]
        opens = [i for i in range(l0, l1) if IMG.match(lines[i][2]) and IMG.match(lines[i][2]).group(2) == "-1"]
        kills = [i for i in range(l0, l1) if "Kill App : " in lines[i][2] and s["game"] in lines[i][2]]
        ended = "stop+gone" if s["stop"] and s["gone"] else "gone" if s["gone"] else "stop" if s["stop"] else "TRUNCATED"
        r = dict(log=os.path.relpath(log, LOGS), k=s["k"], game=s["game"], klog=os.path.relpath(kl, LOGS), ended=ended,
                 t_launch=t_launch, clear_at=s["clear_at"], first_status=s["first_status"], tracking=s["tracking"])
        if opens:
            i_open = opens[0]
            t_open = lines[i_open][1]
            r["t_open"] = t_open
            if t_open is None:
                r["note"] = "untimed klog"
            else:
                rp = Replay(lines)
                rp.advance_to_line(i_open)  # the state as of the open line itself (the open is the game's)
                why0, _ = rp.cam.verdict(t_open)
                t_clear, _ = rp.clear_at_or_after(t_open)
                r["why_at_open"] = why0
                r["t_clear"] = t_clear
                r["loss_at"], r["reknown_at"], r["reknown_by"] = rp.cam.loss_at, rp.cam.reknown_at, rp.cam.reknown_by
                if rp.cam.loss_at is not None and rp.cam.reknown_at is None:
                    rp3 = Replay(lines); rp3.advance_to_line(l1)
                    r["loss_at"] = rp3.cam.loss_at; r["reknown_at"] = rp3.cam.reknown_at; r["reknown_by"] = rp3.cam.reknown_by
                    r["mount_after_loss"] = any(lines[i][1] and lines[i][1] > rp3.cam.loss_at and "SetHmdMountStatus 1" in lines[i][2] for i in range(l0, l1))
                r["extra"] = None if t_clear is None else max(0.0, t_clear - t_open)
                # the residual window: system camera work within RESIDUAL s after the open
                end = min([k for k in kills if k > i_open] + [l1])
                rp2 = Replay(lines)
                rp2.advance_to_line(end)
                after = [(t, kind, txt) for t, kind, txt in rp2.cam.events
                         if t is not None and t_open <= t <= t_open + RESIDUAL and kind in ("camera-job", "sys-open", "playarea-edit", "notice", "system-screen")]
                if after:
                    residual.append((r, after))
                if s["clear_at"] is not None and t_launch is not None:
                    r["calib"] = t_open - t_launch - (s["clear_at"] + 5.0)
        rows.append(r)

timed = [r for r in rows if r.get("t_open") is not None and r.get("extra") is not None]
never = [r for r in rows if r.get("t_open") is not None and "extra" in r and r["extra"] is None]
untimed = [r for r in rows if r.get("note")]
noopen = [r for r in rows if "t_open" not in r]
calib = sorted(r["calib"] for r in rows if r.get("calib") is not None)
cal_med = calib[len(calib) // 2] if calib else 0.0

print("%d sessions with a start and a kernel log window; %d with the game's camera open timed, %d untimed, %d without the open in the window"
      % (len(rows), len(timed) + len(never), len(untimed), len(noopen)))
print("calibration (the open's kernel-log time - the launch line - (clear moment + 5 s)), n=%d: median %.2f s, min %.2f, max %.2f"
      % (len(calib), cal_med, calib[0] if calib else 0, calib[-1] if calib else 0))
print()
print("--- the actual starts: held at the open, reason, and the gate's extra wait")
held = Counter()
extras = []
for r in sorted(timed + never, key=lambda r: (r["log"], r["k"])):
    h = r["why_at_open"] != "clear"
    held[(r["ended"], h)] += 1
    if r.get("extra") is not None:
        extras.append(r["extra"])
    if VERBOSE or h or r.get("extra") is None or r["extra"] > 0:
        print("  %-40s k%d %s %-9s %-5s at open: %-32s extra %s%s" % (
            r["log"][:40], r["k"], r["game"], r["ended"], "HELD" if h else "clear", r["why_at_open"],
            "never (within 600 s)" if r.get("extra") is None else "%.1f s" % r["extra"],
            " (DIED)" if r["ended"] in ("gone", "TRUNCATED") and not r["tracking"] else ""))
print("held at the open, by how the session ended:", dict(held))
if LOSS is not None:
    rk = [r for r in timed + never if r.get("loss_at") is not None]
    back = [r for r in rk if r.get("reknown_at") is not None]
    print("simulated loss %.0f s after the bring-up end: %d sessions lost their levels; known again in %d (by: %s); never again in %d"
          % (LOSS, len(rk), len(back), dict(Counter(re.sub(r"<118>|\[SceShellUI\] I/PSM.UI : ", "", r["reknown_by"])[:40] for r in back)), len(rk) - len(back)))
    d = sorted(r["reknown_at"] - r["loss_at"] for r in back)
    if d:
        print("  known again after: median %.1f s, p90 %.1f s, max %.1f s" % (d[len(d) // 2], d[int(0.9 * len(d))], d[-1]))
    mounts = [r for r in rk if r.get("reknown_at") is None and r.get("mount_after_loss")]
    print("  of those never known again, %d had the headset put on after the loss" % len(mounts))
print("the reason at the open:", dict(Counter(re.sub(r" [\d.]+ s ago", "", r["why_at_open"]) for r in timed + never)))
ages = [float(m.group(2)) for r in timed + never for m in [re.search(r"\((.*) ([\d.]+) s ago\)", r["why_at_open"])] if m]
if ages:
    ages.sort()
    print("the relocalising line's age at the open: min %.1f, median %.1f, max %.1f s" % (ages[0], ages[len(ages) // 2], ages[-1]))
extras.sort()
if extras:
    p = lambda q: extras[min(len(extras) - 1, int(q * len(extras)))]
    print("extra wait over %d survivors: zero for %d; median %.1f s, p90 %.1f s, p95 %.1f s, max %.1f s; > 0: n=%d, median %.1f s"
          % (len(extras), sum(1 for e in extras if e == 0), p(0.5), p(0.9), p(0.95), extras[-1],
             sum(1 for e in extras if e > 0), (sorted(e for e in extras if e > 0) or [0])[len([e for e in extras if e > 0]) // 2]))
print("never clear within 600 s:", [(r["log"], r["k"], r["why_at_open"]) for r in never])
print()

print("--- the residual window: system camera work within %.0f s AFTER an actual start" % RESIDUAL)
if not residual:
    print("  none")
for r, after in residual:
    print("  %-40s k%d %-9s open at %.1f: %s" % (r["log"][:40], r["k"], r["ended"], r["t_open"],
          "; ".join("+%.1f s %s" % (t - r["t_open"], kind) for t, kind, _ in after)))
print()

# ---- the hangs: no start line in the kernel log
print("--- the hangs (the agent's clear moment put on the kernel log's clock with the launch line + calibration %.2f s)" % cal_med)
HANGS = [
    ("allison2.log", "allison2.log.klog", "Allison 2026-11-04 run 2: the old 2-s rule started inside the safety notice"),
    ("allison1.agent.log", "allison1.klog.txt", "Allison run 1 (an older agent: no hand lines)"),
    ("crash-20261108-c-stock/c-stock.log.partial", "crash-20261108-c-stock/c-stock.log.klog", "Chisato from home, worn, 'Don't need show'"),
    ("frame-copy-2026-11-04/stepB-60-copy1.log.partial", "frame-copy-2026-11-04/stepB-60-copy1.log.klog", "Hikari replay, headset off, the frame-copy run"),
    ("switch-repro/console-agent.log", "switch-repro/klog.txt", "Hikari -> Chisato switch (no stamps in its kernel log)"),
]
for alog, aklog, what in HANGS:
    lp, kp = os.path.join(LOGS, alog), os.path.join(LOGS, aklog)
    if not os.path.exists(lp) or not os.path.exists(kp):
        print("  %s: files missing" % alog)
        continue
    ss = sessions(lp)
    s = ss[-1]
    lines = load_klog(kp)
    launches = [i for i, (_, t, l) in enumerate(lines) if "[SceLncService] launchApp(CUSA" in l]
    print("  %s (%s): %s" % (alog, s["game"], what))
    if not launches:
        print("    no launch line in the kernel log")
        continue
    l0 = launches[-1] if len(launches) < len(ss) else launches[len(ss) - 1 + (len(launches) - len(ss))]
    t_launch = lines[l0][1]
    t_end = next((t for _, t, _ in reversed(lines) if t is not None), None)
    rp = Replay(lines)
    if t_launch is None or t_end is None:
        # untimed: the agent log alone. The SLAM reset / bring-up came right before "game frames
        # -> headset", which is 2 s before the first "hand: system status" line (the 240-refresh wait)
        rp.advance_to_line(len(lines))
        why, _ = rp.cam.verdict(0)
        if s["clear_at"] is not None and s["first_status"] is not None:
            gap = s["clear_at"] + 5 - (s["first_status"] - 2.0)
            print("    untimed: start due at %.1f s of game time; the bring-up about %.1f s (the lines before 'game frames -> headset'): %.1f s after it -> %s"
                  % (s["clear_at"] + 5, s["first_status"] - 2.0, gap, "HELD by the %.0f s floor" % BRINGUP if gap < BRINGUP else "NOT HELD"))
        print("    the state at the end of the kernel log (ages unknown): %s" % why)
        continue
    rp.advance_to_time(t_end + 1)
    why_end, _ = rp.cam.verdict(t_end)
    print("    launch at %.1f, the kernel log ends at %.1f (+%.1f s): the gate then: %s" % (t_launch, t_end, t_end - t_launch, why_end))
    if s["clear_at"] is not None:
        t_start = t_launch + s["clear_at"] + 5.0 + cal_med
        rp = Replay(lines)
        rp.advance_to_time(min(t_start, t_end + 1))
        why, _ = rp.cam.verdict(t_start)
        print("    clear moment %.3f s -> the start due at %.1f (%+.1f s from the log's end): the gate then: %s%s" % (
            s["clear_at"], t_start, t_start - t_end, why, "" if why != "clear" else " (NOT HELD)"))
        c = rp.cam
        if c.bringup_at is not None:
            print("    the last bring-up end %.1f s before the start" % (t_start - c.bringup_at))
    else:
        print("    no clear moment in the agent log")
