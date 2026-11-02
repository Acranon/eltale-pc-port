#!/usr/bin/env python3
"""The menus' pointer through the host (port/host/input.cpp input_pointer): window pixels to the game's picture, in a
hidden window (ELTALE_HIDDEN_WINDOW) of several shapes, widescreen modes and resolution scales.

    python tools/mouse_host_test.py --exe out/p1-windows/eltale-p1.exe --rom eltalemonsters.jp.z64

Each case plays ELTALE_INPUT_TEST events (`pointer X Y` in the window's coordinates, left clicks) on the title: a click on
"Press Start", the pointer on "Continue" (the cursor goes there) and a click (the save menu opens). The window
positions come from the picture's place in the window (its own shape, as large as fits, centred: pillarbox or
letterbox), so every case must report the same picture positions (P1_MOUSE_MENU pointer=, within half a picture
pixel) and the same picks (P1_MOUSE_STATE title=20, mode=2 after the last click). Prints MOUSE_HOST <case> OK|FAIL and
MOUSE_HOST_TEST OK when all pass.

The focus cases (4:3) send the window a focus lost with no focus gained after it, as a fullscreen switch can: with
the system saying the window has the focus (the system_focus row stands in for SDL's window flags, which a hidden
window never has) the clicks must still reach the game; with the events alone deciding they must not (the check that
the lost focus did arrive). The same for a mouse leave with no enter, with the system saying the mouse is over the
window: the pointer is not left outside.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (name, window WxH, ELTALE_WIDESCREEN, ELTALE_RES_SCALE or None, extra events, clicks the game must see)
CASES = [
    ("4:3", (1280, 960), "off", None, [], 2),
    ("4:3-pillarbox", (1600, 900), "off", "2", [], 2),
    ("16:9", (1600, 900), "16:9", None, [], 2),
    ("16:9-letterbox", (1280, 1200), "16:9", "3", [], 2),
    ("window-narrow", (1000, 800), "window", None, [], 2),
    ("window-wide", (2000, 700), "window", "1", [], 2),
    # a focus lost and never gained again (a fullscreen switch), the system saying focused: the mouse still works
    ("focus-lost-system-focused", (1280, 960), "off", None, ["40 system_focus 1", "45 focus_lost"], 2),
    # the same lost focus with the events alone deciding: no click gets through (the lost focus did arrive)
    ("focus-lost-events-only", (1280, 960), "off", None, ["45 focus_lost"], 0),
    # a mouse leave just after each press and never an enter, the system saying the mouse is over the window: the
    # clicks still count (the leave after the press: a press itself puts the pointer back inside)
    ("mouse-left-system-inside", (1280, 960), "off", None, ["40 system_mouse 1", "62 mouse_leave", "165 mouse_leave"], 2),
    # the same leaves with the events alone deciding: the pointer is outside, no click gets through
    ("mouse-left-events-only", (1280, 960), "off", None, ["62 mouse_leave", "165 mouse_leave"], 0),
]

# the title's targets, in the 320x240 screen's units (CENTRE): Press Start anywhere, Continue at 113..187 x 154..178
POINTS = [(160, 120), (150, 166)]


def picture_rect(win, mode):
    ww, wh = win
    if mode == "off":
        aspect = 4 / 3
    elif mode == "16:9":
        aspect = 16 / 9
    else:
        aspect = min(max(ww / wh, 4 / 3), 32 / 9)
    w1 = max(320, 2 * round(aspect * 240 * 0.5))
    fw, fh = ww, int(ww * 240 / w1)
    if fh > wh:
        fh, fw = wh, int(wh * w1 / 240)
    return (ww - fw) // 2, (wh - fh) // 2, fw, fh, w1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--rom", type=Path, required=True)
    args = ap.parse_args()
    failed = 0
    for name, win, mode, scale, extra, want in CASES:
        fx, fy, fw, fh, w1 = picture_rect(win, mode)
        side = (w1 - 320) / 2
        pts = [((x + side) , y) for x, y in POINTS]  # picture units
        wpts = [(fx + (u + 0.5) * fw / w1, fy + (v + 0.5) * fh / 240) for u, v in pts]
        events = ["5 key_down Return", "7 key_up Return",
                  f"60 pointer {wpts[0][0]:.1f} {wpts[0][1]:.1f}", "62 mouse_down left", "63 mouse_up left",
                  f"150 pointer {wpts[1][0]:.1f} {wpts[1][1]:.1f}", "165 mouse_down left", "166 mouse_up left"]
        events = sorted(events + extra, key=lambda e: int(e.split()[0]))  # (stable: a tick's rows keep their order)
        d = Path(tempfile.mkdtemp(prefix="mouse_host_"))
        try:
            (d / "settings.json").write_text('{"version": 2, "audio": {"volume": 0}, "game": {"language": "jp"}}\n')
            (d / "in.events").write_text("\n".join(events) + "\n")
            env = {k: v for k, v in os.environ.items() if not k.startswith("ELTALE_")}
            env.update(ELTALE_HIDDEN_WINDOW="1", ELTALE_DATA_DIR=str(d), ELTALE_INPUT_TEST=str(d / "in.events"),
                       ELTALE_MOUSE_LOG="1", ELTALE_WINDOW_SIZE=f"{win[0]}x{win[1]}", ELTALE_WIDESCREEN=mode)
            if scale:
                env["ELTALE_RES_SCALE"] = scale
            p = subprocess.run([str(args.exe), "--window", "--fast", "--frames", "260", "--rom", str(args.rom)], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=d)
            out = p.stdout.decode("utf-8", "replace")
        finally:
            shutil.rmtree(d, ignore_errors=True)
        problems = []
        clicks = [l for l in out.splitlines() if l.startswith("P1_MOUSE_MENU")]
        states = [l for l in out.splitlines() if l.startswith("P1_MOUSE_STATE")]
        if len(clicks) != want:
            problems.append(f"{len(clicks)} clicks seen, {want} wanted")
        for c, (u, v) in zip(clicks, pts):
            m = re.search(r"pointer=([\d.]+),([\d.]+) width=(\d+)", c)
            if not m or abs(float(m.group(1)) - (u + 0.5)) > 0.5 or abs(float(m.group(2)) - (v + 0.5)) > 0.5 or int(m.group(3)) != w1:
                problems.append(f"at {u + 0.5},{v + 0.5} width {w1}: {c}")
        if want and (len(states) < 2 or "title=20" not in states[1] or "mode=" not in states[1]):
            problems.append("Continue not picked: " + (states[1] if len(states) > 1 else "no state"))
        ok = re.search(r"P1_OS_OK[^\n]*", out)
        print(f"MOUSE_HOST {name} window={win[0]}x{win[1]} widescreen={mode} scale={scale or '-'} picture={fx},{fy},{fw}x{fh} "
              f"width={w1} {'OK' if not problems and ok else 'FAIL'}")
        for c in clicks:
            print("   ", c)
        for pr in problems + ([] if ok else ["no P1_OS_OK"]):
            print("    !", pr)
        failed += bool(problems) or not ok
    print("MOUSE_HOST_TEST " + ("OK" if not failed else f"FAIL ({failed})"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
