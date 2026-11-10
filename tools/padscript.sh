#!/bin/sh
# The scripts (pad.txt and head.txt, tools/testrun.sh) from the log of a run recorded with
# test.txt `record`: the `pad:` (or `head:`) lines without their prefix.
#   tools/padscript.sh <agent.log> [pad|head] > pad.txt
grep -o "^${2:-pad}: [0-9].*" "$1" | sed 's/^[a-z]*: //'
