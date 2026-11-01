#!/bin/sh
# The P1 headless checks: the English text's fit (tools/text/fit_check.py), New Game to gameplay, the
# ending, and the ending's camera tour; with --tour
# also every map, saving and loading, the Controller Pak manager and quick saves.
# usage: tests/p1_checks.sh path/to/eltale-p1[.exe] rom.z64 [--tour]
# Each line prints P1_OS_OK (and P1_AUDIO for the first); compare against docs/P1_OS_STATUS.md.
set -u
exe=$1
rom=$2
cd "$(dirname "$0")/.."
# every English string against the window it is drawn in, in all six fonts (no game run)
echo "english fit:"
python3 tools/text/fit_check.py "$rom" 2>&1 | tail -1
echo "new-game:"
"$exe" --headless --fast --frames 700 --input tests/title-new-game.input --rom "$rom" 2>/dev/null | grep 'P1_OS_OK\|P1_AUDIO'
echo "ending (mode 5):"
ELTALE_START_MODE=5 "$exe" --headless --fast --frames 10000 --rom "$rom" 2>/dev/null | grep 'P1_OS_OK'
echo "tour (mode 7):"
ELTALE_START_MODE=7 "$exe" --headless --fast --frames 10000 --rom "$rom" 2>/dev/null | grep 'P1_OS_OK'
if [ "${3:-}" = "--tour" ]; then
    # every sub-area of every map (a few minutes): ELTALE_MAP_TOUR's own pad, no script
    echo "map tour:"
    ELTALE_MAP_TOUR=1 "$exe" --headless --fast --frames 300000 --rom "$rom" 2>/dev/null | grep 'P1_OS_OK\|P1_AUDIO'
fi
if [ "${3:-}" = "--tour" ]; then
    # save at the inn on map 18 (tour-save.input talks to the innkeeper), then Continue from the title
    echo "save and load:"
    pak=$(mktemp)
    rm -f "$pak"
    ELTALE_MAP_TOUR=1 "$exe" --headless --fast --frames 16000 --input tests/tour-save.input --pak "$pak" --rom "$rom" >/dev/null 2>&1
    ELTALE_LOAD_TEST=1 "$exe" --headless --fast --frames 5000 --pak "$pak" --rom "$rom" 2>&1 >/dev/null | grep 'P1_LOAD'
    # the Controller Pak manager deletes that save: its last screen is the empty list
    echo "pak manager:"
    "$exe" --headless --fast --frames 400 --input tests/pak-manager-delete.input --pak "$pak" --rom "$rom" 2>/dev/null |
        grep 'P1_CPU_FRAME' | tail -1
    rm -f "$pak"
    # a quick save (F2) after a walk, then a quick load (F4) from the title in a new process
    echo "quick save and load:"
    data=$(mktemp -d)
    ELTALE_DATA_DIR="$data" ELTALE_QUICK_SAVE_AT=1500 "$exe" --headless --fast --frames 800 \
        --input tests/quicksave-walk.input --rom "$rom" 2>&1 >/dev/null | grep 'P1_QUICKSAVE'
    ELTALE_DATA_DIR="$data" ELTALE_QUICK_LOAD_AT=100 "$exe" --headless --fast --frames 200 \
        --input tests/intro-to-title.input --rom "$rom" 2>&1 >/dev/null | grep 'P1_QUICKLOAD'
    # that quick save rewritten (tools/quicksave.py) and loaded from the title: saved in the evening
    # (the first map frame of the process in the evening palette's hours), as a later build with a
    # longer PC block writes it, and of another version, cut short or damaged (refused, play goes on)
    echo "quick saves of other times and builds:"
    keep="$data/keep.elq"
    cp "$data/quicksaves/quick0.elq" "$keep"
    for edit in "--clock 0x5676" "--pc-extra 16" "--version 2" "--truncate 300" "--damage"; do
        python3 tools/quicksave.py edit "$keep" "$data/quicksaves/quick0.elq" $edit
        printf '%s: ' "$edit"
        ELTALE_DATA_DIR="$data" ELTALE_QUICK_LOAD_AT=100 "$exe" --headless --fast --frames 200 \
            --input tests/intro-to-title.input --rom "$rom" 2>&1 >/dev/null | grep 'P1_QUICK\|P1 crash' | tr '\n' ' '
        echo
    done
    rm -rf "$data"
fi
