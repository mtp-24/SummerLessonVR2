#!/bin/sh
# A run on the console without a player (payload/main.c: the control port's `launch` and
# `kill`; the agent's test mode, test.txt): the resident payload is asked to leave, this one is
# sent, the game is started, left to the pad.txt script (or the player) for a while, closed,
# and the log is pulled.
#
#   tools/testrun.sh <payload.elf> <seconds> <out.log> ["record|replay worn"] [pad.txt] [head.txt]
#
# The game must be closed. The test words go to /data/summerlessonvr2/test.txt (none: the file is
# removed, an ordinary start). pad.txt is the `pad:` lines of a recorded run without the prefix
# (tools/padscript.sh makes it from a log). PS5_HOST is the console's address, from the
# environment or from tools/local.env (not committed: a line PS5_HOST=<address>); PS5_REUSE=1
# keeps the resident instance (the same build) instead of swapping it: every swap is the moment
# the console has gone down in. PS5_TITLE picks the game (CUSA07434 Hikari, the default;
# CUSA11682 Chisato; CUSA11674 Allison).
#
# Settings for the run (optional; the console's own cvars.txt is kept and put back when the run
# ends, however it ends): PS5_CVARS=<file> adds that file's lines to the console's cvars.txt from
# the launch; PS5_CVARS_AT="<s>:<file> <s>:<file> ..." (in order) makes the added lines another
# file's <s> seconds after the launch (/dev/null: back to the console's own). The agent reads the
# file again every 2 s, so a switch shows in its log ("cvars.txt changed") within about 4 s:
# tools/fov/report.py cuts the log there.
set -u
LOCAL_ENV="$(dirname "$0")/local.env"
. "$(dirname "$0")/consfile.sh" # (consfile_get, consfile_put_back: cvars.txt kept and put back)
[ -z "${PS5_HOST:-}" ] && [ -f "$LOCAL_ENV" ] && . "$LOCAL_ENV"
HOST=${PS5_HOST:?set PS5_HOST to the IP address of the PS5}
TITLE=${PS5_TITLE:-CUSA07434}
TITLES="CUSA07434|CUSA11682|CUSA11674" # the payload's k_builds
echo "$TITLE" | grep -qxE "$TITLES" || { echo "PS5_TITLE $TITLE: not a known title"; exit 1; }
FTP="ftp://$HOST:2121"
DIR="/data/summerlessonvr2"
elf=$1
secs=$2
out=$3
words=${4:-}
pad=${5:-}
head=${6:-}

say() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*"; }
ctl() { printf '%s\n' "$1" | nc -w 5 "$HOST" 9088; }
log_tail() { curl -s -m 10 "$FTP$DIR/agent.log" | tail -n "${1:-5}"; }

# 1. the game closed
if curl -s -m 10 "$FTP/mnt/sandbox/pfsmnt/" | grep -qE "$TITLES"; then
  say "a Summer Lesson game is open (mounted): close it first"
  exit 1
fi

# 2. the resident instance asked to leave (refused while it serves a game); AstroBotVR2's own
# (port 9087), if resident, too: a payload loaded while another scans the console with the
# jailbreak's kernel access is the moment the console has gone down in
astro() { printf '%s\n' "$1" | nc -w 5 "$HOST" 9087; }
if [ "$(astro probe)" = yes ]; then
  say "AstroBotVR2 is resident (port 9087): asking it to quit"
  r=$(astro quit)
  if [ -n "$r" ]; then
    say "refused: $r"
    exit 1
  fi
  i=0
  while [ "$(astro probe)" = yes ]; do
    i=$((i + 1))
    [ $i -gt 20 ] && { say "it did not leave"; exit 1; }
    sleep 1
  done
  sleep 5
fi
reuse=${PS5_REUSE:-}
if [ -n "$reuse" ] && [ "$(ctl probe)" = yes ]; then
  say "keeping the resident instance"
elif [ "$(ctl probe)" = yes ]; then
  say "an instance is resident: asking it to quit"
  r=$(ctl quit)
  if [ -n "$r" ]; then
    say "refused: $r"
    exit 1
  fi
  i=0
  while [ "$(ctl probe)" = yes ]; do
    i=$((i + 1))
    [ $i -gt 20 ] && { say "it did not leave"; exit 1; }
    sleep 1
  done
  # its port closes before its process is gone: a payload sent in that moment is loaded over
  # a resident one, which has brought the console down (four times for AstroBotVR2 with 5 s, and
  # once more for SummerLessonVR2 on 2026-10-31 22:33, run 9, after 5 s): 15 s now
  sleep ${PS5_QUIT_WAIT:-15}
fi

