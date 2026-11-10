#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A run's agent.log cut into stretches at each setting switch (tools/testrun.sh PS5_CVARS_AT).

    tools/fov/report.py logs/run.log [--on N --off M]
    tools/fov/report.py logs/plain.log logs/other.log      (two runs: their first stretches)

The log is cut into stretches at each "cvars.txt changed" line, each named by the settings it
applied. For each stretch: the health lines (windows, frames shown twice, the game's GPU time:
average over the windows and the most), leaving out the scene loads (a window over 25 ms at the
most) as the docs do, and the gaze summary when there is one. With two stretches named
(--on/--off, numbers from the list; by default the first stretch with r.ShadowQuality=0 against
the one before it): the GPU time the change made. (The foveation census and cut it also read once
were removed with them, docs/FOVEATION.md.)
"""
import re
import sys

HEALTH = re.compile(r"hmd: (\d+) frames handed over, (\d+) shown, (\d+) shown again, (\d+) never "
                    r"shown; the game's GPU (\d+\.\d) ms a frame on average, (\d+\.\d) at the most")
ONHMD = re.compile(r"hmd: frames on the headset for 1 refresh (\d+), 2 (\d+), 3 (\d+), 4 or more (\d+)")
SETTING = re.compile(r"(?:cvars: (\S+) -> (\S+))|(?:fov: agent\.(\w+) = (\S+))")
LOADING_MS = 25.0


class Stretch:
    def __init__(self, n):
        self.n, self.settings, self.health = n, {}, []
        self.lines = {"gaze": []}
        self.onhmd = []  # per health line: the headset's refreshes a frame (1, 2, 3, 4+), or None

    def name(self):
        return ", ".join("%s=%s" % kv for kv in self.settings.items()) or "(as at the start)"


def read(path):
    stretches = [Stretch(0)]
    s = stretches[0]
    pending = None
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        if "cvars: cvars.txt changed" in line:
            s = Stretch(len(stretches))
            # the game's variables stay as set; the agent's own options are logged again when the
            # new text has them (a line taken out is the option's default)
            s.settings = {k: v for k, v in stretches[-1].settings.items() if not k.startswith("agent.")}
            stretches.append(s)
            continue
        m = SETTING.search(line)
        if m:
            k, v = (m.group(1), m.group(2)) if m.group(1) else ("agent." + m.group(3), m.group(4))
            s.settings[k] = v
        m = ONHMD.search(line)
        if m:
            pending = tuple(int(x) for x in m.groups())
        m = HEALTH.search(line)
        if m:
            s.health.append(tuple(float(x) for x in m.groups()))
            s.onhmd.append(pending)
            pending = None
        if "fov: gaze 10 s" in line:
            s.lines["gaze"].append(line[line.index("fov:"):])
    return stretches


def gpu(s):
    play = [h for h in s.health if h[5] <= LOADING_MS and h[0] > 0]
    if not play:
        return None
    return (len(play), len(s.health), sum(h[4] for h in play) / len(play), max(h[5] for h in play),
            sum(h[2] for h in play), sum(h[0] for h in play))


def show(s):
    print("== stretch %d: %s" % (s.n, s.name()))
    g = gpu(s)
    if g:
        print("   GPU %.2f ms a frame on average, %.1f at the most, over %d play windows (%d in all); "
              "%d of %d frames shown twice" % (g[2], g[3], g[0], g[1], g[4], g[5]))
    # the headset's refreshes a frame (the window's line before each health line): at 60 fps each
    # frame is meant for 2, at the native 90/120 for 1; never shown = a frame dropped
    for label, keep in (("play", lambda h: h[5] <= LOADING_MS and h[0] > 0), ("all", lambda h: True)):
        idx = [i for i, h in enumerate(s.health) if keep(h)]
        hist = [0, 0, 0, 0]
        for i in idx:
            if s.onhmd[i]:
                hist = [a + b for a, b in zip(hist, s.onhmd[i])]
        never = sum(s.health[i][3] for i in idx)
        native = hist[0] > hist[1]  # most frames one refresh each: a native 90/120 run
        held = 1 if native else 2  # a frame held longer than that is a repeat
        bad = sum(1 for i in idx if s.health[i][3] or (s.onhmd[i] and any(s.onhmd[i][held:])))
        print("   %s windows %d: on the headset for 1 refresh %d, 2 %d, 3 %d, 4+ %d; never shown %d; "
              "windows with a frame never shown or held over %d: %d" %
              (label, len(idx), hist[0], hist[1], hist[2], hist[3], never, held, bad))
    for line in s.lines["gaze"][-2:]:
        print("   " + line[:200])


def compare(on, off):
    print("== %d (%s) against %d (%s)" % (on.n, on.name(), off.n, off.name()))
    a, b = gpu(on), gpu(off)
    if a and b:
        print("   GPU on average %.2f -> %.2f ms (%+.2f), at the most %.1f -> %.1f" %
              (a[2], b[2], b[2] - a[2], a[3], b[3]))
    else:
        print("   (no play windows in one of them)")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    if len(argv) > 2 and not argv[2].startswith("--"):
        a, b = read(argv[1])[0], read(argv[2])[0]
        a.n, b.n = 1, 2
        for s in (a, b):
            show(s)
        compare(a, b)
        return 0
    st = read(argv[1])
    for s in st:
        show(s)
    on = off = None
    if "--on" in argv and "--off" in argv:
        on, off = st[int(argv[argv.index("--on") + 1])], st[int(argv[argv.index("--off") + 1])]
    else:
        for s in st[1:]:
            if s.settings.get("r.ShadowQuality") == "0" and s.n > 0:
                on, off = st[s.n - 1], s
                break
    if on and off:
        compare(on, off)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
