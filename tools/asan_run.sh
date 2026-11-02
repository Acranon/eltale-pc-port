#!/usr/bin/env bash
# The memory-safety sweep (docs/MEMORY_SAFETY.md): the sanitizer build through the headless checks, one game process
# at a time, every report kept, then a list of the distinct findings (tools/asan_summary.py).
#
#     make asan ROM=rom.z64                  (out/asan/eltale-p1 + the stub host libeltale_host.so)
#     tools/asan_run.sh --rom rom.z64 [--saves DIR] [--logs DIR] [--exe out/asan/eltale-p1] [SUITE...]
#
# Suites (default: checks frames):
#   checks  tests/p1_checks.sh --tour: New Game, ending, camera tour, map tour, save/load, Pak manager, quick saves;
#           the hashes it prints must be the normal build's (docs/P1_OS_STATUS.md)
#   frames  every run of tests/render_frames*.txt (battles, spells, menus, saves, chests, ...) with --dump-frames (the
#           stub host: the game's native draw side runs), and again with ELTALE_FRAME_IMAGE (the frame image)
#   native  the long plays with --dump-frames: New Game, ending, camera tour, map tour, English, 16:9, small buffers
#   long    battles the player walks away from (the enemies attack), the map tour with ELTALE_BATTLE_LOG=2, and each
#           save's battle (spells cast), 20000 frames each
#   saves   a Controller Pak with two saves (tools/pak_two_saves.py): load the first, the save menu, the Pak manager
#   rdram   RDRAM poisoned until loaded (ELTALE_ASAN_RDRAM=loaded): New Game, ending, tours, battles, the menu
#   tools   the headless test tools against this build: spell_ui_test, mouse_menu_test, pause_menu_test, interp_test
#           (their picture checks fail on the stub's blank pictures; only the reports count here)
# Saves: the quick saves the lists name (default C:/code/eltale/out/bugsaves), copied into a scratch data folder.
# Logs: ASan/UBSan reports go to LOGS/<run>.<pid> (ASAN_OPTIONS log_path), stdout to LOGS/<run>.out.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$ROOT/out/asan/eltale-p1"
ROM="${ELTALE_ROM:-}"
SAVES=/mnt/c/code/eltale/out/bugsaves
LOGS="$ROOT/out/asan-logs"
SUITES=()
while [ $# -gt 0 ]; do
    case "$1" in
        --rom) ROM="$2"; shift 2 ;;
        --exe) EXE="$2"; shift 2 ;;
        --saves) SAVES="$2"; shift 2 ;;
        --logs) LOGS="$2"; shift 2 ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
        *) SUITES+=("$1"); shift ;;
    esac
done
[ -n "$ROM" ] || { echo "supply --rom" >&2; exit 2; }
[ ${#SUITES[@]} -gt 0 ] || SUITES=(checks frames)
EXE="$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")"
mkdir -p "$LOGS"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
cd "$ROOT"

# run NAME SAVE TIMEOUT ENV... -- ARGS...: one game process, its reports to LOGS/NAME.<pid>
run() {
    local name=$1 save=$2 limit=$3; shift 3
    local envs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    local data="$SCRATCH/data-$name"
    rm -rf "$data" "$SCRATCH/frames"; mkdir -p "$data" "$SCRATCH/frames"
    if [ -n "$save" ]; then cp -r "$SAVES/$save/." "$data/"; fi
    local start=$SECONDS
    env $(compgen -e | grep '^ELTALE_' | sed 's/^/-u /') ELTALE_DATA_DIR="$data" \
        ASAN_OPTIONS="log_path=$LOGS/$name" UBSAN_OPTIONS="log_path=$LOGS/$name" ELTALE_ASAN_LOG="$LOGS/$name.own" \
        "${envs[@]}" timeout "$limit" "$EXE" --headless --fast "$@" --rom "$ROM" > "$LOGS/$name.out" 2> "$LOGS/$name.err"
    local rc=$?
    local ok; ok=$(grep -a 'P1_OS_OK' "$LOGS/$name.out" | tail -1)
    local audio; audio=$(grep -a 'P1_AUDIO' "$LOGS/$name.out" | tail -1 | sed 's/.*hash=/audio=/')
    local n; n=$(cat "$LOGS/$name".[0-9]* 2>/dev/null | grep -c 'ERROR: AddressSanitizer\|runtime error')
    printf '%-28s rc=%-3s %4ss reports=%-4s %s %s\n' "$name" "$rc" $((SECONDS - start)) "$n" "${ok:-no P1_OS_OK}" "$audio" |
        tee -a "$LOGS/runs.txt"
}

# the runs of a render_frames list: NAME | [@save=S] [ENV=V...] ARGS | frames
frames_list() {
    local list=$1 mode=$2
    grep -v '^#' "$list" | grep '|' | while IFS='|' read -r name spec frames; do
        name=$(echo "$name" | tr -d ' ')
        local save="" envs=() args=()
        for w in $spec; do
            case "$w" in
                @save=*) save=${w#@save=} ;;
                ELTALE_*=*) envs+=("$w") ;;
                *) args+=("$w") ;;
            esac
        done
        frames=$(echo $frames | tr ' ' ',')
        if [ "$mode" = native ]; then
            run "frames-$name" "$save" 3600 "${envs[@]}" ELTALE_DUMP_FRAMES="$frames" -- "${args[@]}" --dump-frames "$SCRATCH/frames" < /dev/null
        else
            run "image-$name" "$save" 3600 "${envs[@]}" ELTALE_FRAME_IMAGE=1 ELTALE_DUMP_IMAGE="$frames" -- "${args[@]}" < /dev/null
        fi
    done
}

