#!/bin/sh
# The window checks: the host (eltale_host.dll) and its renderer in a hidden window
# (ELTALE_HIDDEN_WINDOW), sound at volume 0, a temporary data folder each run. Run them only when a
# hidden window is fine (it shows nothing, but it uses the GPU and opens the sound device).
# usage: tests/host_checks.sh path/to/eltale-p1.exe rom.z64
# Needs eltale_host.dll and SDL3.dll beside the exe. It prints:
#   new game: P1_OS_OK as tests/p1_checks.sh's new-game line but for last_hash (the frame lists are
#     emptied: every kind is drawn natively), and HOST_AUDIO (frames queued and played: sound went to
#     the device);
#   menu: MENU_TEST open/reset/quit (ELTALE_MENU_TEST: F1 menu at picture 200, Reset to the Controller
#     Pak manager, Quit), P1_RESET pak-manager, the Pak manager's P1_CPU_FRAME lines and
#     P1_NATIVE_CPU_FRAME (the same screens read back from the GPU: the same hashes), and PNGs in
#     out/host_checks/native (ELTALE_PRESENT_DUMP);
#   quick save and load: P1_QUICKSAVE and P1_QUICKLOAD as tests/p1_checks.sh's, the F2 notice
#     ("Quick saved...", an ImGui line top left, over the HP meters) read back from the window as
#     out/host_checks/native-notice/native_notice.png, and NOTICE_CHECK OK when the overlay is there
#     (tools/notice_check.py: light text on the darkened box in the top-left corner);
#   Alt+Enter: the fullscreen key in the intro (ELTALE_INPUT_TEST, tests/alt-enter.events) is not
#     Start: ALT_ENTER OK when the run ends as the same run with no keys does;
#   tools/lifetime_test.py (a plain Reset in play in a hidden window, and full buffers headless;
#     NATIVE_DRAW.md "Resets and full buffers"): a line per case, OK or FAIL.
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
r=native
echo "$r new game:"
d=$(data)
ELTALE_DATA_DIR=$d run --fast --frames 700 --input tests/title-new-game.input | grep 'P1_OS_OK\|HOST_AUDIO'
rm -rf "$d"
echo "$r menu, reset into the Controller Pak manager, quit:"
d=$(data)
shots=out/host_checks/$r
rm -rf "$shots"
mkdir -p "$shots"
ELTALE_PRESENT_DUMP=$shots ELTALE_DATA_DIR=$d ELTALE_MENU_TEST=200 run |
    grep 'MENU_TEST\|P1_RESET\|P1_CPU_FRAME\|P1_NATIVE_CPU_FRAME\|\[render\] wrote'
rm -rf "$d"
echo "$r quick save and load:"
d=$(data)
notice=out/host_checks/$r-notice
rm -rf "$notice"
mkdir -p "$notice"
ELTALE_PRESENT_DUMP=$notice ELTALE_DATA_DIR=$d ELTALE_QUICK_SAVE_AT=1500 run --fast --frames 800 \
    --input tests/quicksave-walk.input | grep 'P1_QUICKSAVE'
python tools/notice_check.py "$notice/native_notice.png"
ELTALE_DATA_DIR=$d ELTALE_QUICK_LOAD_AT=100 run --fast --frames 200 \
    --input tests/intro-to-title.input | grep 'P1_QUICKLOAD'
rm -rf "$d"
echo "$r Alt+Enter in the intro:"
d=$(data)
plain=$(ELTALE_DATA_DIR=$d run --fast --frames 400 | grep 'P1_OS_OK')
alt=$(ELTALE_DATA_DIR=$d ELTALE_INPUT_TEST=tests/alt-enter.events run --fast --frames 400 | grep 'P1_OS_OK')
enter=$(ELTALE_DATA_DIR=$d ELTALE_INPUT_TEST=tests/enter.events run --fast --frames 400 | grep 'P1_OS_OK')
if [ -n "$plain" ] && [ "$alt" = "$plain" ] && [ "$enter" != "$plain" ]; then
    echo "ALT_ENTER OK (as no keys; Enter alone differs)"
else
    echo "ALT_ENTER FAIL"
    echo "  none:      $plain"
    echo "  Alt+Enter: $alt"
    echo "  Enter:     $enter"
fi
rm -rf "$d"
echo "$r reset and full buffers:"
python tools/lifetime_test.py --exe "$exe" --rom "$rom" | grep -v '^        '
