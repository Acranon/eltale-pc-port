#!/usr/bin/env python3
"""Pipelines made ahead (R7, docs/renderer/NATIVE_DRAW.md "Modern options"): the hitch of pipelines made in play,
with and without the list, in hidden windows (ELTALE_HIDDEN_WINDOW=1), and the lists for the shipped one.

    python tools/warm_test.py --rom ROM [--exe EXE] [--msaa 4] [--collect DIR] [--cases ng,spells,menu,ending]

Each case is one run of the game (--renderer native, the Graphics page's defaults at that MSAA, volume 0) in a data
folder of its own; P1_PIPELINES at the end counts the pipelines made ahead and those a draw waited for (made in play,
the time they took: the hitches). The driver's own disk cache is turned off and pointed at an empty folder
(__GL_SHADER_DISK_CACHE=0, __GL_SHADER_DISK_CACHE_PATH: NVIDIA), so a pipeline not in our cache is compiled from
nothing, as on a first start.

- cold: a fresh data folder, nothing made ahead (ELTALE_WARM=0): every pipeline is made by the draw that needs it.
  The list the runs write is kept (in --collect DIR/m<msaa>, for tools/pipeline_list.py).
- warm: a fresh data folder holding only that list (no pipeline cache): the start makes them ahead on its threads.
- shipped: a fresh data folder and nothing else: what the shipped list (port/render/nd_pipeline_list.h) covers.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CASES = {
    "ng": ({}, ["--frames", "900", "--input", "tests/interp-walk.input"], None),
    "spells": ({"ELTALE_QUICK_LOAD_AT": "100"}, ["--frames", "1600", "--input", "tests/battle-spells.input"], "battle-stuck"),
    "menu": ({"ELTALE_QUICK_LOAD_AT": "100"}, ["--frames", "650", "--input", "tests/item-menu.input"], "battle-stuck"),
    "ending": ({"ELTALE_START_MODE": "5"}, ["--frames", "3000"], None),
}


def run(args, case, data, warm):
    env_extra, argv, save = CASES[case]
    if save:
        shutil.copytree(Path(args.saves) / save, data, dirs_exist_ok=True)
    settings = {"version": 2, "audio": {"volume": 0}, "game": {"language": "jp"}, "graphics": {"msaa": args.msaa}}
    (data / "settings.json").write_text(json.dumps(settings))
    env = dict(os.environ, ELTALE_HIDDEN_WINDOW="1", ELTALE_DATA_DIR=str(data), __GL_SHADER_DISK_CACHE="0",
               __GL_SHADER_DISK_CACHE_PATH=str(data / "glcache"),
               ELTALE_WARM="1" if warm else "0", **env_extra)
    cmd = [args.exe, "--window", "--renderer", "native", "--fast", *argv, "--rom", args.rom]
    r = subprocess.run(cmd, env=env, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    line = next((l for l in r.stdout.splitlines() if l.startswith("P1_PIPELINES")), "no P1_PIPELINES (exit %d)" % r.returncode)
    ok = next((l for l in r.stdout.splitlines() if l.startswith("P1_OS_OK")), "no P1_OS_OK")
    return line, ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rom", required=True)
    ap.add_argument("--exe", default=str(ROOT / "out/p1-windows/eltale-p1.exe"))
    ap.add_argument("--saves", default="C:/code/eltale/out/bugsaves")
    ap.add_argument("--msaa", type=int, default=4)
    ap.add_argument("--cases", default="ng,spells,menu,ending")
    ap.add_argument("--collect", help="keep the cold runs' lists in DIR/m<msaa>/<case>")
    ap.add_argument("--modes", default="cold,warm,shipped")
    args = ap.parse_args()
    modes = args.modes.split(",")
    for case in args.cases.split(","):
        tmp = Path(tempfile.mkdtemp(prefix="eltale_warm_"))
        try:
            lst = None
            for mode in modes:
                d = tmp / mode
                d.mkdir()
                if mode == "warm":
                    if not lst:
                        continue
                    shutil.copy(lst, d / "native_pipeline_list.txt")
                line, ok = run(args, case, d, mode != "cold")
                print("%-7s %-7s msaa %d: %s" % (case, mode, args.msaa, line))
                if mode == "cold":
                    l = d / "native_pipeline_list.txt"
                    lst = l if l.exists() else None
                    if lst and args.collect:
                        out = Path(args.collect) / ("m%d" % args.msaa) / case
                        out.mkdir(parents=True, exist_ok=True)
                        shutil.copy(lst, out / "native_pipeline_list.txt")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
