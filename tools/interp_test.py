#!/usr/bin/env python3
"""The tests of the pictures between ticks (R6, docs/renderer/NATIVE_DRAW.md "Pictures between ticks"), headless.

    python tools/interp_test.py --rom ROM [--exe EXE] [--saves DIR] [--between N] [--only a,b] [--keep] [--work DIR]

Each case is one headless run with the native renderer (--dump-frames) that writes, for the ticks it names, the
tick's own picture and N - 1 pictures between it and the tick before (ELTALE_DUMP_BETWEEN=N, 4 by default: what 120
pictures a second show of a 30-tick game), with ELTALE_INTERP_ENDS=1. Then:

- ends: every P1_INTERP_ENDS line must say 0 and 0 (the interpolator's matrices and camera at t = 0 are the last
  tick's and at t = 1 this tick's, bit for bit), and tools/interp_ends.py must find the pictures at t = 0 equal to
  the ticks' own;
- flashes: tools/flash_find.py must flag no picture between;
- the run's game must be the game: its P1_OS_OK line is printed (the frame count and last hash; with the frame
  image off they are the game's own).

The dumps are deleted afterwards (--keep keeps them); a case's flashes.png sheet stays in its folder when it
flagged anything. Needs the exe with eltale_host.dll beside it, Pillow and numpy.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# name, environment, arguments, the ticks dumped (ELTALE_DUMP_FRAMES), a save to copy (or None)
CASES = [
    ("ng", {}, ["--frames", "700", "--input", "tests/title-new-game.input"], "200-700", None),
    ("walk", {}, ["--frames", "900", "--input", "tests/interp-walk.input"], "600-900", None),
    # the mouse camera (port/p0/mouse_camera.c): turns, zooms, a reset and fast turns round the player
    ("mouse", {}, ["--frames", "950", "--input", "tests/mouse-camera.input"], "620-950", None),
    ("mouse-battle", {"ELTALE_QUICK_LOAD_AT": "100"}, ["--frames", "560", "--input", "tests/mouse-battle.input"],
     "110-560", "battle-stuck"),
    ("flames", {}, ["--frames", "700", "--input", "tests/title-new-game.input"], "225-330", None),
    ("spells", {"ELTALE_QUICK_LOAD_AT": "100", "ELTALE_BATTLE_LOG": "2"},
     ["--frames", "1600", "--input", "tests/battle-spells.input"], "60+100/40", "battle-stuck"),
    ("tour", {"ELTALE_MAP_TOUR": "1"}, ["--frames", "300000"], "600+1000/8", None),
    ("ending", {"ELTALE_START_MODE": "5"}, ["--frames", "10000"], "300+500/12", None),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rom", required=True)
    ap.add_argument("--exe", default=str(ROOT / "out/p1-windows/eltale-p1.exe"))
    ap.add_argument("--saves", default="C:/code/eltale/out/bugsaves")
    ap.add_argument("--between", type=int, default=4)
    ap.add_argument("--only", help="the cases to run (names, comma-separated)")
    ap.add_argument("--work", default=str(ROOT / "out/interp_test"))
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    only = set(args.only.split(",")) if args.only else None
    failed = 0
    for name, env_extra, argv, frames, save in CASES:
        if only and name not in only:
            continue
        d = Path(args.work) / name
        shutil.rmtree(d, ignore_errors=True)
        (d / "data").mkdir(parents=True)
        if save:
            shutil.copytree(Path(args.saves) / save, d / "data", dirs_exist_ok=True)
        env = dict(os.environ, ELTALE_DATA_DIR=str(d / "data"), ELTALE_DUMP_FRAMES=frames,
                   ELTALE_DUMP_BETWEEN=str(args.between), ELTALE_INTERP_ENDS="1", **env_extra)
        argv = [str(ROOT / a) if a.startswith("tests/") else a for a in argv]
        cmd = [args.exe, "--headless", "--fast", *argv, "--dump-frames", str(d), "--rom", args.rom]
        r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        out = r.stdout
        ends = re.findall(r"P1_INTERP_ENDS frame=(\d+) differ_at_0=(\d+) differ_at_1=(\d+)", out)
        bad_ends = [e for e in ends if e[1] != "0" or e[2] != "0"]
        ok_line = next((l for l in out.splitlines() if l.startswith("P1_OS_OK")), "no P1_OS_OK")
        audio = next((l for l in out.splitlines() if l.startswith("P1_AUDIO")), "")
        ok_line += " " + (re.search(r"hash=\w+", audio).group(0).replace("hash", "audio") if audio else "")
        stats = next((l for l in out.splitlines() if l.startswith("P1_INTERP ")), "")
        snaps = len(re.findall(r"P1_INTERP_SNAP", out))
        pe = subprocess.run([sys.executable, str(ROOT / "tools/interp_ends.py"), str(d)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
        ff = subprocess.run([sys.executable, str(ROOT / "tools/flash_find.py"), str(d)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
        flagged = re.search(r"(\d+) flagged", ff.stdout)
        flagged = int(flagged.group(1)) if flagged else -1
        good = r.returncode == 0 and ends and not bad_ends and pe.returncode == 0 and flagged == 0
        failed += not good
        print("%-7s %s: %d pairs, ends %s, %s, flash_find: %s" % (
            name, "OK" if good else "FAIL", len(ends), "0/0" if not bad_ends else "%d differ" % len(bad_ends),
            pe.stdout.strip().splitlines()[-1] if pe.stdout.strip() else "?",
            ff.stdout.strip().splitlines()[0] if ff.stdout.strip() else "?"))
        print("        %s; snapped pairs %d" % (ok_line, snaps))
        if stats:
            print("        " + stats)
        if flagged:
            print("\n".join("        " + l for l in ff.stdout.strip().splitlines()[1:12]))
        if not args.keep:
            for p in d.glob("*.png"):
                if p.name != "flashes.png":
                    p.unlink()
            shutil.rmtree(d / "data", ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
