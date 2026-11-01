#!/usr/bin/env python3
"""The pacing of pictures between ticks (R6, docs/renderer/NATIVE_DRAW.md "Pictures between ticks"), in real time in
a hidden window (ELTALE_HIDDEN_WINDOW=1: vsync off, nothing on the screen), after King's Field IV's tools/pace_test.py.

    python tools/pace_test.py --rom ROM [--exe EXE] [--hz 240,120,60] [--frames 1500] [--input FILE] [--sim]
                              [--saves DIR --save NAME --load-at VI]

Each rate is one run of the game (tests/interp-walk.input by default: New Game, then a walk that turns) with the
Graphics page's frame rate fixed at that rate (a settings.json of the run's own, volume 0) and ELTALE_PRESENT_LOG.
From the log, over the run's second half (play):

- pictures a second, and the intervals between them (mean, spread, worst): evenly paced at the rate;
- retraces a second: 60 is the game at its own speed (the pictures never slow it);
- the kinds of picture: between (b), the newest as it is (n), as is for a reason (f: no earlier recording, a gap, a
  camera cut; the window's F1 Graphics page counts the reasons);
- the game's P1_OS_OK line (frames and the last hash: the same game as without pictures between).

--sim: ELTALE_PRESENT_SIM=1, the pacing alone (nothing drawn or presented): what the schedule does by itself.
Run it while the computer is otherwise idle (it measures real time).
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(args, hz):
    d = Path(tempfile.mkdtemp(prefix="eltale_pace_"))
    try:
        if args.save:
            shutil.copytree(Path(args.saves) / args.save, d, dirs_exist_ok=True)
        settings = {"version": 2, "audio": {"volume": 0}, "game": {"language": "jp"},
                    "graphics": {"frame_rate": "fixed" if hz else "original", "frame_rate_target": hz or 240}}
        (d / "settings.json").write_text(json.dumps(settings))
        log = d / "present.log"
        env = dict(os.environ, ELTALE_HIDDEN_WINDOW="1", ELTALE_DATA_DIR=str(d), ELTALE_PRESENT_LOG=str(log))
        if args.sim:
            env["ELTALE_PRESENT_SIM"] = "1"
        if args.save:
            env["ELTALE_QUICK_LOAD_AT"] = str(args.load_at)
        cmd = [args.exe, "--window", "--renderer", "native", "--frames", str(args.frames), "--input",
               str(ROOT / args.input), "--rom", args.rom]
        r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        ok = next((l for l in r.stdout.splitlines() if l.startswith("P1_OS_OK")), "no P1_OS_OK (exit %d)" % r.returncode)
        stats = next((l for l in r.stdout.splitlines() if l.startswith("P1_INTERP ")), "")
        pics, vis = [], []
        for line in log.read_text().splitlines():
            f = line.split()
            if f[0] == "P":
                pics.append((float(f[1]), float(f[2]), f[3]))
            elif f[0] == "V":
                vis.append(float(f[1]))
        return pics, vis, ok, stats
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rom", required=True)
    ap.add_argument("--exe", default=str(ROOT / "out/p1-windows/eltale-p1.exe"))
    ap.add_argument("--hz", default="240,120,60", help="rates (0: Original, the game's timing)")
    ap.add_argument("--frames", type=int, default=1500)
    ap.add_argument("--input", default="tests/interp-walk.input")
    ap.add_argument("--saves", default="C:/code/eltale/out/bugsaves")
    ap.add_argument("--save")
    ap.add_argument("--load-at", type=int, default=100)
    ap.add_argument("--sim", action="store_true")
    args = ap.parse_args()
    for hz in [int(h) for h in args.hz.split(",")]:
        pics, vis, ok, stats = run(args, hz)
        if not pics or not vis:
            print("%3d Hz: no pictures (%s)" % (hz, ok))
            continue
        t0 = vis[len(vis) // 2]
        t1 = vis[-1]
        play = [p for p in pics if t0 <= p[0] < t1]
        span = t1 - t0
        gaps = [b[0] - a[0] for a, b in zip(play, play[1:])]
        kinds = {k: sum(1 for p in play if p[2] == k) for k in "bnf"}
        vps = sum(1 for v in vis if t0 <= v < t1) / span if span > 0 else 0
        print("%3d Hz%s: %.1f pictures a second, interval %.2f ms (spread %.2f, worst %.2f), %.2f retraces a second, "
              "between %d / newest %d / as is %d" % (
                  hz, " (sim)" if args.sim else "", len(play) / span if span > 0 else 0,
                  1000 * statistics.mean(gaps) if gaps else 0, 1000 * statistics.pstdev(gaps) if gaps else 0,
                  1000 * max(gaps) if gaps else 0, vps, kinds["b"], kinds["n"], kinds["f"]))
        print("        " + ok)
        if stats:
            print("        " + stats)


if __name__ == "__main__":
    main()
