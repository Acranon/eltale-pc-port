#!/usr/bin/env python3
"""The area map's checks (docs/AUTOMAP.md): pictures of the map in a hidden window, hash-checked.

    python tools/automap_test.py --exe out/p1-windows/eltale-p1.exe --rom ROM [--saves DIR] [--update] [--out DIR]

Runs, each with its own data folder (sound off, map background 100%: the full map covers the game's picture):
  keys-4x3, keys-16x9  tests/automap.events (ELTALE_INPUT_TEST) after the quick save compass-spirit, with the marks and
                       the cheats on and the mouse put on a person (ELTALE_AUTOMAP_HOVER: their name shown): M opens the
                       full map, the wheel zooms, a drag moves it, a right click fits it, W walks with it open, Esc
                       closes, ] ] [ zoom the minimap (P1_AUTOMAP_ZOOM: 800 -> 640 -> 512 -> 640 units);
                       ELTALE_AUTOMAP_DUMP writes a picture per view (automap_full_N.png) and the minimap once
                       (automap_minimap.png: not hashed, the game shows through it). A 1280x960 4:3 window and a
                       1280x720 16:9 one.
  none                 tests/automap-none.events: the same walk without the map and its keys. The player's positions
                       (ELTALE_AUTOMAP_TRACE) must be those of keys-4x3: the map changes nothing in the game.
  tour                 ELTALE_MAP_TOUR with ELTALE_AUTOMAP_SHOTS (cheats on) for its first areas (the throne room, the
                       first town and its sub-areas, the first fields): the truth view, the map's lines and marks over
                       it and the full map (am_MM_SS_map.png, hashed for maps 0-3 and 13).
  indoor-castle        New Game (tests/title-new-game.input): the minimap in the throne room, indoors, the room fitted
                       (P1_AUTOMAP_MINIMAP indoor=1 fitted=1; the minimap's square of automap_minimap.png hashed).
  indoor-inn           the tour saves at the map-18 inn (tests/tour-save.input, as tests/p1_checks.sh does), then the
                       title's Continue loads it (tests/automap-continue.input): the minimap in the inn (hashed).
The hashes (of the pixels) are compared with tests/automap_hashes.txt; --update writes them. Prints AUTOMAP_TEST lines
and AUTOMAP_TEST_OK or AUTOMAP_TEST_FAIL at the end. The DLLs used are those beside the exe.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
HASHES = ROOT / "tests/automap_hashes.txt"


def pixels_hash(path: Path) -> str:
    im = Image.open(path).convert("RGBA")
    return hashlib.sha1(im.tobytes()).hexdigest()[:16]


def run(exe: Path, rom: str, out: Path, env_extra: dict, args: list, settings: dict, save: Path | None) -> str:
    data = out / "data"
    shots = out / "shots"
    shutil.rmtree(out, ignore_errors=True)
    shots.mkdir(parents=True)
    data.mkdir(parents=True)
    if save:
        for p in save.iterdir():
            if p.is_dir():
                shutil.copytree(p, data / p.name)
            else:
                shutil.copy(p, data / p.name)
    (data / "settings.json").write_text(json.dumps(settings), encoding="utf-8")
    env = dict(os.environ, ELTALE_HIDDEN_WINDOW="1", ELTALE_DATA_DIR=str(data), ELTALE_PRESENT_DUMP=str(shots), **env_extra)
    r = subprocess.run([str(exe), "--window", "--fast", *args, "--rom", rom], env=env, capture_output=True, text=True,
                       errors="replace", cwd=exe.parent)
    (out / "stdout.txt").write_text(r.stdout, encoding="utf-8")
    (out / "stderr.txt").write_text(r.stderr, encoding="utf-8")
    return r.stdout + r.stderr


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--saves", default="C:/code/eltale/out/bugsaves", type=Path)
    ap.add_argument("--update", action="store_true", help="write tests/automap_hashes.txt from this run")
    ap.add_argument("--out", type=Path, help="keep the pictures here (default: a temporary folder)")
    a = ap.parse_args()
    exe = a.exe.resolve()
    base = a.out or Path(tempfile.mkdtemp(prefix="automap_test_"))
    def settings(wide: str) -> dict:
        return {"version": 2, "audio": {"volume": 0}, "game": {"language": "en"},
                "graphics": {"preset": "custom", "widescreen": wide},
                "map": {"background": 100, "minimap": True, "cheat_chests": True, "cheat_spirits": True,
                        "cheat_bosses": True}}
    save = a.saves / "compass-spirit"
    got: dict[str, str] = {}
    traces: dict[str, list[str]] = {}
    fail = False
    keys_env = {"ELTALE_AUTOMAP_DUMP": "1", "ELTALE_AUTOMAP_TRACE": "1", "ELTALE_QUICK_LOAD_AT": "100",
                "ELTALE_AUTOMAP_HOVER": "person"}
    for name, size, wide, events in (("keys-4x3", "1280x960", "off", "automap.events"),
                                     ("keys-16x9", "1280x720", "16:9", "automap.events"),
                                     ("none", "1280x960", "off", "automap-none.events")):
        out = base / name
        text = run(exe, a.rom, out, dict(keys_env, ELTALE_WINDOW_SIZE=size, ELTALE_INPUT_TEST=str(ROOT / "tests" / events)),
                   ["--frames", "700"], settings(wide), save)
        traces[name] = [re.sub(r"frame=\d+ ", "", l) for l in text.splitlines() if l.startswith("P1_AUTOMAP_POS")]
        views = [l for l in text.splitlines() if l.startswith("P1_AUTOMAP_VIEW")]
        for l in views:
            print(f"AUTOMAP_TEST {name} {l}")
        for p in sorted((out / "shots").glob("automap_full_*.png")):
            got[f"{name}/{p.name}"] = pixels_hash(p)
        if name != "none" and not views:
            print(f"AUTOMAP_TEST FAIL {name}: the full map never opened")
            fail = True
        if name != "none" and not (out / "shots" / "automap_minimap.png").exists():
            print(f"AUTOMAP_TEST FAIL {name}: no minimap picture")
            fail = True
        zooms = [l.split()[-1] for l in text.splitlines() if l.startswith("P1_AUTOMAP_ZOOM")]
        if name != "none":
            print(f"AUTOMAP_TEST {name} minimap zoom: {' '.join(zooms)}")
            if zooms != ["reach=640", "reach=512", "reach=640"]:
                print(f"AUTOMAP_TEST FAIL {name}: the minimap zoom keys")
                fail = True
    same = traces["keys-4x3"] == traces["none"] and len(traces["none"]) > 100
    print(f"AUTOMAP_TEST game unchanged by the map: {'OK' if same else 'FAIL'} ({len(traces['none'])} ticks traced)")
    fail |= not same
    out = base / "tour"
    run(exe, a.rom, out, {"ELTALE_AUTOMAP_SHOTS": "1", "ELTALE_MAP_TOUR": "1", "ELTALE_WINDOW_SIZE": "1280x960"},
        ["--frames", "2600"], settings("off"), None)
    for p in sorted((out / "shots").glob("am_*_map.png")):
        if int(p.name[3:5]) < 4 or p.name.startswith("am_13_"):  # (the run's last area depends on where it is cut)
            got[f"tour/{p.name}"] = pixels_hash(p)
    for need in ("am_13_17_map.png", "am_00_00_map.png", "am_02_00_map.png"):
        if not (out / "shots" / need).exists():
            print(f"AUTOMAP_TEST FAIL tour: no {need}")
            fail = True

    # the minimap indoors: a castle room (New Game's throne room) and a house (the map-18 inn)
    def minimap(name: str, out: Path, text: str) -> None:
        nonlocal fail
        lines = [l for l in text.splitlines() if l.startswith("P1_AUTOMAP_MINIMAP")]
        pic = out / "shots" / "automap_minimap.png"
        if not lines or not pic.exists():
            print(f"AUTOMAP_TEST FAIL {name}: no minimap picture")
            fail = True
            return
        print(f"AUTOMAP_TEST {name} {lines[0]}")
        if "indoor=1 fitted=1" not in lines[0]:
            print(f"AUTOMAP_TEST FAIL {name}: not the indoor room view")
            fail = True
        r = [int(v) for v in re.search(r"rect=([\d,]+)", lines[0]).group(1).split(",")]
        crop = out / "shots" / "automap_minimap_square.png"
        # (inside its rounded corners: the minimap is solid at opacity 100, the game shows through nowhere)
        Image.open(pic).convert("RGBA").crop((r[0] + 12, r[1] + 12, r[2] - 12, r[3] - 12)).save(crop)
        got[f"{name}/minimap"] = pixels_hash(crop)

    def solid(wide: str) -> dict:
        d = settings(wide)
        d["map"]["minimap_opacity"] = 100
        return d

    out = base / "indoor-castle"
    text = run(exe, a.rom, out, {"ELTALE_AUTOMAP_DUMP": "1", "ELTALE_WINDOW_SIZE": "1280x960"},
               ["--frames", "900", "--input", str(ROOT / "tests/title-new-game.input")], solid("off"), None)
    minimap("indoor-castle", out, text)
    out = base / "indoor-inn"
    pak = base / "inn.mpk"
    pak.unlink(missing_ok=True)
    subprocess.run([str(exe), "--headless", "--fast", "--frames", "16000", "--input", str(ROOT / "tests/tour-save.input"),
                    "--pak", str(pak), "--rom", a.rom], env=dict(os.environ, ELTALE_MAP_TOUR="1"), capture_output=True,
                   cwd=exe.parent)
    text = run(exe, a.rom, out, {"ELTALE_AUTOMAP_DUMP": "1", "ELTALE_AUTOMAP_MINIMAP_AT": "18,7",
                                 "ELTALE_WINDOW_SIZE": "1280x960"},
               ["--frames", "1400", "--input", str(ROOT / "tests/automap-continue.input"), "--pak", str(pak)],
               solid("off"), None)
    minimap("indoor-inn", out, text)

    if a.update:
        HASHES.write_text("".join(f"{k} {v}\n" for k, v in sorted(got.items())), encoding="utf-8", newline="\n")
        print(f"AUTOMAP_TEST wrote {len(got)} hashes to {HASHES.relative_to(ROOT)}")
    else:
        want = {}
        if HASHES.exists():
            for line in HASHES.read_text(encoding="utf-8").splitlines():
                k, v = line.split()
                want[k] = v
        for k in sorted(set(want) | set(got)):
            if want.get(k) != got.get(k):
                print(f"AUTOMAP_TEST FAIL {k}: {got.get(k, 'missing')} (want {want.get(k, 'none')})")
                fail = True
        print(f"AUTOMAP_TEST {len(got)} pictures hashed")
    print(f"AUTOMAP_TEST pictures in {base}")
    print("AUTOMAP_TEST_FAIL" if fail else "AUTOMAP_TEST_OK")
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
