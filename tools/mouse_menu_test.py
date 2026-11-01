#!/usr/bin/env python3
"""The mouse in the game's menus, headless (port/p0/mouse_menu.c; docs/CONTROLS.md "The mouse in menus").

    python tools/mouse_menu_test.py --exe out/p1-windows/eltale-p1.exe --rom eltalemonsters.jp.z64
        [--saves C:/code/eltale/out/bugsaves] [--pak SAVE.pak] [--out out/mouse_menu_test] [--only NAME,...]

Each case plays an input script whose `mouse` rows point and click at the menus (picture units: x across the 1x
picture, 320 wide at 4:3 and 426 at 16:9, so a CENTRE menu's x moves by 53 at 16:9 and the HUD's LEFT crystals do
not), with ELTALE_MOUSE_LOG=1: the game prints what each click pressed (P1_MOUSE_MENU) and the state it led to
(P1_MOUSE_STATE). A case passes when
- the states after the clicks are the ones expected (the right item, spell, save, answer or option was picked),
- 4:3 and 16:9 give the same states and the same game sound (the game runs the same; only the pointer's x differs),
- the same case played with the controller (the buttons and stick pushes the clicks stand for) ends on the same
  frame list (last_hash, runs without pictures) as the mouse's.
The 4:3 and 16:9 runs write their pictures (--dump-frames) to OUT/<case>-<shape>/frame_N.png: before and while
pointing (hover highlights, the crystal's spell list).

Cases: items (the item menu: point, click, right click), spell_field and spell_battle (click a crystal for its spell
list, click a name to add it, click the middle name to cast), load (the title's "Continue" and the save list),
delete (the save menu's question: point at "No" and click), pak (the Controller Pak manager: the note list, delete,
confirm). Saves: <saves>/items and <saves>/battle-stuck (quick saves; tools/make_items_save.py); a Controller Pak
holding a save (--pak, else made once by tests/tour-save.input as tests/p1_checks.sh does, a few minutes).
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

START_TITLE = ["5 0x1000 0 0", "7 0 0 0"]  # Start on the title (the quick load follows at VI 100)


def walk_north():
    return ["120 0 0 80", "200 0 0 0"]


# Each case: the mouse script (rows; {c:N} is a CENTRE x, {l:N} a LEFT one), the controller script that stands for
# it, and what the states after the clicks must hold (in order; a dict of field: value, or a function of the line).
CASES = [
    dict(name="items", save="items", frames=300, env={},
         mouse=START_TITLE + ["300 0x0010 0 0", "302 0 0 0",
                              "mouse 400 0 0 0 0 {c:166} 135",   # point at the 4th item: the cursor goes there
                              "mouse 440 0 0 0 0 {c:132} 135",   # the 2nd
                              "mouse 480 0 0 0 2",               # click: A on it (full HP: the bread stays)
                              "mouse 520 0 0 0 4",               # right click: B closes the menu
                              "mouse 540 0 0 0 8"],
         pad=START_TITLE + ["300 0x0010 0 0", "302 0 0 0", "420 0 80 0", "428 0 0 0", "480 0x8000 0 0", "482 0 0 0",
                            "520 0x4000 0 0", "522 0 0 0"],
         hovers=[("4", "3"), ("4", "1")],
         states=[{"item": "0+1", "core_flags": lambda v: int(v, 16) & 1},
                 {"core_flags": lambda v: not int(v, 16) & 1}],
         pictures=[195, 200, 220, 260]),
    dict(name="spell_field", save="battle-stuck", frames=260, env={},
         mouse=START_TITLE + ["mouse 200 0 0 0 0 {l:78} 205",    # the water crystal glows
                              "mouse 220 0 0 0 2",               # click: C down, its spell list
                              "mouse 260 0 0 0 0 {c:160} 196",   # the name below: water again
                              "mouse 280 0 0 0 2",               # click: C down
                              "mouse 300 0 0 0 0 {c:160} 57",    # the spell put together
                              "mouse 320 0 0 0 2",               # click: A casts it
                              "mouse 340 0 0 0 8"],
         pad=START_TITLE + ["220 0x0004 0 0", "222 0 0 0", "280 0x0004 0 0", "282 0 0 0", "320 0x8000 0 0", "322 0 0 0"],
         same_inputs=True,
         # water's list; water twice is not known yet (the game refuses it: still one element); A looks the spell up
         # (water's first, id 200) but in the field with no target it is not cast, as with the controller
         states=[{"spell": lambda v: v.startswith("1:2"), "core_flags": lambda v: int(v, 16) & 4},
                 {"spell": lambda v: v.startswith("1:2")},
                 {"spell": lambda v: v.startswith("1:2"), "id": "200"}],
         pictures=[98, 102, 115, 132, 150]),
    dict(name="spell_battle", save="battle-stuck", frames=240, env={"ELTALE_BATTLE_LOG": "2"},
         # (the player can act from about VI 360: the battle's start)
         mouse=START_TITLE + walk_north() + ["mouse 370 0 0 0 0 {l:34} 205",   # the fire crystal glows
                                             "mouse 380 0 0 0 2",              # click: C up, fire's list
                                             "mouse 400 0 0 0 0 {c:160} 147",  # the name above: its gems glow
                                             "mouse 410 0 0 0 0 {c:160} 57",
                                             "mouse 420 0 0 0 2",              # click: A casts the fire spell
                                             "mouse 440 0 0 0 8"],
         pad=START_TITLE + walk_north() + ["380 0x0008 0 0", "382 0 0 0", "420 0x8000 0 0", "422 0 0 0"],
         same_inputs=True,
         states=[{"spell": lambda v: v.startswith("1:0"), "core_flags": lambda v: int(v, 16) & 4},
                 {"spell": lambda v: v.startswith("0:"), "id": "0", "core_flags": lambda v: not int(v, 16) & 4}],
         pictures=[181, 184, 190, 199, 205, 212]),
    dict(name="load", pak=True, frames=420, env={},
         mouse=["10 0x1000 0 0", "12 0 0 0",
                "mouse 120 0 0 0 0 {c:160} 120", "mouse 122 0 0 0 2",   # "Press Start": a click is Start
                "mouse 300 0 0 0 0 {c:150} 166",                        # Continue: the cursor goes there
                "mouse 330 0 0 0 2",                                    # click: A
                "mouse 600 0 0 0 0 {c:168} 84", "mouse 620 0 0 0 2",    # the save: click loads it
                "mouse 640 0 0 0 8"],
         pad=["10 0x1000 0 0", "12 0 0 0", "120 0x1000 0 0", "122 0 0 0", "300 0 80 0", "304 0 0 0",
              "330 0x8000 0 0", "332 0 0 0", "620 0x8000 0 0", "622 0 0 0"],
         hovers=[("0", "-1"), ("0", "1"), ("0", "0")],
         states=[{"mode": "3"}, {"title": "20"}, {"mode": "2", "slot": "0"}],
         pictures=[150, 155, 305]),
    dict(name="delete", pak=True, frames=420, env={},
         mouse=["10 0x1000 0 0", "12 0 0 0",
                "mouse 120 0 0 0 0 {c:160} 120", "mouse 122 0 0 0 2",
                "mouse 300 0 0 0 0 {c:225} 166",                        # the third item (delete)
                "mouse 330 0 0 0 2",
                "mouse 600 0 0 0 0 {c:168} 84", "mouse 620 0 0 0 2",    # the save: the question
                "mouse 700 0 0 0 0 {c:240} 182",                        # point at "No"
                "mouse 720 0 0 0 2",                                    # click: A on "No"
                "mouse 760 0 0 0 4",                                    # right click: B leaves
                "mouse 780 0 0 0 8"],
         pad=["10 0x1000 0 0", "12 0 0 0", "120 0x1000 0 0", "122 0 0 0", "300 0 80 0", "304 0 0 0",
              "316 0 80 0", "320 0 0 0", "330 0x8000 0 0", "332 0 0 0", "620 0x8000 0 0", "622 0 0 0",
              "700 0 0 -80", "704 0 0 0", "720 0x8000 0 0", "722 0 0 0", "762 0x4000 0 0", "764 0 0 0"],
         hovers=[("0", "-1"), ("2", "2"), ("0", "0"), ("1", "1")],
         # the question (dialog 5), "No" picked (back to the list), then B: the leaving message (dialog 7)
         states=[{"mode": "3"}, {"title": "30"}, {"dialog": "5"}, {"choice": "1"}, {"dialog": "7"}],
         pictures=[345, 355]),
    dict(name="pak", pak=True, frames=400, env={}, pak_manager=True,
         mouse=["0 0x1000 0 0", "60 0 0 0",
                "mouse 100 0 0 0 0 100 58", "mouse 102 0 0 0 2",        # the note list (left), A
                "mouse 150 0 0 0 0 150 94", "mouse 152 0 0 0 2",        # note 1, A
                "mouse 200 0 0 0 0 109 198", "mouse 202 0 0 0 2",       # Delete (left), A
                "mouse 250 0 0 0 0 116 168", "mouse 252 0 0 0 2",       # confirm (left), A
                "mouse 300 0 0 0 4",                                    # right click: B leaves
                "mouse 320 0 0 0 8"],
         pad_file="tests/pak-manager-delete.input",
         states=[], pictures=[]),
]


def script(rows, side):
    out = []
    for r in rows:
        r = re.sub(r"\{c:(-?\d+)\}", lambda m: str(int(m.group(1)) + side), r)
        r = re.sub(r"\{l:(-?\d+)\}", lambda m: m.group(1), r)
        out.append(r)
    return "\n".join(out) + "\n"


def run(case, args, kind, work: Path, pak: Path):
    """kind: 'mouse43' / 'mouse169' (pictures) or 'mouse' / 'pad' (no pictures: the frame list's hash)"""
    d = work / f"{case['name']}-{kind}"
    if d.exists():
        shutil.rmtree(d)
    (d / "data").mkdir(parents=True)
    if case.get("save"):
        src = args.saves / case["save"]
        if not src.is_dir():
            raise SystemExit(f"mouse_menu_test: {case['name']} needs the quick save {src} (--saves)")
        shutil.copytree(src, d / "data", dirs_exist_ok=True)
    wide = kind == "mouse169"
    side = 53 if wide else 0
    if kind == "pad" and case.get("pad_file"):
        (d / "in.input").write_text((ROOT / case["pad_file"]).read_text())
    else:
        (d / "in.input").write_text(script(case["mouse"], side))
    env = {k: v for k, v in os.environ.items() if not k.startswith("ELTALE_")}
    env.update(case["env"])
    env["ELTALE_DATA_DIR"] = str(d / "data")
    env["ELTALE_MOUSE_LOG"] = "1"
    if case.get("save"):
        env["ELTALE_QUICK_LOAD_AT"] = "100"
    cmd = [str(args.exe), "--headless", "--fast", "--frames", str(case["frames"]), "--input", str(d / "in.input")]
    if case.get("pak"):
        p = d / "save.pak"
        shutil.copy(pak, p)
        cmd += ["--pak", str(p)]
    if kind in ("mouse43", "mouse169") and case["pictures"]:
        (d / "pictures").mkdir()
        env["ELTALE_WIDESCREEN"] = "16:9" if wide else "off"
        if case["pictures"]:
            env["ELTALE_DUMP_FRAMES"] = ",".join(str(f) for f in case["pictures"])
        cmd += ["--dump-frames", str(d / "pictures")]
    cmd += ["--rom", str(args.rom)]
    p = subprocess.run(cmd, cwd=d, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = p.stdout.decode("utf-8", "replace")
    (d / "log.txt").write_text(out)
    (d / "err.txt").write_bytes(p.stderr)
    return out


def field(line, name):
    m = re.search(r"\b" + name + r"=(\S+)", line)
    return m.group(1) if m else None


def check_states(case, out, label):
    problems = []
    states = [l for l in out.splitlines() if l.startswith("P1_MOUSE_STATE")]
    want = case["states"]
    if len(states) < len(want):
        problems.append(f"{label}: {len(states)} clicks seen, {len(want)} expected")
    for i, w in enumerate(want[:len(states)]):
        for k, v in w.items():
            got = field(states[i], k)
            ok = v(got) if callable(v) and got is not None else got == v
            if not ok:
                problems.append(f"{label}: click {i + 1}: {k}={got} ({states[i]})")
    hovers = [l for l in out.splitlines() if l.startswith("P1_MOUSE_HOVER")]
    for i, (tgt_value) in enumerate(case.get("hovers", [])):
        _, value = tgt_value
        if i >= len(hovers) or field(hovers[i], "value") != value:
            problems.append(f"{label}: pointing {i + 1}: {hovers[i] if i < len(hovers) else 'none'}, wanted value={value}")
    return problems, states


def strip_vi(lines):
    return [re.sub(r"vi=\d+ ", "", l) for l in lines]


def make_pak(args, work: Path) -> Path:
    pak = work / "tour-save.pak"
    if pak.exists():
        return pak
    print("making a Controller Pak with a save (tests/tour-save.input, a few minutes)...", flush=True)
    d = work / "tour-save"
    d.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("ELTALE_")}
    env["ELTALE_MAP_TOUR"] = "1"
    env["ELTALE_DATA_DIR"] = str(d)
    subprocess.run([str(args.exe), "--headless", "--fast", "--frames", "16000", "--input", str(ROOT / "tests/tour-save.input"),
                    "--pak", str(pak), "--rom", str(args.rom)], cwd=d, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    if not pak.exists():
        raise SystemExit("mouse_menu_test: the tour save made no Controller Pak")
    return pak


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--saves", type=Path, default=Path("C:/code/eltale/out/bugsaves"))
    ap.add_argument("--pak", type=Path, help="a Controller Pak image holding a save (else made once)")
    ap.add_argument("--out", type=Path, default=ROOT / "out/mouse_menu_test")
    ap.add_argument("--only", help="cases, comma-separated")
    args = ap.parse_args()
    args.exe = args.exe.resolve()
    args.rom = args.rom.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    cases = [c for c in CASES if not args.only or c["name"] in args.only.split(",")]
    pak = None
    if any(c.get("pak") for c in cases):
        pak = args.pak.resolve() if args.pak else make_pak(args, args.out)
    failed = 0
    for case in cases:
        problems = []
        o43 = run(case, args, "mouse43", args.out, pak)
        o169 = run(case, args, "mouse169", args.out, pak)
        om = run(case, args, "mouse", args.out, pak)
        op = run(case, args, "pad", args.out, pak)
        p43, s43 = check_states(case, o43, "4:3")
        p169, s169 = check_states(case, o169, "16:9")
        problems += p43 + p169
        if strip_vi(s43) != strip_vi(s169):
            problems.append("4:3 and 16:9 states differ")
        audio = [re.search(r"P1_AUDIO .*hash=(\w+)", o) for o in (o43, o169, om, op)]
        audio = [a.group(1) if a else None for a in audio]
        if audio[0] != audio[1] or audio[0] != audio[2]:
            problems.append(f"the game's sound differs between the mouse runs: {audio[:3]}")
        if case.get("same_inputs") and audio[3] != audio[2]:
            problems.append(f"the controller run's sound differs: {audio[3]} (mouse {audio[2]})")
        if case.get("pak_manager"):
            frames = [[l for l in o.splitlines() if l.startswith("P1_CPU_FRAME")] for o in (om, op)]
            last = [f[-1].split("hash=")[1] if f else None for f in frames]
            if last[0] is None or last[0] != last[1]:
                problems.append(f"the Pak manager's last screen differs from the controller's: {last}")
            got = last[0]
        else:
            hashes = [re.search(r"P1_OS_OK .*last_hash=(\w+)", o) for o in (om, op)]
            hashes = [h.group(1) if h else None for h in hashes]
            if hashes[0] is None or hashes[0] != hashes[1]:
                problems.append(f"the mouse and the controller end differently: {hashes}")
            got = hashes[0]
        status = "OK" if not problems else "FAIL"
        failed += bool(problems)
        print(f"MOUSE_MENU {case['name']} {status} clicks={len(s43)} audio={audio[0]} {got}")
        for line in s43:
            print("   ", line)
        for pr in problems:
            print("    !", pr)
    print("MOUSE_MENU_TEST " + ("OK" if not failed else f"FAIL ({failed})"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
