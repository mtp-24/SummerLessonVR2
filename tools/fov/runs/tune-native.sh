#!/bin/sh
# tools/fov/runs/tune-native.sh: the native 90 and 120 sweep (docs/SUMMER-LESSON.md), tune-sweep.sh's
# runs at the native rates. Inset mode, the rate's own rows (540 at 90, 480 at 120), the agent's
# default TAA (13 on the newer engine), the palm tracker off (tune-native.txt). Per game and rate the
# eyes go up a step only while the last step held: its play windows (the scene loads, a window over
# 25 ms at the most, left out) with at most 0.5 % of frames on the headset two refreshes or more.
# framerate.txt and resolution.txt are put back as they were (or removed when there were none).
#   tools/fov/runs/tune-native.sh [game ...]      (chisato allison hikari; all three by default)
W=$(cd "$(dirname "$0")/../../.." && pwd)
LOGS=$(mkdir -p "${PS5_LOGS:-$W/logs}/tune" && cd "${PS5_LOGS:-$W/logs}/tune" && pwd) || exit 1
T=$(cd "${PS5_TESTS:-$W/tests}" && pwd) || exit 1
[ -z "${PS5_HOST:-}" ] && [ -f "$W/tools/local.env" ] && . "$W/tools/local.env"
HOST=${PS5_HOST:?set PS5_HOST to the console address}
FTP=ftp://$HOST:2121/data/summerlessonvr2
cd "$W" || exit 1
. "$W/tools/consfile.sh"
had_resolution= had_framerate=
for f in resolution framerate; do # (every file's backup or marker looked for before any is read)
  if consfile_left "$LOGS/$f.txt.orig"; then
    echo "an earlier sweep's $LOGS/$f.txt.orig or .orig.none is still there: put the console's $f.txt right (or delete it), then remove it"; exit 1
  fi
done
for f in resolution framerate; do
  consfile_get "$FTP" $f.txt "$LOGS/$f.txt.orig"
  case $? in
    0) eval had_$f=1; echo "$f.txt was $(cat "$LOGS/$f.txt.orig")" ;;
    1) echo "$f.txt: none" ;;
    *) echo "not starting"; rm -f "$LOGS/resolution.txt.orig" "$LOGS/framerate.txt.orig"; exit 1 ;; # (only this run's)
  esac
done
put_back() { # file had: 0 when put back (its backup removed) or removed
  consfile_put_back "$FTP" $1.txt "$LOGS/$1.txt.orig" "$2" && rm -f "$LOGS/$1.txt.orig"
}
# (the sweep's status 1 when either was not put right)
trap 'st=$?; trap "" INT TERM; put_back resolution "$had_resolution" || st=1; put_back framerate "$had_framerate" || st=1; exit $st' EXIT
trap 'exit 130' INT TERM
# a run's play windows: frames on the headset 1 / 2+ refreshes, never shown, the GPU average and
# the most (python: "ok held total never gpuavg gpumax" with ok 1 when held <= 0.5 %)
judge() {
  python3 - "$1" << 'EOF'
import re, sys
H = re.compile(r"hmd: \d+ frames handed over, \d+ shown, \d+ shown again, (\d+) never shown; the game's GPU (\d+\.\d) ms a frame on average, (\d+\.\d) at the most")
O = re.compile(r"hmd: frames on the headset for 1 refresh (\d+), 2 (\d+), 3 (\d+), 4 or more (\d+)")
one = held = never = 0; avgs = []; mx = 0.0; last = None
for line in open(sys.argv[1], errors="replace"):
    m = O.search(line)
    if m: last = [int(x) for x in m.groups()]; continue
    m = H.search(line)
    if m:
        if float(m.group(3)) <= 25.0 and last:
            one += last[0]; held += sum(last[1:]); never += int(m.group(1))
            avgs.append(float(m.group(2))); mx = max(mx, float(m.group(3)))
        last = None
tot = one + held
ok = 1 if tot and held <= 0.005 * tot else 0
print(ok, held, tot, never, "%.1f-%.1f" % (min(avgs), max(avgs)) if avgs else "-", mx)
EOF
}
run() { # title secs name pad head fps eye: 0 when it held
  name=$3
  rm -f "$LOGS/$name.log"
  i=0
  while curl -s -m 10 "ftp://$HOST:2121/mnt/sandbox/pfsmnt/" | grep -qE "CUSA07434|CUSA11682|CUSA11674"; do
    i=$((i + 1)); [ $i -gt 30 ] && { echo "$name: a game still mounted after 60 s: stopping"; exit 1; }; sleep 2
  done
  printf '%s' "$6" | curl -s -m 10 -T - "$FTP/framerate.txt" || exit 1
  printf '%s' "$7" | curl -s -m 10 -T - "$FTP/resolution.txt" || exit 1
  PS5_HOST=$HOST PS5_REUSE=1 PS5_OTHER_PAYLOADS=${PS5_OTHER_PAYLOADS:-0} PS5_TITLE=$1 PS5_CVARS=tools/fov/runs/tune-native.txt \
    sh tools/testrun.sh payload/summerlessonvr2.elf $2 "$LOGS/$name.log" "replay worn" "$4" "$5" > "$LOGS/$name.runner.txt" 2>&1
  st=$?
  if [ $st -ne 0 ] || ! grep -q "game gone" "$LOGS/$name.log" 2>/dev/null; then echo "$name: the runner gave $st or no 'game gone': stopping"; exit 1; fi
  if grep -q "hand: starting" "$LOGS/$name.log" || ! grep -q "vr2: no palm tracker (agent.hands=0)" "$LOGS/$name.log"; then
    echo "$name: not a run with the palm tracker off: stopping"; exit 1
  fi
  if ! grep -q "test: replaying" "$LOGS/$name.log" || ! grep -qE "^gpu: r.ScreenPercentage .* eyes $7x" "$LOGS/$name.log"; then
    echo "$name: not the replay or not the eye size asked for ($7): stopping"; exit 1
  fi
  if ! grep -q "multiview: the game's native $6 Hz sequence" "$LOGS/$name.log"; then
    echo "$name: the native $6 not confirmed in the log: stopping"; exit 1
  fi
  set -- $(judge "$LOGS/$name.log")
  echo "$name: held 2+ refreshes $2 of $3 play frames, never shown $4, GPU $5 ms, at the most $6"
  [ "$1" = 1 ]
}
sweep() { # title secs short pad head
  for eye in 1200 1320 1440; do run "$1" "$2" "$3-90-$eye" "$4" "$5" 90 $eye || break; done
  for eye in 960 1080 1200; do run "$1" "$2" "$3-120-$eye" "$4" "$5" 120 $eye || break; done
}
for g in ${*:-chisato allison hikari}; do
  case $g in
    chisato) sweep CUSA11682 255 chisato "$T"/rec7-chisato.pad.txt "$T"/rec7-chisato-held.head.txt ;;
    allison) sweep CUSA11674 300 allison "$T"/rec8-allison.pad.txt "$T"/rec8-allison.head.txt ;;
    hikari) sweep CUSA07434 260 hikari102room "$T"/rec1-102.pad.txt "$T"/rec1-102.head.txt ;;
    *) echo "$g: chisato, allison or hikari"; exit 1 ;;
  esac
done
