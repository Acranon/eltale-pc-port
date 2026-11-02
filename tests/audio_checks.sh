#!/bin/sh
# The sound reference check: the game's own aspMain microcode (from the ROM, on the RSP interpreter in
# port/rsp) against the native audio (port/audio/aspmain.c) on every audio task of these runs, each
# whole task compared independently (ELTALE_AUDIO_CHECK=task: every DRAM byte either side wrote and the
# buffers in DMEM; a task that differs is run again a command at a time to name the command), and on
# some runs every task first run as a mutated copy too (ELTALE_AUDIO_FUZZ: random volumes, rates,
# pitches, gains, states, flags, lengths and full-scale noise). The game still plays the native
# result, so each run's P1_AUDIO hash is the one it has without the check (docs/AUDIO.md).
# usage: tests/audio_checks.sh path/to/eltale-p1[.exe] rom.z64 [quick saves folder] [--quick]
#   quick saves folder: holds battle-stuck/, items/, compass-spirit/ (default out/bugsaves)
#   --quick: New Game only
# Every line must say differing_tasks=0 differing_commands=0 (and differing_commands=0 for the fuzz);
# the last line is "audio check OK" or "audio check FAILED".
set -u
exe=$1
rom=$2
saves=${3:-out/bugsaves}
quick=0
for a in "$@"; do [ "$a" = "--quick" ] && quick=1; done
[ "$saves" = "--quick" ] && saves=out/bugsaves
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
fail=0

# run NAME SAVE FUZZ [VAR=value ...] -- ARGS: one headless run with the check
run() {
    name=$1 save=$2 fuzz=$3
    shift 3
    data="$tmp/$name"
    mkdir -p "$data"
    if [ "$save" != "-" ]; then
        if [ ! -d "$saves/$save" ]; then echo "$name: skipped (no quick save $saves/$save)"; return; fi
        cp -r "$saves/$save/." "$data/"
    fi
    vars=""
    while [ "$1" != "--" ]; do vars="$vars $1"; shift; done
    shift
    fz=""
    [ "$fuzz" != "0" ] && fz="ELTALE_AUDIO_FUZZ=$fuzz"
    env ELTALE_DATA_DIR="$data" ELTALE_AUDIO_CHECK=task $fz $vars "$exe" --headless --fast "$@" --rom "$rom" \
        >"$data/out.txt" 2>"$data/err.txt"
    hash=$(grep -o 'P1_AUDIO buffers=[0-9]* nonzero_bytes=[0-9]* hash=[0-9a-f]*' "$data/out.txt" | sed 's/.*hash=//')
    check=$(grep '^P1_AUDIO_CHECK mode=' "$data/err.txt" | sed 's/^P1_AUDIO_CHECK //')
    fzl=$(grep '^P1_AUDIO_FUZZ ' "$data/err.txt" | sed 's/^P1_AUDIO_FUZZ /fuzz /')
    echo "$name: audio=$hash $check${fzl:+ | $fzl}"
    case "$check" in *"differing_tasks=0 differing_commands=0"*) ;; *) fail=1 ;; esac
    if [ -n "$fzl" ]; then case "$fzl" in *"differing_commands=0"*) ;; *) fail=1 ;; esac; fi
    if [ -z "$hash" ]; then fail=1; tail -3 "$data/err.txt"; fi
    grep '^P1_AUDIO_CHECK [a-z_]*=' "$data/err.txt" | head -5
}

run new-game - 1 -- --frames 700 --input tests/title-new-game.input
if [ $quick = 0 ]; then
    # the intro and title (the attract sequence's music, sequence 27)
    run intro - 4 -- --frames 4000
    run ending - 4 ELTALE_START_MODE=5 -- --frames 10000
    run camera-tour - 0 ELTALE_START_MODE=7 -- --frames 10000
    # every sub-area of every map, battles casting spells
    run map-tour - 8 ELTALE_MAP_TOUR=1 -- --frames 300000
    run battle battle-stuck 2 ELTALE_QUICK_LOAD_AT=100 -- --frames 1500 --input tests/battle-stuck.input
    run spells battle-stuck 1 ELTALE_QUICK_LOAD_AT=100 -- --frames 1600 --input tests/battle-spells.input
    run item-menu battle-stuck 1 ELTALE_QUICK_LOAD_AT=100 -- --frames 650 --input tests/item-menu.input
    run items items 1 ELTALE_QUICK_LOAD_AT=100 -- --frames 260 --input tests/item-icons.input
    run spirit compass-spirit 1 ELTALE_QUICK_LOAD_AT=100 -- --frames 100 --input tests/spirit-haystack.input
    run pak-manager - 1 -- --frames 400 --input tests/pak-manager-delete.input --pak "$tmp/pak.mpk"
fi
rm -rf "$tmp"
if [ $fail = 0 ]; then echo "audio check OK"; else echo "audio check FAILED"; exit 1; fi