# 3. the settings, and a fresh log
if [ -n "$words" ]; then
  printf '%s\n' "$words" | curl -s -m 10 -T - "$FTP$DIR/test.txt" || exit 1
else
  curl -s -m 10 "$FTP/" -Q "DELE $DIR/test.txt" > /dev/null 2>&1
fi
if [ -n "$pad" ]; then
  curl -s -m 10 -T "$pad" "$FTP$DIR/pad.txt" || exit 1
fi
if [ -n "$head" ]; then
  curl -s -m 10 -T "$head" "$FTP$DIR/head.txt" || exit 1
fi
# the console's own cvars.txt while a run has its own there: one fixed place for every run of this
# checkout, so a run whose put-back failed stops the next (whatever its log), and a copy of it next
# to the run's log once put back
cvars_orig="$(cd "$(dirname "$0")" && pwd)/.cvars.orig"
cvars_had=
cvars_put() { # the console's own lines, then the file's
  { [ -n "$cvars_had" ] && cat "$cvars_orig" && echo; [ -n "$1" ] && cat "$1"; } |
    curl -s -m 10 -T - "$FTP$DIR/cvars.txt"
}
cvars_restore() { # (0 when put back or removed; said either way)
  cf_say=$(consfile_put_back "$FTP$DIR" cvars.txt "$cvars_orig" "$cvars_had")
  cf_st=$?
  say "$cf_say"
  if [ $cf_st -eq 0 ] && [ -n "$cvars_had" ]; then mv -f "$cvars_orig" "$out.cvars.orig"; fi
  return $cf_st
}
cvars_pending="${PS5_CVARS_AT:-}"
if [ -n "${PS5_CVARS:-}$cvars_pending" ]; then
  for item in ${PS5_CVARS:-} $cvars_pending; do
    f=${item#*:}
    [ -f "$f" ] || [ "$f" = /dev/null ] || { say "no such settings file: $f"; exit 1; }
  done
  # (a read that failed for any reason but "no such file": not started, or the console's own would
  # be deleted at the end as if there were none)
  if consfile_left "$cvars_orig"; then
    say "an earlier run's $cvars_orig (the console's own cvars.txt) or its .none is still there: put the console's cvars.txt right (or delete it), then remove it"
    exit 1
  fi
  cf_say=$(consfile_get "$FTP$DIR" cvars.txt "$cvars_orig")
  case $? in
    0) cvars_had=1 ;;
    1) ;;
    *) say "$cf_say: not starting"; exit 1 ;;
  esac
  trap 'st=$?; trap "" INT TERM; cvars_restore || st=1; exit $st' EXIT # (the run's status 1 when cvars.txt was not put right)
  trap 'exit 130' INT TERM
  cvars_put "${PS5_CVARS:-}" || { say "cvars.txt could not be written"; exit 1; }
  say "cvars.txt for the run: the console's own${cvars_had:+ ($(wc -l < "$cvars_orig" | tr -d ' ') lines)} + ${PS5_CVARS:-nothing}"
fi
curl -s -m 10 "$FTP/" -Q "DELE $DIR/agent.log" > /dev/null 2>&1
# the agent as built: the payload injects this file rather than its built-in copy, so a new
# agent needs no payload swap (a swap has brought the console down three times)
agent_elf="$(dirname "$elf")/../agent/build/agent.elf"
if [ -f "$agent_elf" ]; then
  curl -s -m 20 -T "$agent_elf" "$FTP$DIR/agent.elf" || { say "the agent could not be uploaded"; exit 1; }
fi

# 4. the payload
if [ -z "$reuse" ] || [ "$(ctl probe)" != yes ]; then
  say "sending $elf"
  nc -w 5 "$HOST" 9021 < "$elf" > /dev/null &
  ncpid=$!
  i=0
  while [ "$(ctl probe)" != yes ]; do
    i=$((i + 1))
    [ $i -gt 30 ] && { say "the payload did not come up"; kill $ncpid 2> /dev/null; exit 1; }
    sleep 1
  done
  sleep 3
  kill $ncpid 2> /dev/null
fi
# every ELF elfldr loads is a payload.elf: this one, and PS5_OTHER_PAYLOADS more that the console
# autoloads (one here: pldmgr's sm-badges); any more is a stray injector
expect=$((1 + ${PS5_OTHER_PAYLOADS:-1}))
payloads=$(ctl ps | grep -c payload.elf)
if [ "$payloads" != "$expect" ]; then
  say "$payloads payload.elf processes, $expect expected (a stray injector?): not launching"
  ctl ps | grep "payload.elf\|elfldr"
  exit 1
fi

