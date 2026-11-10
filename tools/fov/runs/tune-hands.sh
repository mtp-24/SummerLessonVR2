#!/bin/sh
# tools/fov/runs/tune-hands.sh: the sweep with the palm tracker ON (the user, 2026-11-09: the real load), on
# the replays recorded for it (tests/rec10-allison, rec1-102 for Hikari 1.02, rec9-chisato). Inset mode,
# the agent's defaults (tune-hands.txt adds no agent.hands line: the console's own 60 stays), at 60, 90
# and 120 (framerate.txt none / 90 / 120, each rate's own inset rows). Per game and rate the eyes go up a
# step only while the last step held: its play windows (the scene loads, a window over 25 ms at the
# most, left out) with at most 0.5 % of frames held too long (at 60 three refreshes or more, at 90 and
# 120 two or more). A run counts only when the replay played, the eyes were the size asked for and the
# palm tracker started ("hand: tracking started"). resolution.txt and framerate.txt are put back as
# they were (tools/consfile.sh). Stops at the first run that fails a check.
#   tools/fov/runs/tune-hands.sh [game ...]      (allison hikari chisato: all three by default)
W=$(cd "$(dirname "$0")/../../.." && pwd)
LOGS=$(mkdir -p "${PS5_LOGS:-$W/logs}/tune-hands" && cd "${PS5_LOGS:-$W/logs}/tune-hands" && pwd) || exit 1
T=$(cd "${PS5_TESTS:-$W/tests}" && pwd) || exit 1
[ -z "${PS5_HOST:-}" ] && [ -f "$W/tools/local.env" ] && . "$W/tools/local.env"
HOST=${PS5_HOST:?set PS5_HOST to the console address}
FTP=ftp://$HOST:2121/data/summerlessonvr2
cd "$W" || exit 1
. "$W/tools/consfile.sh"
had_resolution= had_framerate=
for f in resolution framerate; do
  if consfile_left "$LOGS/$f.txt.orig"; then
    echo "an earlier sweep's $LOGS/$f.txt.orig or .orig.none is still there: put the console's $f.txt right, then remove it"; exit 1
  fi
done
for f in resolution framerate; do
  consfile_get "$FTP" $f.txt "$LOGS/$f.txt.orig"
  case $? in
    0) eval had_$f=1; echo "$f.txt was $(cat "$LOGS/$f.txt.orig")" ;;
    1) echo "$f.txt: none" ;;
    *) echo "not starting"; rm -f "$LOGS/resolution.txt.orig" "$LOGS/framerate.txt.orig"; exit 1 ;;
  esac
done
put_back() { consfile_put_back "$FTP" $1.txt "$LOGS/$1.txt.orig" "$2" && rm -f "$LOGS/$1.txt.orig"; }
trap 'st=$?; trap "" INT TERM; put_back resolution "$had_resolution" || st=1; put_back framerate "$had_framerate" || st=1; exit $st' EXIT
trap 'exit 130' INT TERM
# ok held total never gpu-range gpu-max: held = frames on the headset `minref` refreshes or more
judge() {
  python3 - "$1" "$2" << 'EOF'
import re, sys
minref = int(sys.argv[2])
H = re.compile(r"hmd: \d+ frames handed over, \d+ shown, \d+ shown again, (\d+) never shown; the game's GPU (\d+\.\d) ms a frame on average, (\d+\.\d) at the most")
O = re.compile(r"hmd: frames on the headset for 1 refresh (\d+), 2 (\d+), 3 (\d+), 4 or more (\d+)")
tot = held = never = 0; avgs = []; mx = 0.0; last = None
for line in open(sys.argv[1], errors="replace"):
    m = O.search(line)
    if m: last = [int(x) for x in m.groups()]; continue
    m = H.search(line)
    if m:
        if float(m.group(3)) <= 25.0 and last:
            tot += sum(last); held += sum(last[minref - 1:]); never += int(m.group(1))
            avgs.append(float(m.group(2))); mx = max(mx, float(m.group(3)))
        last = None
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
  if [ "$6" = 60 ]; then
    curl -s -m 10 "ftp://$HOST:2121/" -Q "DELE /data/summerlessonvr2/framerate.txt" > /dev/null 2>&1
  else
    printf '%s' "$6" | curl -s -m 10 -T - "$FTP/framerate.txt" || exit 1
  fi
  printf '%s' "$7" | curl -s -m 10 -T - "$FTP/resolution.txt" || exit 1
  PS5_HOST=$HOST PS5_REUSE=1 PS5_OTHER_PAYLOADS=${PS5_OTHER_PAYLOADS:-0} PS5_TITLE=$1 PS5_CVARS=tools/fov/runs/tune-hands.txt \
    sh tools/testrun.sh payload/summerlessonvr2.elf $2 "$LOGS/$name.log" "replay worn" "$4" "$5" > "$LOGS/$name.runner.txt" 2>&1
  st=$?
  if [ $st -ne 0 ] || ! grep -q "game gone" "$LOGS/$name.log" 2>/dev/null; then echo "$name: the runner gave $st or no 'game gone': stopping"; exit 1; fi
  if ! grep -q "hand: tracking started" "$LOGS/$name.log" || grep -q "vr2: no palm tracker" "$LOGS/$name.log"; then
    echo "$name: the palm tracker did not start: stopping"; exit 1
  fi
  if ! grep -q "test: replaying" "$LOGS/$name.log" || ! grep -qE "^gpu: r.ScreenPercentage .* eyes $7x" "$LOGS/$name.log"; then
    echo "$name: not the replay or not the eye size asked for ($7): stopping"; exit 1
  fi
  if [ "$6" != 60 ] && ! grep -q "multiview: the game's native $6 Hz sequence" "$LOGS/$name.log"; then
    echo "$name: the native $6 not confirmed in the log: stopping"; exit 1
  fi
  minref=$([ "$6" = 60 ] && echo 3 || echo 2)
  set -- $(judge "$LOGS/$name.log" $minref)
  echo "$name: held $minref+ refreshes $2 of $3 play frames, never shown $4, GPU $5 ms, at the most $6"
  [ "$1" = 1 ]
}
sweep() { # title secs short pad head
  for eye in 1440 1520 1600; do run "$1" "$2" "$3-60-$eye" "$4" "$5" 60 $eye || break; done
  for eye in 1200 1320 1440; do run "$1" "$2" "$3-90-$eye" "$4" "$5" 90 $eye || break; done
  for eye in 960 1080 1200; do run "$1" "$2" "$3-120-$eye" "$4" "$5" 120 $eye || break; done
}
for g in ${*:-allison hikari chisato}; do
  case $g in
    allison) sweep CUSA11674 300 allison "$T"/rec10-allison.pad.txt "$T"/rec10-allison.head.txt ;;
    hikari) sweep CUSA07434 250 hikari102 "$T"/rec1-102.pad.txt "$T"/rec1-102.head.txt ;;
    chisato) sweep CUSA11682 305 chisato "$T"/rec9-chisato.pad.txt "$T"/rec9-chisato.head.txt ;;
    *) echo "$g: allison, hikari or chisato"; exit 1 ;;
  esac
done
