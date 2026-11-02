#!/bin/sh
# The sound reference check: the game's own aspMain microcode (from the ROM, on the RSP interpreter in
# port/rsp) against the native audio (port/audio/aspmain.c) on every audio task of these runs, each
# whole task compared independently (ELTALE_AUDIO_CHECK=task: every DRAM byte either side wrote and the
# buffers in DMEM; a task that differs is run again a command at a time to name the command), and on
# most runs tasks also first run as mutated copies (ELTALE_AUDIO_FUZZ: random volumes, rates, pitches,
# gains, states, flags, lengths and full-scale noise; everything undone). See docs/AUDIO.md.
# Each case runs twice: plain, then checked. The game plays the native result either way, so the two
# P1_AUDIO hashes must be the same.
# usage: tests/audio_checks.sh path/to/eltale-p1[.exe] rom.z64 [quick saves folder] [--quick]
#   quick saves folder: holds battle-stuck/, items/, compass-spirit/ (default out/bugsaves)
#   --quick: New Game only
# Each case prints its audio hash, the check's summary (differing_tasks=0 differing_commands=0) and the
# fuzzer's (differing_commands=0), and the first differences if there are any; the last line is
# "audio check OK" or "audio check FAILED".
set -u
exe=$1
rom=$2
saves=out/bugsaves
quick=0
for a in "${3:-}" "${4:-}"; do
    case "$a" in --quick) quick=1 ;; "") ;; *) saves=$a ;; esac
done
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
fail=0

# one headless run: run_one DIR CHECK FUZZ VARS ARGS...
run_one() {
    r_dir=$1 r_check=$2 r_fuzz=$3 r_vars=$4
    shift 4
    set -- env ELTALE_DATA_DIR="$r_dir" $r_vars "$exe" --headless --fast "$@" --rom "$rom"
    if [ "$r_check" = 1 ]; then
        if [ "$r_fuzz" != 0 ]; then set -- env ELTALE_AUDIO_CHECK=task ELTALE_AUDIO_FUZZ="$r_fuzz" "$@"
        else set -- env ELTALE_AUDIO_CHECK=task "$@"; fi
    fi
    "$@" >"$r_dir/out.txt" 2>"$r_dir/err.txt"
}

# case NAME SAVE FUZZ "VARS" ARGS...: SAVE is a quick save folder or -, FUZZ every k-th task or 0
case_() {
    name=$1 save=$2 fuzz=$3 vars=$4
    shift 4
    for k in plain check; do
        d="$tmp/$name-$k"
        mkdir -p "$d"
        if [ "$save" != "-" ]; then
            if [ ! -d "$saves/$save" ]; then echo "$name: skipped (no quick save $saves/$save)"; return; fi
            cp -r "$saves/$save/." "$d/"
        fi
    done
    # (a fresh copy of the Pak for each run: the Pak manager case deletes its save)
    [ -f "$tmp/pak.mpk" ] && cp "$tmp/pak.mpk" "$tmp/pak-run.mpk"
    run_one "$tmp/$name-plain" 0 0 "$vars" "$@"
    [ -f "$tmp/pak.mpk" ] && cp "$tmp/pak.mpk" "$tmp/pak-run.mpk"
    run_one "$tmp/$name-check" 1 "$fuzz" "$vars" "$@"
    d="$tmp/$name-check"
    plain=$(sed -n 's/^P1_AUDIO buffers=.* hash=\([0-9a-f]*\).*/\1/p' "$tmp/$name-plain/out.txt")
    hash=$(sed -n 's/^P1_AUDIO buffers=.* hash=\([0-9a-f]*\).*/\1/p' "$d/out.txt")
    check=$(sed -n 's/^P1_AUDIO_CHECK \(mode=\)/\1/p' "$d/err.txt")
    fzl=$(sed -n 's/^P1_AUDIO_FUZZ /fuzz /p' "$d/err.txt")
    same="same as plain"
    if [ -z "$hash" ] || [ "$hash" != "$plain" ]; then same="PLAIN RUN GAVE ${plain:-nothing}"; fail=1; fi
    echo "$name: audio=$hash ($same) $check${fzl:+ | $fzl}"
    case "$check" in *"differing_tasks=0 differing_commands=0"*) ;; *) fail=1 ;; esac
    if [ -n "$fzl" ]; then case "$fzl" in *"differing_commands=0"*) ;; *) fail=1 ;; esac; fi
    grep -A2 '^P1_AUDIO_CHECK [a-z_]*=[0-9]' "$d/err.txt" | head -12
    [ -z "$hash" ] && tail -3 "$d/err.txt"
}

case_ new-game - 1 "" --frames 700 --input tests/title-new-game.input
if [ $quick = 0 ]; then
    # the intro and title (the attract sequence's music, sequence 27)
    case_ intro - 4 "" --frames 4000
    case_ ending - 4 "ELTALE_START_MODE=5" --frames 10000
    case_ camera-tour - 0 "ELTALE_START_MODE=7" --frames 10000
    # every sub-area of every map, battles casting spells
    case_ map-tour - 8 "ELTALE_MAP_TOUR=1" --frames 300000
    case_ battle battle-stuck 2 "ELTALE_QUICK_LOAD_AT=100" --frames 1500 --input tests/battle-stuck.input
    case_ spells battle-stuck 1 "ELTALE_QUICK_LOAD_AT=100" --frames 1600 --input tests/battle-spells.input
    case_ item-menu battle-stuck 1 "ELTALE_QUICK_LOAD_AT=100" --frames 650 --input tests/item-menu.input
    case_ items items 1 "ELTALE_QUICK_LOAD_AT=100" --frames 260 --input tests/item-icons.input
    case_ spirit compass-spirit 1 "ELTALE_QUICK_LOAD_AT=100" --frames 100 --input tests/spirit-haystack.input
    # a save at the inn (as tests/p1_checks.sh makes it), then Continue from the title loading it (the
    # Pak manager is left out: its run ends before the summaries print)
    mkdir -p "$tmp/pak"
    ELTALE_DATA_DIR="$tmp/pak" ELTALE_MAP_TOUR=1 "$exe" --headless --fast --frames 16000 --input tests/tour-save.input \
        --pak "$tmp/pak.mpk" --rom "$rom" >/dev/null 2>&1
    case_ load-save - 1 "ELTALE_LOAD_TEST=1" --frames 5000 --pak "$tmp/pak-run.mpk"
fi
rm -rf "$tmp"
if [ $fail = 0 ]; then echo "audio check OK"; else echo "audio check FAILED"; exit 1; fi
