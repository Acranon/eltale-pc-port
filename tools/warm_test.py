#!/usr/bin/env python3
"""Pipelines made ahead (R7, docs/renderer/NATIVE_DRAW.md "Modern options"): the hitch of pipelines made in play,
with and without the list, in hidden windows (ELTALE_HIDDEN_WINDOW=1), and the lists for the shipped one.

    python tools/warm_test.py --rom ROM [--exe EXE] [--msaa 4[,8..]] [--looks everything[,classic..]]
                              [--collect DIR] [--cases ng,spells,menu,ending,tour] [--modes cold,warm,shipped]
    python tools/warm_test.py --rom ROM --check       # the shipped list covers everything (exit 1 if not)
    python tools/warm_test.py --rom ROM --regen [--fresh]   # make port/render/nd_pipeline_list.h again

Each case is one run of the game (--renderer native, a look's switches at that MSAA, volume 0) in a data folder of
its own; P1_PIPELINES at the end counts the pipelines made ahead and those a draw waited for (made in play, the
time they took: the hitches). The driver's own disk cache is turned off and pointed at an empty folder
(__GL_SHADER_DISK_CACHE=0, __GL_SHADER_DISK_CACHE_PATH: NVIDIA), so a pipeline not in our cache is compiled from
nothing, as on a first start.

- cold: a fresh data folder, nothing made ahead (ELTALE_WARM=0): every pipeline is made by the draw that needs it.
  The list the runs write is kept (in --collect DIR/<look>/m<msaa>/<case>, for tools/pipeline_list.py).
- warm: a fresh data folder holding only that list (no pipeline cache): the start makes them ahead on its threads.
- shipped: a fresh data folder and nothing else: what the shipped list (port/render/nd_pipeline_list.h) covers.

Cases: ng (New Game and a walk), spells (a battle with every element's spells), menu (the item menu and pages),
ending, and tour (ELTALE_MAP_TOUR: every sub-area of every map, a few minutes; map 0's stone rooms made 2-4
pipelines in play before it was in the list). Looks (LOOKS: the Graphics page's presets, plus "dither", Everything
on with the N64 dither, and "border", Everything on at 4:3 with the N64 border and fog): each run's settings.json
is the look's switches with "msaa" set to --msaa.

--check: the shipped mode for --cases (default all of them) x --msaa (default 1,2,4,8) x --looks (default every
look); WARM_CHECK_FAIL (exit 1) when any run made a pipeline in play or did not end with P1_OS_OK.
--regen: the cold mode for every case x MSAA 1,2,4,8 x every look into a scratch folder, then
tools/pipeline_list.py --keep on it (a run's list holds only the pipelines the shipped list lacks, so they are added
to it; when no run wrote one, every pipeline was in it already and it stays as it is), then tools/build_host.py and
--check: one command, run again after a shader, Granite or 2D change.
--regen --fresh: the shipped list emptied and the host built first, so the runs write every pipeline and the list
is made anew without stale lines (after a shader or Granite change). --jobs N runs N games at once (default 1; 2 at
most is kind to the machine). The looks make the same pipelines (the dither, fog and border are push constants,
not pipeline state; 2026-11-01): the MSAA count and what is drawn decide them.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CASES = {
    "ng": ({}, ["--frames", "900", "--input", "tests/interp-walk.input"], None),
    "spells": ({"ELTALE_QUICK_LOAD_AT": "100"}, ["--frames", "1600", "--input", "tests/battle-spells.input"], "battle-stuck"),
    "menu": ({"ELTALE_QUICK_LOAD_AT": "100"}, ["--frames", "650", "--input", "tests/item-menu.input"], "battle-stuck"),
    "ending": ({"ELTALE_START_MODE": "5"}, ["--frames", "3000"], None),
    "tour": ({"ELTALE_MAP_TOUR": "1"}, ["--frames", "300000"], None),
}

# the switches of each look (port/host/settings.cpp preset_graphics; "msaa" is set per run)
EVERYTHING = {"resolution": "window", "frame_rate": "fixed", "frame_rate_target": 240, "three_point": True,
              "scaling": "smooth_pixel_edges", "widescreen": "16:9", "border": False, "draw_distance": "far",
              "n64_fog": False, "n64_dither": False, "shader_warmup": True}
LOOKS = {
    "everything": EVERYTHING,
    "classic": dict(EVERYTHING, resolution="original", frame_rate="original", scaling="sharp", widescreen="off",
                    border=True, draw_distance="original", n64_fog=True, n64_dither=True),
    "cvisuals": dict(EVERYTHING, frame_rate="original", widescreen="off", n64_fog=True),
    "dither": dict(EVERYTHING, n64_dither=True),
    "border": dict(EVERYTHING, widescreen="off", border=True, n64_fog=True),
}
# environment variables that would change what a run draws (scrubbed: the look and the case say it all)
SCRUB = re.compile(r"^ELTALE_")


def run(args, case, look, msaa, data, warm):
    env_extra, argv, save = CASES[case]
    if save:
        shutil.copytree(Path(args.saves) / save, data, dirs_exist_ok=True)
    graphics = dict(LOOKS[look], msaa=msaa)
    settings = {"version": 2, "audio": {"volume": 0}, "game": {"language": "jp"}, "graphics": graphics}
    (data / "settings.json").write_text(json.dumps(settings))
    env = {k: v for k, v in os.environ.items() if not SCRUB.match(k)}
    env.update(ELTALE_HIDDEN_WINDOW="1", ELTALE_DATA_DIR=str(data), __GL_SHADER_DISK_CACHE="0",
               __GL_SHADER_DISK_CACHE_PATH=str(data / "glcache"), ELTALE_WARM="1" if warm else "0", **env_extra)
    cmd = [args.exe, "--window", "--renderer", "native", "--fast", *argv, "--rom", args.rom]
    r = subprocess.run(cmd, env=env, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    line = next((l for l in r.stdout.splitlines() if l.startswith("P1_PIPELINES")), "no P1_PIPELINES (exit %d)" % r.returncode)
    ok = next((l for l in r.stdout.splitlines() if l.startswith("P1_OS_OK")), None)
    return line, ok


def one(args, case, look, msaa, modes):
    """the modes of one case at one look and MSAA, in a scratch folder; the printed lines and the made-in-play
    counts of the shipped mode (None: no P1_PIPELINES or P1_OS_OK)"""
    out, shipped = [], []
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
            line, ok = run(args, case, look, msaa, d, mode != "cold")
            out.append("%-7s %-10s %-7s msaa %d: %s%s" % (case, look, mode, msaa, line, "" if ok else " (no P1_OS_OK)"))
            if mode == "shipped":
                m = re.search(r"made_in_play=(\d+)", line)
                shipped.append(int(m.group(1)) if m and ok else None)
            if mode == "cold":
                l = d / "native_pipeline_list.txt"
                lst = l if l.exists() else None
                if lst and args.collect:
                    dst = Path(args.collect) / look / ("m%d" % msaa) / case
                    dst.mkdir(parents=True, exist_ok=True)
                    shutil.copy(lst, dst / "native_pipeline_list.txt")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return out, shipped


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--exe", default=str(ROOT / "out/p1-windows/eltale-p1.exe"))
    ap.add_argument("--saves", default="C:/code/eltale/out/bugsaves")
    ap.add_argument("--msaa", help="MSAA counts, comma list (default 4; --check and --regen: 1,2,4,8)")
    ap.add_argument("--looks", help="looks, comma list (default everything; --check and --regen: all: %s)" % ",".join(LOOKS))
    ap.add_argument("--cases", help="cases, comma list (default ng,spells,menu,ending; --check and --regen: all)")
    ap.add_argument("--collect", help="keep the cold runs' lists in DIR/<look>/m<msaa>/<case>")
    ap.add_argument("--modes", default="cold,warm,shipped")
    ap.add_argument("--check", action="store_true", help="shipped only; fail when any pipeline is made in play")
    ap.add_argument("--regen", action="store_true", help="cold runs of everything, then tools/pipeline_list.py")
    ap.add_argument("--fresh", action="store_true", help="--regen: from an empty shipped list (needs two builds)")
    ap.add_argument("--jobs", type=int, default=1, help="games at once (default 1)")
    args = ap.parse_args()
    every = args.check or args.regen
    msaas = [int(m) for m in (args.msaa or ("1,2,4,8" if every else "4")).split(",")]
    looks = (args.looks or (",".join(LOOKS) if every else "everything")).split(",")
    cases = (args.cases or (",".join(CASES) if every else "ng,spells,menu,ending")).split(",")
    bad = [l for l in looks if l not in LOOKS] + [c for c in cases if c not in CASES]
    if bad:
        sys.exit("warm_test: unknown look or case: %s" % " ".join(bad))
    modes = args.modes.split(",")
    if args.check:
        return check(args, cases, looks, msaas)
    scratch = None
    if args.regen:
        modes = ["cold"]
        if not args.collect:
            scratch = tempfile.mkdtemp(prefix="eltale_warm_lists_")
            args.collect = scratch
        if args.fresh:
            header = ROOT / "port/render/nd_pipeline_list.h"
            header.write_text('#pragma once\n\nstatic const char *const nd_pipeline_list[] = {\n    "",\n};\n', newline="\n")
            build_host()
    work = [(c, l, m) for c in cases for l in looks for m in msaas]
    fails = 0
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for lines, shipped in pool.map(lambda w: one(args, w[0], w[1], w[2], modes), work):
            for line in lines:
                print(line, flush=True)
    if args.regen:
        # no list written: every pipeline made was in the shipped list already (new=0), nothing to add
        new = any(Path(args.collect).rglob("native_pipeline_list.txt"))
        try:
            r = subprocess.run([sys.executable, str(ROOT / "tools/pipeline_list.py"),
                                *([] if args.fresh else ["--keep"]), args.collect]) if new else None
        finally:
            if scratch:
                shutil.rmtree(scratch, ignore_errors=True)
        if r is None:
            print("warm_test: no run made a pipeline the shipped list lacks; the list stays as it is")
        elif r.returncode:
            return r.returncode
        build_host()
        args.check, args.regen = True, False
        return check(args, cases, looks, msaas)
    return 0


def build_host():
    """tools/build_host.py, and its DLL beside the exe"""
    subprocess.run([sys.executable, str(ROOT / "tools/build_host.py")], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    exe_dir = Path(ROOT / "out/p1-windows")
    if exe_dir.is_dir():
        shutil.copy(ROOT / "out/host/eltale_host.dll", exe_dir / "eltale_host.dll")


def check(args, cases, looks, msaas):
    fails = 0
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for lines, shipped in pool.map(lambda w: one(args, w[0], w[1], w[2], ["shipped"]),
                                       [(c, l, m) for c in cases for l in looks for m in msaas]):
            for line in lines:
                print(line, flush=True)
            fails += sum(1 for s in shipped if s is None or s > 0)
    print("WARM_CHECK_OK" if not fails else "WARM_CHECK_FAIL %d" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
