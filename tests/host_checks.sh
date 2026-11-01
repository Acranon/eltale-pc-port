#!/bin/sh
# The window checks: the host (eltale_host.dll) with each renderer, in a hidden window
# (ELTALE_HIDDEN_WINDOW), sound at volume 0, a temporary data folder each run. Run them only when a
# hidden window is fine (it shows nothing, but it uses the GPU and opens the sound device).
# usage: tests/host_checks.sh path/to/eltale-p1.exe rom.z64 [rt64|native]
# Needs eltale_host.dll and SDL3.dll beside the exe (and eltale_rt64.dll with its DLLs for rt64).
# Each renderer prints:
#   new game: P1_OS_OK as tests/p1_checks.sh's new-game line (the text is Japanese, as headless), and
#     HOST_AUDIO (frames queued and played: sound went to the device);
#   menu: MENU_TEST open/reset/quit (ELTALE_MENU_TEST: F1 menu at picture 200, Reset to the Controller
#     Pak manager, Quit), P1_RESET pak-manager, the Pak manager's P1_CPU_FRAME lines; native also
#     P1_NATIVE_CPU_FRAME (the same screens read back from the GPU: the same hashes) and PNGs in
#     out/host_checks/native (ELTALE_PRESENT_DUMP); rt64 "[rt64] overlay drawn" (the menu reached RT64);
#   quick save and load: P1_QUICKSAVE and P1_QUICKLOAD as tests/p1_checks.sh's.
set -u
exe=$1
rom=$2
cd "$(dirname "$0")/.."
data() {
    d=$(mktemp -d)
    printf '{"version": 2, "audio": {"volume": 0}, "game": {"language": "jp"}}\n' > "$d/settings.json"
    echo "$d"
}
run() { ELTALE_HIDDEN_WINDOW=1 "$exe" --window --rom "$rom" "$@" 2>&1; }
for r in ${3:-rt64 native}; do
    echo "$r new game:"
    d=$(data)
    ELTALE_DATA_DIR=$d run --renderer "$r" --fast --frames 700 --input tests/title-new-game.input |
        grep 'P1_OS_OK\|HOST_AUDIO\|overlay drawn'
    rm -rf "$d"
    echo "$r menu, reset into the Controller Pak manager, quit:"
    d=$(data)
    shots=out/host_checks/$r
    rm -rf "$shots"
    mkdir -p "$shots"
    ELTALE_PRESENT_DUMP=$shots ELTALE_DATA_DIR=$d ELTALE_MENU_TEST=200 run --renderer "$r" |
        grep 'MENU_TEST\|P1_RESET\|P1_CPU_FRAME\|P1_NATIVE_CPU_FRAME\|overlay drawn\|\[render\] wrote'
    rm -rf "$d"
    echo "$r quick save and load:"
    d=$(data)
    ELTALE_DATA_DIR=$d ELTALE_QUICK_SAVE_AT=1500 run --renderer "$r" --fast --frames 800 \
        --input tests/quicksave-walk.input | grep 'P1_QUICKSAVE'
    ELTALE_DATA_DIR=$d ELTALE_QUICK_LOAD_AT=100 run --renderer "$r" --fast --frames 200 \
        --input tests/intro-to-title.input | grep 'P1_QUICKLOAD'
    rm -rf "$d"
done
