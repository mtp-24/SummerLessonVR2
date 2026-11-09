# tools/consfile.sh: a console file kept and put back around a run (sourced by tools/testrun.sh and
# tools/fov/runs/tune-*.sh; plain sh). The console's file is read into a backup first; at the end it
# is put back byte for byte, or deleted when there was none. A delete counts only when the file then
# reads as missing (curl 78: whatever the delete itself said), and one that failed leaves
# <backup>.none, which the sweeps take as a reason not to start (consfile_left).
#
#   consfile_get <dir url> <name> <backup>
#       0: the console's file is in <backup>; 1: there is none (no backup); 2: it could not be read
#       (curl's error, no backup): don't start, as nothing is known about the console's own
#   consfile_put_back <dir url> <name> <backup> <had: non-empty when there was one>
#       0: put back or deleted; 1: not (the backup kept, or <backup>.none made), said
#   consfile_left <backup>: 0 when an earlier run left <backup> or <backup>.none
# <dir url>: ftp://host:port/dir (the delete goes to its server root with the dir's path).
# The callers' EXIT traps ignore INT and TERM while they put back (a second Ctrl-C not cutting it short).

consfile_get() {
  rm -f "$3"
  curl -s -m 10 -o "$3" "$1/$2" 2> /dev/null
  cf_rc=$?
  if [ $cf_rc -eq 0 ] && [ -f "$3" ]; then return 0; fi
  rm -f "$3"
  [ $cf_rc -eq 78 ] && return 1
  echo "$2 could not be read (curl $cf_rc)"
  return 2
}

consfile_put_back() {
  if [ -n "$4" ]; then
    if curl -s -m 10 -T "$3" "$1/$2"; then echo "$2: the console's own back"; return 0; fi
    echo "$2 NOT put back: the console's own is $3"
    return 1
  fi
  : > "$3.none" # (made first: a put-back cut short, by a signal or a kill, still leaves it)
  cf_root=${1%"${1#ftp://*/}"} # (ftp://host:port/)
  curl -s -m 10 "$cf_root" -Q "DELE /${1#ftp://*/}/$2" > /dev/null 2>&1
  curl -s -m 10 -o /dev/null "$1/$2" 2> /dev/null
  if [ $? -eq 78 ]; then rm -f "$3.none"; echo "$2: removed (there was none)"; return 0; fi
  echo "$2 NOT removed (there was none before the run): delete it on the console, then remove $3.none"
  return 1
}

consfile_left() {
  [ -f "$1" ] || [ -f "$1.none" ]
}
