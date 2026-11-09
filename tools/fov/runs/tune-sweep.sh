#!/bin/sh
# tools/fov/runs/tune-sweep.sh: the 2026-11-09 sweep (docs/SUMMER-LESSON.md) as one script; it was run in pieces
# (Chisato's 1200 with its switches last). The replays are the user's (tests/, not in the repository).
# The engine's own TAA (agent.taa=0 in the files), as measured.
# the tuning sweep: inset mode, agent.hands=0 in every file; resolution.txt put back at the end
W=$(cd "$(dirname "$0")/../../.." && pwd) # (the checkout this script is in)
LOGS=$(mkdir -p "${PS5_LOGS:-$W/logs}/tune" && cd "${PS5_LOGS:-$W/logs}/tune" && pwd) || exit 1 # (absolute: the runs are made from $W)
T=$(cd "${PS5_TESTS:-$W/tests}" && pwd) || exit 1
[ -z "${PS5_HOST:-}" ] && [ -f "$W/tools/local.env" ] && . "$W/tools/local.env"
HOST=${PS5_HOST:?set PS5_HOST to the console address}
FTP=ftp://$HOST:2121/data/summerlessonvr2
cd "$W" || exit 1
# resolution.txt as it was, byte for byte (or none), put back however the sweep ends (testrun.sh's
# way with cvars.txt)
ORIG="$LOGS/resolution.txt.orig"
. "$W/tools/consfile.sh"
if consfile_left "$ORIG"; then # (a sweep before ended without putting it back: the .orig is the
  # console's own; .none: there was none, and the sweep's own could not be deleted)
  echo "an earlier sweep's $ORIG or $ORIG.none is still there: put the console's resolution.txt right (or delete it), then remove it"; exit 1
fi
had=
consfile_get "$FTP" resolution.txt "$ORIG"
case $? in
  0) had=1; echo "resolution.txt was $(cat "$ORIG")" ;;
  1) echo "resolution.txt: none" ;;
  *) echo "not starting"; exit 1 ;;
esac
restore() { consfile_put_back "$FTP" resolution.txt "$ORIG" "$had"; } # (0 when put back or removed)
trap 'st=$?; trap "" INT TERM; if restore; then rm -f "$ORIG"; else st=1; fi; exit $st' EXIT # (status 1 when not put right)
trap 'exit 130' INT TERM
knobs() { # start, step: base sh3 sh2 base ao2 ao0 base ssr2 base
  s=$1; d=$2; out=""
  for f in base sh3 sh2 base ao2 ao0 base ssr2 base; do out="$out $s:tools/fov/runs/tune-$f.txt"; s=$((s + d)); done
  echo "${out# }" # (no leading space: the runner reads its first item as the first switch)
}
run() { # title secs name pad head cvars_at
  name=$3
  rm -f "$LOGS/$name.log" # (a log from an earlier sweep never taken for this run's)
  i=0 # (the last game's sandbox gone first: testrun.sh refuses while one is mounted)
  while curl -s -m 10 "ftp://$HOST:2121/mnt/sandbox/pfsmnt/" | grep -qE "CUSA07434|CUSA11682|CUSA11674"; do
    i=$((i + 1)); [ $i -gt 30 ] && { echo "$name: a game still mounted after 60 s: stopping"; exit 1; }; sleep 2
  done
  PS5_HOST=$HOST PS5_REUSE=1 PS5_OTHER_PAYLOADS=${PS5_OTHER_PAYLOADS:-0} PS5_TITLE=$1 PS5_CVARS=tools/fov/runs/tune-base.txt PS5_CVARS_AT="$6" \
    sh tools/testrun.sh payload/summerlessonvr2.elf $2 "$LOGS/$name.log" "replay worn" "$4" "$5" > "$LOGS/$name.runner.txt" 2>&1
  st=$?
  if [ $st -ne 0 ] || ! grep -q "game gone" "$LOGS/$name.log" 2>/dev/null; then echo "$name: the runner gave $st or no 'game gone': stopping"; exit 1; fi
  if grep -q "hand: starting" "$LOGS/$name.log" || ! grep -q "vr2: no palm tracker (agent.hands=0)" "$LOGS/$name.log"; then
    echo "$name: not a run with the palm tracker off (docs/SUMMER-LESSON.md): stopping"; exit 1
  fi
  # (a debug payload and agent that played the replay, and the eyes as asked: the size applied)
  if ! grep -q "test: replaying" "$LOGS/$name.log" || ! grep -qE "^gpu: r.ScreenPercentage .* eyes ${eye}x" "$LOGS/$name.log"; then
    echo "$name: not the replay or not the eye size asked for ($eye; a resolution-<game>.txt?): stopping"; exit 1
  fi
  echo "$name: done ($(grep -c '^hmd: [0-9]* frames handed over' "$LOGS/$name.log") 10 s windows)"
}
for eye in 1200 1440 1600; do
  printf '%s' $eye | curl -s -m 10 -T - $FTP/resolution.txt || exit 1
  run CUSA11682 255 chisato-$eye "$T"/rec7-chisato.pad.txt "$T"/rec7-chisato-held.head.txt "$(knobs 62 21)"
  run CUSA11674 300 allison-$eye "$T"/rec8-allison.pad.txt "$T"/rec8-allison.head.txt ""
  run CUSA07434 260 hikari102room-$eye "$T"/rec1-102.pad.txt "$T"/rec1-102.head.txt "$(knobs 73 20)"
done