for suite in "${SUITES[@]}"; do
    echo "== $suite" | tee -a "$LOGS/runs.txt"
    case "$suite" in
        checks)
            ASAN_OPTIONS="log_path=$LOGS/checks" UBSAN_OPTIONS="log_path=$LOGS/checks" ELTALE_ASAN_LOG="$LOGS/checks.own" \
                sh tests/p1_checks.sh "$EXE" "$ROM" --tour 2>&1 | tee "$LOGS/checks.out" | tee -a "$LOGS/runs.txt" ;;
        frames)
            for list in tests/render_frames*.txt; do frames_list "$list" native; done
            for list in tests/render_frames.txt tests/render_frames_2d.txt; do frames_list "$list" image; done ;;
        native)
            run native-newgame "" 600 -- --frames 3000 --input tests/title-new-game.input --dump-frames "$SCRATCH/frames"
            run native-newgame-en "" 600 ELTALE_LANG=en -- --frames 3000 --input tests/title-new-game.input --dump-frames "$SCRATCH/frames"
            run native-ending "" 1200 ELTALE_START_MODE=5 -- --frames 10000 --dump-frames "$SCRATCH/frames"
            run native-ending-en "" 1200 ELTALE_START_MODE=5 ELTALE_LANG=en -- --frames 10000 --dump-frames "$SCRATCH/frames"
            run native-camtour "" 1200 ELTALE_START_MODE=7 -- --frames 10000 --dump-frames "$SCRATCH/frames"
            run native-maptour "" 7200 ELTALE_MAP_TOUR=1 -- --frames 300000 --dump-frames "$SCRATCH/frames"
            run native-maptour-en-wide "" 7200 ELTALE_MAP_TOUR=1 ELTALE_LANG=en ELTALE_WIDESCREEN=16:9 \
                -- --frames 300000 --dump-frames "$SCRATCH/frames"
            run native-maptour-small "" 7200 ELTALE_MAP_TOUR=1 ELTALE_ND_SMALL_BUFFERS=256 -- --frames 300000 --dump-frames "$SCRATCH/frames" ;;
        long)
            # battles where the player steps back and swings at the air: the enemies attack, battles end and start again
            printf '5 0x1000 0 0\n7 0 0 0\n120 0 0 80\n200 0 0 0\n230 0 0 -80\n250 0 0 0\n260 0x8000 0 0\n262 0 0 0\nrepeat 230 60\n' \
                > "$SCRATCH/away.input"
            run long-walkaway battle-stuck 3600 ELTALE_QUICK_LOAD_AT=100 ELTALE_BATTLE_LOG=2 \
                -- --frames 20000 --input "$SCRATCH/away.input" --dump-frames "$SCRATCH/frames"
            run long-maptour-battles "" 7200 ELTALE_MAP_TOUR=1 ELTALE_BATTLE_LOG=2 -- --frames 300000 --dump-frames "$SCRATCH/frames"
            for s in "$SAVES"/*/; do
                s=$(basename "$s")
                run "long-battle-$s" "$s" 3600 ELTALE_QUICK_LOAD_AT=100 ELTALE_BATTLE_LOG=2 \
                    -- --frames 20000 --input tests/battle-spells.input --dump-frames "$SCRATCH/frames"
            done ;;
        rdram)
            # RDRAM poisoned until a ROM DMA loads it (port/p0/asan_rdram.c): reads and writes of RDRAM nothing loaded
            for r in "ng||--frames 3000 --input tests/title-new-game.input" "ending|ELTALE_START_MODE=5|--frames 10000" \
                     "camtour|ELTALE_START_MODE=7|--frames 10000" "maptour|ELTALE_MAP_TOUR=1|--frames 300000" \
                     "maptour-native|ELTALE_MAP_TOUR=1|--frames 300000 --dump-frames $SCRATCH/frames" \
                     "battle|ELTALE_QUICK_LOAD_AT=100 ELTALE_BATTLE_LOG=2|--frames 20000 --input tests/battle-spells.input" \
                     "walkaway|ELTALE_QUICK_LOAD_AT=100 ELTALE_BATTLE_LOG=2|--frames 20000 --input $SCRATCH/away.input" \
                     "menu|ELTALE_QUICK_LOAD_AT=100|--frames 650 --input tests/item-menu.input"; do
                IFS='|' read -r n e a <<< "$r"
                printf '5 0x1000 0 0\n7 0 0 0\n120 0 0 80\n200 0 0 0\n230 0 0 -80\n250 0 0 0\n260 0x8000 0 0\n262 0 0 0\nrepeat 230 60\n' \
                    > "$SCRATCH/away.input"
                save=""; case "$n" in battle|walkaway|menu) save=battle-stuck ;; esac
                run "rdram-$n" "$save" 7200 ELTALE_ASAN_RDRAM=loaded $e -- $a
            done ;;
        saves)
            pak="$SCRATCH/one.mpk"
            rm -f "$pak"
            run saves-make "" 1200 ELTALE_MAP_TOUR=1 -- --frames 16000 --input tests/tour-save.input --pak "$pak"
            for tail in keep 0 0x55; do
                if [ "$tail" = keep ]; then python3 tools/pak_two_saves.py "$pak" "$SCRATCH/two.mpk" > /dev/null
                else python3 tools/pak_two_saves.py "$pak" "$SCRATCH/two.mpk" "$tail" > /dev/null; fi
                cp "$SCRATCH/two.mpk" "$SCRATCH/t.mpk"
                run "saves-load-$tail" "" 1200 ELTALE_LOAD_TEST=1 -- --frames 5000 --pak "$SCRATCH/t.mpk"
                grep -a P1_LOAD "$LOGS/saves-load-$tail.err" | tee -a "$LOGS/runs.txt"
                cp "$SCRATCH/two.mpk" "$SCRATCH/t.mpk"
                run "saves-continue-$tail" "" 1200 -- --frames 500 --input tests/title-continue.input --pak "$SCRATCH/t.mpk" \
                    --dump-frames "$SCRATCH/frames"
                cp "$SCRATCH/two.mpk" "$SCRATCH/t.mpk"
                # (one save is left after the delete, so the manager stays open and --frames is never reached: 60 s)
                run "saves-pakmanager-$tail" "" 60 -- --frames 400 --input tests/pak-manager-delete.input --pak "$SCRATCH/t.mpk"
            done ;;
        tools)
            for t in spell_ui_test mouse_menu_test pause_menu_test interp_test; do
                start=$SECONDS
                if [ $t = interp_test ]; then where=--work; else where=--out; fi
                ASAN_OPTIONS="log_path=$LOGS/tool-$t" UBSAN_OPTIONS="log_path=$LOGS/tool-$t" ELTALE_ASAN_LOG="$LOGS/tool-$t.own" \
                    timeout 7200 python3 tools/$t.py --exe "$EXE" --rom "$ROM" --saves "$SAVES" $where "$SCRATCH/tool-$t" \
                    > "$LOGS/tool-$t.out" 2>&1
                rc=$?
                n=$(cat "$LOGS/tool-$t".[0-9]* 2>/dev/null | grep -c 'ERROR: AddressSanitizer\|runtime error')
                printf '%-28s rc=%-3s %4ss reports=%s (%s)\n' "tool-$t" "$rc" $((SECONDS - start)) "$n" \
                    "$(tail -1 "$LOGS/tool-$t.out" | cut -c1-100)" | tee -a "$LOGS/runs.txt"
            done ;;
        *) echo "unknown suite $suite" >&2 ;;
    esac
done
python3 tools/asan_summary.py "$LOGS"/*.[0-9]* "$LOGS"/*.own* 2>/dev/null | tee "$LOGS/summary.txt"
