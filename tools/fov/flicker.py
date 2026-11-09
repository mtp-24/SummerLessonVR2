#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A run's flicker lines (agent.probe=3, agent/mirror.c) summed up in stretches:

    tools/fov/flicker.py logs/run.log

Each stretch runs from one "cvars.txt changed" line to the next. Per stretch: how many seconds,
then for each eye the median picture-to-picture change (in luma steps), the same for the bright
samples, and the right eye's figure over the left's. With the head held, a flicker in one eye
shows as that eye's change well above the other's.
"""
import re
import statistics
import sys

F = re.compile(r"flicker: picture-to-picture change L ([\d.]+) \(at most ([\d.]+), bright ([\d.]+) of (\d+)\), "
               r"R ([\d.]+) \(at most ([\d.]+), bright ([\d.]+) of (\d+)\); luma L ([\d.]+) R ([\d.]+)")


def label(name, setting):
    return name + "".join(" %s=%s" % (k.split(".")[-1], v) for k, v in sorted(setting.items()))


def main(path):
    # each stretch named by the settings in force at its first flicker line; agent.taa (logged only
    # when it changes) carried over changes
    stretches, cur, name, setting, first = [], [], "(start)", {}, None
    for line in open(path, errors="replace"):
        if "cvars.txt changed" in line:
            stretches.append((label(name, first if first is not None else setting), cur))
            cur, name, first = [], "after change %d" % len(stretches), None
            # (a game variable taken out of cvars.txt logs nothing: only agent.taa, which logs every
            # change, carried over)
            setting = {k: v for k, v in setting.items() if k == "agent.taa"}
            continue
        # (the new value only: "cvars: agent.taa 0 -> 13", "cvars: r.X -> 3", "... set back to 3 (the game
        # had made it 4)"; not "r.X = 4 (render thread 4)", read out before a change is made)
        m = re.search(r"cvars: (r\.PostProcessAAQuality|agent\.taa)\b.*?(?:-> |\bset back to )(\d+)", line) or \
            re.search(r"cvars: (agent\.taa) = (\d+)\s*$", line)  # (the agent's own at the start)
        if m:
            setting[m.group(1)] = m.group(2)
        m = F.search(line)
        if m:
            if not cur:
                first = dict(setting)
            cur.append([float(x) for x in m.groups()])
    stretches.append((label(name, first if first is not None else setting), cur))
    for name, rows in stretches:
        if not rows:
            continue
        med = lambda k: statistics.median(r[k] for r in rows)
        l, r, lb, rb = med(0), med(4), med(2), med(6)
        print("%-48s %3d s  change L %.2f R %.2f (R/L %.2f)  bright L %.1f R %.1f  luma L %.1f R %.1f"
              % (name, len(rows), l, r, r / l if l else 0, lb, rb, med(8), med(9)))
    print("seconds (L R change, R bright):", " ".join("%.1f/%.1f/%.0f" % (x[0], x[4], x[6]) for _, rows in stretches for x in rows))


if __name__ == "__main__":
    main(sys.argv[1])
