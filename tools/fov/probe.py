#!/usr/bin/env python3
"""The inset's brightness against the periphery's at the same directions (agent.probe=1 lines),
summed up per state of the settings that were changing (r.BloomQuality, r.AmbientOcclusionLevels,
r.DepthOfFieldQuality, as cvars: lines set them): the median inset/eye ratio per channel and eye,
over samples where the eye is bright enough to measure (luma >= 20).
    tools/fov/probe.py logs/<run>.log"""
import re, sys, statistics
WATCH = ('r.BloomQuality', 'r.AmbientOcclusionLevels', 'r.DepthOfFieldQuality')
state = {}
groups = {}
order = []
pr = re.compile(r'probe: inset/eye L r ([\d.]+) g ([\d.]+) b ([\d.]+) luma ([\d.]+) \(eye ([\d.]+)\), '
                r'R r ([\d.]+) g ([\d.]+) b ([\d.]+) luma ([\d.]+) \(eye ([\d.]+)\)')
cv = re.compile(r'cvars: (r\.[\w.]+) (?:->|=) (-?\d+)')
for line in open(sys.argv[1], errors='replace'):
    m = cv.search(line)
    if m and m.group(1) in WATCH:
        state[m.group(1)] = m.group(2)
        continue
    m = pr.search(line)
    if not m:
        continue
    v = [float(x) for x in m.groups()]
    key = ' '.join('%s=%s' % (w.split('.')[-1], state.get(w, '?')) for w in WATCH)
    if key not in groups:
        groups[key] = []
        order.append(key)
    for e, off in ((0, 0), (1, 5)):
        if v[off + 4] >= 20:
            groups[key].append((e, v[off:off + 4]))
print('%-52s %5s  %-29s  %-29s' % ('settings', 'n', 'left  r/g/b/luma', 'right r/g/b/luma'))
for key in order:
    out = []
    for e in (0, 1):
        s = [x for ee, x in groups[key] if ee == e]
        if not s:
            out.append('-')
            continue
        med = [statistics.median(c[i] for c in s) for i in range(4)]
        out.append('%.3f %.3f %.3f %.3f' % tuple(med))
    print('%-52s %5d  %-29s  %-29s' % (key, len(groups[key]) // 2, out[0], out[1]))

# The headset's own picture with and without the insets' layer (agent.insetblink, probeh lines;
# older logs label the on-stretches with the agent.insetfx variant in force, since removed): per
# eye and label, the mean over on-stretches of the on brightness over the
# off-stretches either side (the same scenes, so scene changes cancel), samples within 400 ms of
# a switch left out (the frames in flight). 1.000: the inset adds no step.
ph = re.compile(r'probeh: (?:fx (\d+) )?layer (on|off) \((\d+) ms since\) L r ([\d.]+) g ([\d.]+) b ([\d.]+) luma ([\d.]+), '
                r'R r ([\d.]+) g ([\d.]+) b ([\d.]+) luma ([\d.]+)')
runs = []  # [state, fx, [samples]]
for line in open(sys.argv[1], errors='replace'):
    m = ph.search(line)
    if not m:
        continue
    fx, st, since = m.group(1) or '-', m.group(2), int(m.group(3))
    v = [float(x) for x in m.groups()[3:]]
    if not runs or runs[-1][0] != st:
        runs.append([st, fx, []])
    if st == 'on':
        runs[-1][1] = fx  # (the label the on-stretch was drawn with)
    if since >= 400:
        runs[-1][2].append(v)
if runs:
    print()
    print("the headset picture at the eyes' middles, inset layer on over off (neighbouring stretches):")
    print("  (logs from before agent.insetfxtest was removed: fx = its variant; '-' none)")
    labels = sorted(set(r[1] for r in runs if r[0] == 'on'))
    for fx in labels:
        for e, name in ((0, 'left'), (1, 'right')):
            res = {c: [] for c in range(4)}
            for i, r in enumerate(runs):
                if r[0] != 'on' or r[1] != fx or not r[2]:
                    continue
                nb = [runs[j] for j in (i - 1, i + 1) if 0 <= j < len(runs) and runs[j][0] == 'off' and runs[j][2]]
                if not nb:
                    continue
                for c in range(4):
                    on = statistics.mean(x[4 * e + c] for x in r[2])
                    off = statistics.mean(statistics.mean(x[4 * e + c] for x in n[2]) for n in nb)
                    if min(on, off) >= 10:
                        res[c].append(on / off)
            if res[3]:
                print('  fx %s %-5s luma %.3f (%d stretches), r %.3f g %.3f b %.3f' %
                      (fx, name, statistics.mean(res[3]), len(res[3]),
                       *(statistics.mean(res[c]) if res[c] else 0 for c in range(3))))
