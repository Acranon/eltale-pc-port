#!/usr/bin/env python3
"""The native renderer's lifetime tests (docs/renderer/NATIVE_DRAW.md "Resets and full buffers").

    python tools/lifetime_test.py --rom ROM [--exe EXE] [--only reset,full] [--keep] [--work DIR]

reset: one run in a hidden window (ELTALE_HIDDEN_WINDOW: nothing shows; sound at volume 0; pictures between at a
  fixed 120 a second) that goes into play (tests/title-new-game.input), opens the F1 menu and chooses Reset
  (ELTALE_MENU_TEST=N:reset): the game boots again and the input script starts again, so it goes back into play the
  same way. The renderer numbers the ticks after the reset from 10001 (frame_10001.png ...). Checks:
  - P1_ND_LIFETIME: one reset, no stale item; ticks after the reset drawn natively (P1_NATIVE_FRAME items > 0);
  - the dumps (the ticks with ELTALE_DUMP_BETWEEN=4 pictures between, ELTALE_INTERP_ENDS): every P1_INTERP_ENDS 0/0,
    tools/interp_ends.py and tools/flash_find.py clean, before and after the reset;
  - each tick after the reset, and its pictures between, is the same picture as the same tick before it (the game
    replays the same inputs: nothing of before the reset is left in the renderer);
  - the flash recorder's 64 pictures shown around the reset (written 24 pictures after it): tools/flash_find.py
    --neighbours flags none (no picture of before the reset, or a broken one, among those after it).
full: headless runs with the buffers shrunk (ELTALE_ND_SMALL_BUFFERS=512: 512 KiB each, so they fill in play and
  the renderer keeps the recordings' resources, many times, mid-tick) against the same runs at full size: the frame
  dumps and the pictures between must be the same pictures, P1_OS_OK the same, flushes > 0, no stale item, ends 0/0,
  interp_ends and flash_find clean.

Needs the exe with eltale_host.dll and SDL3.dll beside it, Pillow and numpy. The dumps are deleted afterwards
(--keep keeps them).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent

# name, environment, arguments, the ticks dumped
FULL_CASES = [
    ("ng", {}, ["--frames", "700", "--input", "tests/title-new-game.input"], "200-700"),
    ("walk", {}, ["--frames", "900", "--input", "tests/interp-walk.input"], "600-900"),
    ("tour", {"ELTALE_MAP_TOUR": "1"}, ["--frames", "300000"], "600+1000/8"),
]
SMALL = "512"


def run(cmd, env):
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    return r.returncode, r.stdout


def line(out, prefix):
    return next((l for l in out.splitlines() if l.startswith(prefix)), "")


def lifetime(out):
    m = re.search(r"P1_ND_LIFETIME resets=(\d+) flushes=(\d+) .* stale=(\d+)", out)
    return tuple(int(x) for x in m.groups()) if m else None


def tools_check(d, neighbours=False):
    """interp_ends (dumps only) and flash_find: (ok, text)"""
    texts, ok = [], True
    if not neighbours:
        pe = subprocess.run([sys.executable, str(ROOT / "tools/interp_ends.py"), str(d)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
        ok &= pe.returncode == 0
        texts.append(pe.stdout.strip().splitlines()[-1] if pe.stdout.strip() else "interp_ends: ?")
    ff = subprocess.run([sys.executable, str(ROOT / "tools/flash_find.py"), str(d)] + (["--neighbours"] if neighbours else []),
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    flagged = re.search(r"(\d+) flagged", ff.stdout)
    ok &= bool(flagged) and int(flagged.group(1)) == 0
    texts.append("flash_find%s: %s" % (" --neighbours" if neighbours else "",
                                      ff.stdout.strip().splitlines()[0] if ff.stdout.strip() else "?"))
    return ok, "; ".join(texts)


def ends_ok(out):
    ends = re.findall(r"P1_INTERP_ENDS frame=(\d+) differ_at_0=(\d+) differ_at_1=(\d+)", out)
    bad = [e for e in ends if e[1] != "0" or e[2] != "0"]
    return bool(ends) and not bad, "%d pairs, ends %s" % (len(ends), "0/0" if not bad else "%d differ" % len(bad))


def same_picture(a, b):
    if a.read_bytes() == b.read_bytes():
        return True
    return np.array_equal(np.asarray(Image.open(a).convert("RGB")), np.asarray(Image.open(b).convert("RGB")))


def pngs(d):
    return {p.name: p for p in d.glob("*.png") if p.name != "flashes.png"}


def case_reset(args):
    d = Path(args.work) / "reset"
    shutil.rmtree(d, ignore_errors=True)
    (d / "data").mkdir(parents=True)
    (d / "dump").mkdir()
    settings = {"version": 2, "audio": {"volume": 0}, "game": {"language": "jp"},
                "graphics": {"preset": "custom", "frame_rate": "fixed", "frame_rate_target": 120, "widescreen": "off",
                             "resolution": "original", "msaa": 1, "vsync": False}}
    (d / "data/settings.json").write_text(json.dumps(settings))
    # the menu at the 900th retrace (in play, ~tick 440), Reset 30 later, Quit 900 after it (back in play); the flash
    # recorder on from the start (ELTALE_FLASH: it is off until a first F8 otherwise), so the F8 after the reset writes it
    env = dict(os.environ, ELTALE_HIDDEN_WINDOW="1", ELTALE_DATA_DIR=str(d / "data"), ELTALE_MENU_TEST="900:reset:900",
               ELTALE_FLASH="1",
               ELTALE_DUMP_FRAMES="250-460,10001-10060,10250-10460", ELTALE_DUMP_BETWEEN="4", ELTALE_INTERP_ENDS="1")
    rc, out = run([args.exe, "--window", "--rom", args.rom, "--input",
                   str(ROOT / "tests/title-new-game.input"), "--dump-frames", str(d / "dump")], env)
    (d / "out.txt").write_text(out)
    good, notes = rc == 0, []
    life = lifetime(out)
    good &= life is not None and life[0] == 1 and life[2] == 0
    notes.append(line(out, "P1_ND_LIFETIME") or "no P1_ND_LIFETIME")
    good &= "MENU_TEST reset game" in out and "P1_RESET" in out
    after = [(int(f), int(i)) for f, i in re.findall(r"P1_NATIVE_FRAME frame=(\d+) items=(\d+)", out) if int(f) > 10000]
    drawn = sum(1 for f, i in after if i > 0)
    good &= drawn > 0
    notes.append("ticks dumped after the reset %d (%d with items)" % (len(after), drawn))
    ok, t = ends_ok(out)
    good &= ok
    notes.append(t)
    ok, t = tools_check(d / "dump")
    good &= ok
    notes.append(t)
    # after the reset = before it
    files = pngs(d / "dump")
    pairs = same = 0
    differ = []
    for name, p in sorted(files.items()):
        m = re.match(r"frame_(\d+)((?:_\d+)?)\.png$", name)
        if not m or int(m.group(1)) <= 10000:
            continue
        twin = files.get("frame_%d%s.png" % (int(m.group(1)) - 10000, m.group(2)))
        if not twin:
            continue
        pairs += 1
        if same_picture(p, twin):
            same += 1
        else:
            differ.append(name)
    good &= pairs > 0 and not differ
    notes.append("after = before: %d of %d pictures%s" % (same, pairs, (" (differ: %s)" % ", ".join(differ[:6])) if differ else ""))
    flash = sorted((d / "data").glob("flash_*"))
    reset_vi = re.search(r"P1_ND_RESET vi=(\d+)", out)
    if flash and reset_vi:
        ok, t = tools_check(flash[-1], neighbours=True)
        good &= ok
        notes.append(t)
        # no picture after the reset is the scene before it (the pre-reset recordings drawn on, as before the fix:
        # the play scene went on for ~10 pictures, a mean difference under 1 level from the last before the reset;
        # what follows a reset, black then the logo, is ~50 levels off it)
        shown = [l.split() for l in (flash[-1] / "pictures.txt").read_text().splitlines() if l and not l.startswith("#")]
        # (the pictures of the reset's own retrace are neither: one between the last two ticks before it can still be
        # shown in it, as the game's thread takes the reset during that retrace's tick)
        before = [f for f in shown if int(f[1]) + float(f[2]) < int(reset_vi.group(1))]
        after = [f for f in shown if int(f[1]) > int(reset_vi.group(1))]
        stale = []
        if before:
            last = np.asarray(Image.open(flash[-1] / before[-1][0]).convert("RGB"), dtype=np.int16)
            for f in after:
                p = np.asarray(Image.open(flash[-1] / f[0]).convert("RGB"), dtype=np.int16)
                if p.shape == last.shape and np.abs(p - last).mean() < 4:
                    stale.append(f[0])
        good &= bool(before) and bool(after) and not stale
        notes.append("the recorder's pictures: %d before the reset, %d after it, %d of those the scene before it" %
                     (len(before), len(after), len(stale)))
    else:
        good = False
        notes.append("no flash recorder folder")
    notes.append(line(out, "P1_INTERP ") or "no P1_INTERP")
    print("reset   %s" % ("OK" if good else "FAIL"))
    for n in notes:
        print("        " + n)
    if not args.keep:
        shutil.rmtree(d / "dump", ignore_errors=True)
        for f in flash:
            shutil.rmtree(f, ignore_errors=True)
    return good


def case_full(args, name, env_extra, argv, frames):
    base = Path(args.work) / ("full_" + name)
    argv = [str(ROOT / a) if a.startswith("tests/") else a for a in argv]
    outs = {}
    for k in ("normal", "small"):
        d = base / k
        shutil.rmtree(d, ignore_errors=True)
        (d / "data").mkdir(parents=True)
        env = dict(os.environ, ELTALE_DATA_DIR=str(d / "data"), ELTALE_DUMP_FRAMES=frames, ELTALE_DUMP_BETWEEN="4",
                   ELTALE_INTERP_ENDS="1", **env_extra)
        env.pop("ELTALE_ND_SMALL_BUFFERS", None)
        if k == "small":
            env["ELTALE_ND_SMALL_BUFFERS"] = SMALL
        rc, out = run([args.exe, "--headless", "--fast", *argv, "--dump-frames", str(d), "--rom", args.rom], env)
        (d / "out.txt").write_text(out)
        outs[k] = (rc, out)
    good, notes = all(rc == 0 for rc, _ in outs.values()), []
    small = outs["small"][1]
    life = lifetime(small)
    good &= life is not None and life[1] > 0 and life[2] == 0
    notes.append(line(small, "P1_ND_LIFETIME") or "no P1_ND_LIFETIME")
    oks = [line(o, "P1_OS_OK") for _, o in outs.values()]
    good &= bool(oks[0]) and oks[0] == oks[1]
    notes.append(oks[1] + ("" if oks[0] == oks[1] else " (normal: %s)" % oks[0]))
    ok, t = ends_ok(small)
    good &= ok
    notes.append(t)
    ok, t = tools_check(base / "small")
    good &= ok
    notes.append(t)
    a, b = pngs(base / "normal"), pngs(base / "small")
    differ = [n for n in sorted(a) if n not in b or not same_picture(a[n], b[n])]
    missing = [n for n in b if n not in a]
    good &= bool(a) and not differ and not missing
    notes.append("small = full size: %d of %d pictures%s" % (len(a) - len(differ), len(a),
                                                               (" (differ: %s)" % ", ".join(differ[:6])) if differ else ""))
    print("full %-3s %s" % (name, "OK" if good else "FAIL"))
    for n in notes:
        print("        " + n)
    if not args.keep:
        for k in ("normal", "small"):
            for p in (base / k).glob("*.png"):
                if p.name != "flashes.png":
                    p.unlink()
            shutil.rmtree(base / k / "data", ignore_errors=True)
    return good


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rom", required=True)
    ap.add_argument("--exe", default=str(ROOT / "out/p1-windows/eltale-p1.exe"))
    ap.add_argument("--only", help="reset, full, or full cases by name (ng, walk, tour), comma-separated")
    ap.add_argument("--work", default=str(ROOT / "out/lifetime_test"))
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    args.exe = str(Path(args.exe).resolve())
    only =set(args.only.split(",")) if args.only else None
    failed = 0
    if not only or "reset" in only:
        failed += not case_reset(args)
    for name, env_extra, argv, frames in FULL_CASES:
        if not only or "full" in only or name in only:
            failed += not case_full(args, name, env_extra, argv, frames)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
