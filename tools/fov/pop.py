#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The pop-in of what comes back into an inset (docs/MULTIVIEW.md 30): 1:1 inset captures taken a
set number of pictures after agent.insetgaze=11's look comes back (agent.capafter), against the
settled one (a capture long after the look back, the window as still as before it left):
    pop.py <settled.png> <capture.png>... [--box x0,y0,x1,y1]
    pop.py --steps <settled.png> <first.png> <second.png>... [--box ...]   (frame-to-frame steps)
    --gain: each capture's mean luma in the box scaled to the settled one's first (an exposure drift left out)
For each capture, aligned to the settled one as ghost.py does: the RMS luma difference in the box
(how far from settled it still is) and its fine detail against the settled one's (the mean
difference of neighbouring pixels: a raw frame's aliasing raises it, a blur lowers it; 1.00 is
settled)."""
import sys
from ghost import png, rms, best_shift


def detail(a, w, h, box):
    x0, y0, x1, y1 = box
    s = n = 0
    for y in range(max(y0, 0), min(y1, h - 1)):
        for x in range(max(x0, 0), min(x1, w - 1)):
            s += abs(a[y][x] - a[y][x + 1]) + abs(a[y][x] - a[y + 1][x]); n += 2
    return s / max(n, 1)


def steps(argv, box):
    """--steps <settled> <c1> <c2> ...: each crop's RMS from the settled one and from the crop
    before it (the frame-to-frame steps: the biggest is the pop the eye sees), all aligned to the
    settled one"""
    ref, w, h = png(argv[0])
    box = box or (0, 0, w, h)
    prev = None
    worst = 0.0
    for p in argv[1:]:
        b, _, _ = png(p)
        dx, dy = best_shift(ref, b, w, h)
        if GAIN:
            b = gained(ref, b, w, h, dx, dy, box)
        r = rms(ref, b, w, h, dx, dy, box)
        st = rms(prev[0], b, w, h, dx - prev[1], dy - prev[2], box) if prev else float("nan")
        worst = st if prev and st > worst else worst
        print("%s: from settled %.2f, step from the one before %.2f" % (p.split("/")[-1], r, st))
        prev = (b, dx, dy)
    print("biggest step %.2f" % worst)


GAIN = False


def gained(ref, b, w, h, dx, dy, box):
    """b scaled so its mean luma in the box (shifted onto ref's) is ref's: a scene whose exposure
    drifts over the burst (Chisato's cafe after a look away: ~2% over 2 s) compared for its
    detail, not its brightness"""
    x0, y0, x1, y1 = box
    sa = sb = 0.0; n = 0
    for y in range(max(y0, -dy, 0), min(y1, h - dy, h)):
        for x in range(max(x0, -dx, 0), min(x1, w - dx, w)):
            sa += ref[y][x]; sb += b[y + dy][x + dx]; n += 1
    g = sa / sb if sb > 0 else 1.0
    return [[v * g for v in row] for row in b]


def main(argv):
    global GAIN
    box = None
    if "--gain" in argv:
        GAIN = True; argv.remove("--gain")
    if "--box" in argv:
        i = argv.index("--box"); box = tuple(int(v) for v in argv[i + 1].split(",")); argv = argv[:i] + argv[i + 2:]
    if argv[0] == "--steps":
        return steps(argv[1:], box)
    ref, w, h = png(argv[0])
    box = box or (0, 0, w, h)
    d0 = detail(ref, w, h, box)
    for p in argv[1:]:
        b, w2, h2 = png(p)
        dx, dy = best_shift(ref, b, w, h)
        if GAIN:
            b = gained(ref, b, w, h, dx, dy, box)
        # (the capture's detail in the same place of the picture: the box moved by the shift)
        bb = (box[0] + dx, box[1] + dy, box[2] + dx, box[3] + dy)
        print("%s: shift %d,%d, RMS %.2f in the box, detail %.2f of the settled one's" % (
            p, dx, dy, rms(ref, b, w, h, dx, dy, box), detail(b, w2, h2, bb) / d0))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1:])
