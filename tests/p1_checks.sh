#!/bin/sh
# The P1 headless checks: New Game to gameplay, the ending, and the ending's camera tour.
# usage: tests/p1_checks.sh path/to/eltale-p1[.exe] rom.z64
# Each line prints P1_OS_OK (and P1_AUDIO for the first); compare against docs/P1_OS_STATUS.md.
set -u
exe=$1
rom=$2
cd "$(dirname "$0")/.."
echo "new-game:"
"$exe" --headless --fast --frames 700 --input tests/title-new-game.input --rom "$rom" 2>/dev/null | grep 'P1_OS_OK\|P1_AUDIO'
echo "ending (mode 5):"
ELTALE_START_MODE=5 "$exe" --headless --fast --frames 10000 --rom "$rom" 2>/dev/null | grep 'P1_OS_OK'
echo "tour (mode 7):"
ELTALE_START_MODE=7 "$exe" --headless --fast --frames 10000 --rom "$rom" 2>/dev/null | grep 'P1_OS_OK'