# 5. the game. The console's kernel log (klogsrv, port 3232) is streamed to $out.klog and the
# agent's log copied to $out.partial every 10 s: a console crash loses what was only on the
# console (its page cache never reaches the disk; agent.log was 0 bytes after the 01:39 crash)
# (nc hangs up when its input ends: a background job's input is empty, so it is fed a sleep.
# The console's klogsrv serves one client: a leftover connection is closed first, and nothing is
# launched unless the log is seen flowing)
pkill -f "nc $HOST 3232" 2> /dev/null
sleep 1
(sleep 3600 | nc "$HOST" 3232 > "$out.klog" 2> /dev/null) &
klogpid=$!
i=0
klogfile=
sleep 2
while [ ! -s "$out.klog" ]; do
  i=$((i + 1))
  if [ $i -gt 5 ]; then
    # the payload opened /dev/klog first (it keeps it in $DIR/klog.txt): that file from here on
    kill $klogpid 2> /dev/null
    pkill -f "nc $HOST 3232" 2> /dev/null
    klogfile=$(curl -s -m 10 "$FTP$DIR/" | awk '$9 == "klog.txt" {print $5; exit}')
    [ -n "$klogfile" ] || { say "no kernel log (klogsrv silent, no klog.txt): not launching"; exit 1; }
    : > "$out.klog"
    break
  fi
  sleep 1
done
if [ -z "$klogfile" ] && [ ! -s "$out.klog" ]; then say "the kernel log is empty: not launching"; exit 1; fi
if [ -n "$klogfile" ]; then say "kernel log: klog.txt from byte $klogfile"; else say "kernel log: klogsrv, $(wc -c < "$out.klog" | tr -d ' ') bytes so far"; fi
say "launch: $(ctl "launch $TITLE")"
launched=$(date +%s)
end=$((launched + secs))
# every tick, how far each log had come then ($out.ticks): a line of either placed in time within
# 2 s of the other's (the agent's lines carry the game's process time, the kernel log's none)
rm -f "$out.ticks"
tick=0
while [ "$(date +%s)" -lt "$end" ]; do
  left=$((end - $(date +%s)))
  [ $left -gt 2 ] && sleep 2 || sleep "$left"
  # (a failed fetch, the console gone, must not empty the last copy: it is all there is then)
  curl -s -m 8 "$FTP$DIR/agent.log" > "$out.partial.new" 2> /dev/null &&
    [ -s "$out.partial.new" ] && mv "$out.partial.new" "$out.partial"
  rm -f "$out.partial.new"
  [ -n "$klogfile" ] && curl -s -m 8 -r "$klogfile-" "$FTP$DIR/klog.txt" > "$out.klog.new" 2> /dev/null &&
    [ -s "$out.klog.new" ] && mv "$out.klog.new" "$out.klog"
  printf '%s agent.log %s lines, klog %s bytes\n' "$(date +%H:%M:%S)" \
    "$(cat "$out.partial" 2> /dev/null | wc -l | tr -d ' ')" "$(wc -c < "$out.klog" | tr -d ' ')" >> "$out.ticks"
  # the settings' switches due by now (PS5_CVARS_AT)
  while [ -n "$cvars_pending" ]; do
    item=${cvars_pending%% *}
    at=${item%%:*}
    [ $(($(date +%s) - launched)) -ge "$at" ] || break
    cvars_put "${item#*:}" && say "cvars.txt at $at s: the console's own + ${item#*:}"
    case "$cvars_pending" in *" "*) cvars_pending=${cvars_pending#* } ;; *) cvars_pending= ;; esac
  done
  tick=$((tick + 1))
  [ $((tick % 15)) -eq 0 ] && say "$((end - $(date +%s))) s left: $(tail -n 1 "$out.partial" | cut -c1-110)"
done

# 6. closed, the log pulled
say "kill: $(ctl kill)"
i=0
until log_tail 30 | grep -q "game gone"; do
  i=$((i + 1))
  [ $i -gt 60 ] && { say "no 'game gone' in the log yet"; break; }
  sleep 1
done
curl -s -m 60 "$FTP$DIR/agent.log" > "$out"
kill $klogpid 2> /dev/null
pkill -f "nc $HOST 3232" 2> /dev/null
curl -s -m 10 "$FTP/" -Q "DELE $DIR/test.txt" > /dev/null 2>&1 # (read once by the payload anyway)
say "log: $out ($(wc -l < "$out" | tr -d ' ') lines)"
grep -n "launch:\|kill:\|test:\|hmd: \|headset\|game gone\|hooks live\|load failed\|entry point\|through the traps\|resolution:" "$out" | tail -40
say "$(grep -c '^pad: [0-9]' "$out") pad lines and $(grep -c '^head: [0-9]' "$out") head lines recorded"
