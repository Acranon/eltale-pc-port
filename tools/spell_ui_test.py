#!/usr/bin/env python3
"""The spell UI, headless (port/p0/spell_ui.c; docs/SPELL_UI.md).

    python tools/spell_ui_test.py --exe out/p1-windows/eltale-p1.exe --rom eltalemonsters.jp.z64
        [--saves C:/code/eltale/out/bugsaves] [--out out/spell_ui_test] [--only NAME,...]

Each case plays an input script (`mouse` rows: the pointer and its clicks; `spellkey` rows: the UI's keys) from a
quick save, with ELTALE_SPELL_UI=1 and ELTALE_SPELLUI_LOG=1: the game prints every press the UI made
(P1_SPELLUI_PRESS), the spell input after it (P1_SPELLUI_STATE), the quick bar (P1_SPELLUI_SLOTS) and what is shown
(P1_SPELLUI_VIEW: shown/live/building, which crystals the game would take, each slot's state). A case passes when
- the lines it expects are there, in order (the spell built, taken back, cast; slots kept, swapped, paged; the greyed
  states at low MP);
- 4:3 and 16:9 (pictures: OUT/<case>-43 and -169/pictures) print the same UI lines and the same game sound;
- the same case played with the controller alone (the C buttons, A and B the UI pressed, at the same reads, with the
  UI off) ends on the same frame list (last_hash) and the same sound as the mouse's run (both without pictures).

Saves (made here from <saves>/battle-stuck's quick save, only numbers changed, as tools/make_items_save.py does):
"spells" (element levels 12/10/25/8, MP 30/30: most two- and three-element spells learned) and "lowmp" (the same, MP
2/30), with a quick bar file written for it (spellbar/save_<key>.bar: the key the game hashes from the save record).
Battles: ELTALE_BATTLE_LOG=2 (the next steps start one).
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RECORD = 0x18 + 44  # the quick save's record (port/p0/quicksave.c "File")

START = ["5 0x1000 0 0", "7 0 0 0"]  # Start on the title (the quick load follows at VI 100)
WALK = ["120 0 0 80", "200 0 0 0"]   # a few steps north: a battle (ELTALE_BATTLE_LOG=2)

# {c:X} a CENTRE x (the panel; +53 at 16:9), {l:X} a LEFT x, {s:I} the middle of quick-bar slot I (LEFT; the bar
# has its "Spells" label at 16:9), {d:P} page dot P (16:9 only)
PANEL_CRYSTAL = [49, 123, 197, 271]   # the panel's crystals (and the next-spell plates under them), CENTRE x
NEXT_Y, PLATE = 160, (160, 110)
HUD_CRYSTAL = [34, 56, 78, 100]       # the HUD's crystals, LEFT x (y 205)

CASES = [
    dict(name="field", save="spells", frames=360, battle=False,
         rows=START + [
             "mouse 200 0 0 0 0 {l:78} 205",       # the HUD's water crystal
             "mouse 210 0 0 0 2",                   # click: C down (the panel opens with water)
             "mouse 230 0 0 0 0 {c:123} 160",       # the plate under earth: "Healing Lv1"
             "mouse 240 0 0 0 2",                   # click: earth (C left)
             "mouse 260 0 0 0 4",                   # right click: earth taken back (B)
             "mouse 280 0 0 0 2",                   # earth again
             "mouse 300 0 0 0 0 {s:2} 205",         # the quick bar's third slot ("+")
             "mouse 310 0 0 0 2",                   # click: Healing kept there
             "mouse 330 0 0 0 0 {c:160} 110",       # the plate
             "mouse 340 0 0 0 2",                   # click: cast (A) - Healing works in the field
             "mouse 360 0 0 0 0 {s:2} 205",
             "mouse 560 0 0 0 2",                   # (after the cast) the slot: Healing again (C down, C left, A)
             "spellkey 660 0x40",                   # key 7 (the third slot): once more
             "mouse 680 0 0 0 8"],
         expect=[("P1_SPELLUI_STATE", "count=2 dirs=21"), ("P1_SPELLUI_STATE", "count=1"),
                 ("P1_SPELLUI_STATE", "count=2 dirs=21"), ("P1_SPELLUI_SLOTS", "page=0 - - 21 -"),
                 ("P1_SPELLUI_STATE", "count=0 .* mp=28"),
                 ("P1_SPELLUI_PRESS", "why=slot-element"), ("P1_SPELLUI_PRESS", "why=slot-element"),
                 ("P1_SPELLUI_PRESS", "why=slot-cast"), ("P1_SPELLUI_STATE", "count=0 .* mp=26"),
                 ("P1_SPELLUI_PRESS", "why=slot-cast"), ("P1_SPELLUI_STATE", "count=0 .* mp=24")],
         pictures=[106, 118, 125, 135, 150, 165]),
    dict(name="battle", save="spells", frames=520, battle=True,
         rows=START + WALK + [
             "mouse 370 0 0 0 0 {l:78} 205",        # the HUD's water crystal
             "mouse 380 0 0 0 2",                   # click: the panel opens with water
             "mouse 400 0 0 0 0 {c:197} 160",       # the plate under water: "Water Pillar Lv2"
             "mouse 410 0 0 0 2",
             "mouse 430 0 0 0 4",                   # right click: back to Water Pillar Lv1
             "mouse 440 0 0 0 0 {s:4} 205",         # slot 5 ("+"): keep it there
             "mouse 450 0 0 0 2",
             "mouse 456 0 0 0 0 {c:160} 110",       # the plate: dragged onto slot 2
             "mouse 460 0 0 0 18",
             "mouse 464 0 0 0 0 {c:150} 150",
             "mouse 468 0 0 0 0 {s:1} 205",
             "mouse 472 0 0 0 32",
             "mouse 476 0 0 0 0 {c:160} 110",       # the plate: click casts Water Pillar Lv1
             "mouse 480 0 0 0 2",
             "mouse 820 0 0 0 0 {s:4} 205",         # (the next turn) slot 5 dragged onto slot 1: swapped
             "mouse 860 0 0 0 18",
             "mouse 864 0 0 0 0 {s:3} 205",
             "mouse 868 0 0 0 0 {s:1} 205",
             "mouse 872 0 0 0 32 {s:0} 205",
             "mouse 890 0 0 0 0 {s:2} 205",         # the wheel over the bar: page 2
             "mouse 900 0 0 -1 0",
             "spellkey 910 0x400",                  # the page key (,): back to page 1
             "mouse 920 0 0 0 0 {s:0} 205",
             "mouse 980 0 0 0 2",                   # slot 1: Water Pillar Lv1 (C down, A)
             "mouse 1000 0 0 0 8"],
         expect=[("P1_SPELLUI_STATE", "count=2 dirs=22"), ("P1_SPELLUI_STATE", "count=1 dirs=2"),
                 ("P1_SPELLUI_SLOTS", "page=0 - - - - 2 -"), ("P1_SPELLUI_SLOTS", "page=0 - 2 - - 2 -"),
                 ("P1_SPELLUI_STATE", "count=0 .* mp=29"),
                 ("P1_SPELLUI_SLOTS", "page=0 2 2 - - - -"), ("P1_SPELLUI_SLOTS", "page=1 "),
                 ("P1_SPELLUI_SLOTS", "page=0 "), ("P1_SPELLUI_PRESS", "why=slot-element"),
                 ("P1_SPELLUI_PRESS", "why=slot-cast"), ("P1_SPELLUI_STATE", "count=0 .* mp=28")],
         pictures=[191, 206, 216, 226, 233, 418, 423, 452]),
    dict(name="lowmp", save="lowmp", frames=260, battle=False,
         bar=["slot 0 2", "slot 1 2 1", "slot 2 2 1 1", "slot 3 0", "slot 4 2 3"],
         rows=START + [
             "spellkey 200 0x4",                    # key 3: water (1 MP)
             "mouse 220 0 0 0 0 {c:123} 160",       # earth: Healing Lv1 (2 MP, all there is)
             "mouse 230 0 0 0 2",
             "mouse 250 0 0 0 0 {s:2} 205",         # slot 3: Healing Lv2 (3 MP): not taken
             "spellkey 300 0x1000",                 # Esc: both elements back
             "mouse 320 0 0 0 2",                   # (the panel is closed: a click on the slot casts nothing)
             "mouse 340 0 0 0 8"],
         # (Water Pillar and Fire Ball: battle spells, x in the field; Healing Lv2: 3 MP, m)
         expect=[("P1_SPELLUI_SAVE", "read .* result=1"), ("P1_SPELLUI_VIEW", "view=SLb .... xcmxc-"),
                 ("P1_SPELLUI_VIEW", r"view=SLB 1111 xcmxc\+"), ("P1_SPELLUI_VIEW", r"view=SLB 0000 xcmxc\+"),
                 ("P1_SPELLUI_PRESS", "why=slot-back"), ("P1_SPELLUI_PRESS", "why=slot-back"),
                 ("P1_SPELLUI_VIEW", "view=SLb .... xcmxc-"), ("P1_SPELLUI_SLOT_REFUSED", "slot=2 state=m")],
         pictures=[106, 116, 126, 140]),
    dict(name="persist", save="spells", frames=260, battle=False, quick_save=400,
         rows=START + [
             "spellkey 200 0x4",                    # water, earth: Healing Lv1
             "spellkey 220 0x2",
             "mouse 240 0 0 0 0 {s:5} 205",         # kept in slot 6
             "mouse 250 0 0 0 2",
             "spellkey 280 0x1000",                 # Esc: taken back (F2 then saves at VI 400)
             "mouse 290 0 0 0 8"],
         expect=[("P1_SPELLUI_SLOTS", "page=0 - - - - - 21"), ("P1_SPELLUI_SAVE", "write slot=0")],
         then=dict(frames=200, quick_load=100,
                   rows=["5 0x1000 0 0", "7 0 0 0", "spellkey 160 0x200"],  # key 0 (slot 6): Healing
                   expect=[("P1_SPELLUI_SAVE", "read .* result=1"), ("P1_SPELLUI_VIEW", "view=S.b .... -----c"),
                           ("P1_SPELLUI_PRESS", "why=slot-cast"), ("P1_SPELLUI_STATE", "count=0 .* mp=28")]),
         pictures=[120]),
]


# The host's whole path in a hidden window (ELTALE_INPUT_TEST: SDL's own events by pad read, nothing reaches the
# desktop; 640x480, 4:3: a picture unit is 2 window pixels), with mouse look: Tab turns it on (the view turns), the
# key 3 opens the panel (water; while mouse look is on the pointer is the camera's, so a key or a C button opens it) and
# mouse look goes off; a click on the plate under earth works; a right drag there is B (undo) and never turns the view;
# a click on the spell's plate casts (on the release) and the panel closes; then mouse motion: mouse look stays off.
WINDOW_EVENTS = ["3 key_down Return", "5 key_up Return",
                 "110 key_down Tab", "112 key_up Tab", "115 motion 20 0",
                 "120 key_down 3", "122 key_up 3",
                 "140 pointer 247 321", "150 mouse_down left", "152 mouse_up left",
                 "160 mouse_down right", "162 motion 30 0", "164 motion 30 0", "166 mouse_up right",
                 "170 mouse_down left", "172 mouse_up left",
                 "175 pointer 320 220", "184 mouse_down left", "186 mouse_up left",
                 "200 motion 40 0", "205 motion 40 0"]
WINDOW_EXPECT = [("P1_SPELLUI_PRESS", "buttons=0004 why=element"), ("P1_SPELLUI_PRESS", "buttons=0002 why=element"), ("P1_SPELLUI_STATE", "count=2 dirs=21"),
                 ("P1_SPELLUI_PRESS", "buttons=4000 why=back"), ("P1_SPELLUI_STATE", "count=1 dirs=2"),
                 ("P1_SPELLUI_PRESS", "buttons=0002 why=element"), ("P1_SPELLUI_PRESS", "buttons=8000 why=cast"),
                 ("P1_SPELLUI_STATE", "count=0 .* mp=28")]


def window_case(args, work: Path, saves: Path):
    d = work / "window"
    if d.exists():
        shutil.rmtree(d)
    shutil.copytree(saves / "spells", d)
    (d / "settings.json").write_text('{"version": 2, "audio": {"volume": 0}, "game": {"language": "en"}}\n')
    (d / "in.events").write_text("\n".join(WINDOW_EVENTS) + "\n")
    env = {k: v for k, v in os.environ.items() if not k.startswith("ELTALE_")}
    env.update(ELTALE_HIDDEN_WINDOW="1", ELTALE_DATA_DIR=str(d), ELTALE_INPUT_TEST=str(d / "in.events"),
               ELTALE_QUICK_LOAD_AT="100", ELTALE_SPELLUI_LOG="1", ELTALE_MOUSE_LOG="1", ELTALE_WINDOW_SIZE="640x480",
               ELTALE_WIDESCREEN="off")
    p = subprocess.run([str(args.exe), "--window", "--fast", "--frames", "260", "--rom", str(args.rom)], env=env, cwd=d,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout.decode("utf-8", "replace")
    (d / "log.txt").write_text(out)
    problems = check_expect(WINDOW_EXPECT, out, "window")
    # (the host's lines are on stderr, the game's on stdout: checked apart) mouse look: off at the start, on with Tab, off
    # when the panel opens (screen=1), and not on again after it closed
    looks = re.findall(r"P1_HOST_LOOK (on|off) read=\d+ screen=(\d)", out)
    if looks != [("off", "1"), ("on", "0"), ("off", "1")]:
        problems.append(f"window: mouse look went {looks} (wanted off, on, off when the panel opened, and no more)")
    return problems


def fnv(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def make_save(src: Path, dst: Path, mp: int, levels, bar_rows=None):
    """battle-stuck's quick save with other MP and spirit levels (the record's SaveHeader is big-endian)"""
    q = bytearray((src / "quicksaves/quick0.elq").read_bytes())
    if q[:8] != b"ELTQUICK":
        raise SystemExit(f"{src}: not a quick save")
    q[RECORD + 0x08:RECORD + 0x0C] = struct.pack(">HH", mp, 30)
    q[RECORD + 0x24:RECORD + 0x28] = bytes(levels)
    q[0x10:0x18] = struct.pack("<Q", fnv(bytes(q[0x18:])))
    (dst / "quicksaves").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src / "controller-pak.mpk", dst / "controller-pak.mpk")
    (dst / "quicksaves/quick0.elq").write_bytes(q)
    if bar_rows:
        # the key the game hashes (spell_ui.c Port_SpellUiSaveEvent): play time, day, map, sub-area as 32-bit
        # little-endian numbers, then the 0xC8 bytes of the SaveGameStateBlock
        rec = bytes(q[RECORD:])
        play, day = struct.unpack(">HH", rec[0x38:0x3C])
        mapid, sub = struct.unpack(">ii", rec[0x3C:0x44])
        key = fnv(struct.pack("<IIii", play, day, mapid, sub) + rec[0x80:0x80 + 0xC8])
        (dst / "spellbar").mkdir(exist_ok=True)
        (dst / f"spellbar/save_{key:016x}.bar").write_text("ELTSPELLBAR 1\npage 0\n" + "\n".join(bar_rows) + "\n")


def script(rows, wide):
    side = 53 if wide else 0
    out = []
    for r in rows:
        r = re.sub(r"\{c:(-?\d+)\}", lambda m: str(int(m.group(1)) + side), r)
        r = re.sub(r"\{l:(-?\d+)\}", lambda m: m.group(1), r)
        r = re.sub(r"\{s:(\d)\}", lambda m: str(124 + 3 + (38 if wide else 0) + 20 * int(m.group(1)) + 8), r)
        out.append(r)
    return "\n".join(out) + "\n"


def run(case, args, kind, work: Path, saves: Path, rows=None, data_from=None, frames=None, extra_env=None):
    """kind: '43' / '169' (pictures), 'mouse' (no pictures), 'pad' (the controller's presses, the UI off)"""
    d = work / f"{case['name']}-{kind}"
    if d.exists():
        shutil.rmtree(d)
    (d / "data").mkdir(parents=True)
    shutil.copytree(data_from or saves / case["save"], d / "data", dirs_exist_ok=True)
    wide = kind == "169"
    (d / "in.input").write_text(rows if isinstance(rows, str) else script(rows or case["rows"], wide))
    env = {k: v for k, v in os.environ.items() if not k.startswith("ELTALE_")}
    env.update({"ELTALE_DATA_DIR": str(d / "data"), "ELTALE_QUICK_LOAD_AT": "100", "ELTALE_LANG": "en",
                "ELTALE_SPELL_UI": "0" if kind == "pad" else "1", "ELTALE_SPELLUI_LOG": "1", "ELTALE_MOUSE_LOG": "1"})
    if case.get("battle"):
        env["ELTALE_BATTLE_LOG"] = "2"
    env.update(extra_env or {})
    cmd = [str(args.exe), "--headless", "--fast", "--frames", str(frames or case["frames"]), "--input", str(d / "in.input")]
    if kind in ("43", "169") and case["pictures"]:
        (d / "pictures").mkdir()
        env["ELTALE_WIDESCREEN"] = "16:9" if wide else "off"
        env["ELTALE_DUMP_FRAMES"] = ",".join(str(f) for f in case["pictures"])
        cmd += ["--dump-frames", str(d / "pictures")]
    cmd += ["--rom", str(args.rom)]
    p = subprocess.run(cmd, cwd=d, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = p.stdout.decode("utf-8", "replace")
    (d / "log.txt").write_text(out)
    (d / "err.txt").write_bytes(p.stderr)
    if p.returncode:
        out += f"\nP1_TEST_EXIT {p.returncode}\n"
    return out, d


def ui_lines(out):
    keep = ("P1_SPELLUI_PRESS", "P1_SPELLUI_STATE", "P1_SPELLUI_SLOTS", "P1_SPELLUI_VIEW", "P1_SPELLUI_SAVE",
            "P1_SPELLUI_SLOT_REFUSED", "P1_SPELLUI_MACRO")
    return [l for l in out.splitlines() if l.startswith(keep)]


def check_expect(expect, out, label):
    lines = ui_lines(out)
    problems, at = [], 0
    for tag, pattern in expect:
        while at < len(lines) and not (lines[at].startswith(tag + " ") and re.search(pattern, lines[at])):
            at += 1
        if at == len(lines):
            problems.append(f"{label}: no {tag} matching '{pattern}' (in order)")
            break
        at += 1
    if "P1_TEST_EXIT" in out or "P1_OS_OK" not in out:
        problems.append(f"{label}: the run did not finish")
    return problems


def strip_vi(lines):
    return [re.sub(r"vi=\d+ ", "", l) for l in lines]


def pad_rows(case, out):
    """the controller's script: the case's own pad rows and, at each read the mouse or the UI pressed something, those
    buttons for that read (the next read lets go)"""
    presses = {}
    for l in out.splitlines():
        m = re.match(r"P1_SPELLUI_PRESS vi=(\d+) buttons=([0-9a-f]+)", l)
        if not m:
            m = re.match(r"P1_MOUSE_MENU vi=(\d+) .*buttons=([0-9a-f]+)", l)
        if m and int(m.group(2), 16):
            vi = int(m.group(1))
            presses[vi] = presses.get(vi, 0) | int(m.group(2), 16)
    rows = {}
    for r in case["rows"]:
        if r.startswith(("mouse", "spellkey")):
            continue
        vi, rest = r.split(" ", 1)
        rows[int(vi)] = rest
    for vi, b in presses.items():
        rows[vi] = f"{b:#06x} 0 0"
        rows.setdefault(vi + 1, "0 0 0")
    return "".join(f"{vi} {rows[vi]}\n" for vi in sorted(rows))


def field(out, pattern):
    m = re.search(pattern, out)
    return m.group(1) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--saves", type=Path, default=Path("C:/code/eltale/out/bugsaves"))
    ap.add_argument("--out", type=Path, default=ROOT / "out/spell_ui_test")
    ap.add_argument("--only", help="cases, comma-separated")
    args = ap.parse_args()
    args.exe = args.exe.resolve()
    args.rom = args.rom.resolve()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    saves = work / "saves"
    src = args.saves / "battle-stuck"
    if not (src / "quicksaves/quick0.elq").is_file():
        raise SystemExit(f"spell_ui_test: needs the quick save {src} (--saves)")
    if saves.exists():
        shutil.rmtree(saves)
    make_save(src, saves / "spells", 30, [12, 10, 25, 8])
    make_save(src, saves / "lowmp", 2, [12, 10, 25, 8], next(c for c in CASES if c["name"] == "lowmp")["bar"])
    cases = [c for c in CASES if not args.only or c["name"] in args.only.split(",")]
    failed = 0
    for case in cases:
        problems = []
        extra = {"ELTALE_QUICK_SAVE_AT": str(case["quick_save"])} if case.get("quick_save") else None
        o43, _ = run(case, args, "43", work, saves, extra_env=extra)
        o169, _ = run(case, args, "169", work, saves, extra_env=extra)
        om, dm = run(case, args, "mouse", work, saves, extra_env=extra)
        problems += check_expect(case["expect"], o43, "4:3")
        problems += check_expect(case["expect"], o169, "16:9")
        if strip_vi(ui_lines(o43)) != strip_vi(ui_lines(o169)):
            problems.append("4:3 and 16:9 print different UI lines")
        audio = [field(o, r"P1_AUDIO .*hash=(\w+)") for o in (o43, o169, om)]
        if len(set(audio)) != 1:
            problems.append(f"the game's sound differs between the mouse runs: {audio}")
        got = field(om, r"P1_OS_OK .*last_hash=(\w+)")
        if not case.get("then"):
            op, _ = run(case, args, "pad", work, saves, rows=pad_rows(case, om))
            pad_hash = field(op, r"P1_OS_OK .*last_hash=(\w+)")
            pad_audio = field(op, r"P1_AUDIO .*hash=(\w+)")
            if got is None or got != pad_hash or audio[2] != pad_audio:
                problems.append(f"the mouse and the controller end differently: {got}/{audio[2]} pad {pad_hash}/{pad_audio}")
        else:
            # a second process from the first one's data folder (its quick save and its quick bar file)
            t = case["then"]
            ot, _ = run(case, args, "then", work, saves, rows=t["rows"], data_from=dm / "data", frames=t["frames"])
            problems += check_expect(t["expect"], ot, "then")
        status = "OK" if not problems else "FAIL"
        failed += bool(problems)
        print(f"SPELL_UI {case['name']} {status} audio={audio[0]} {got}")
        for pr in problems:
            print("    !", pr)
    if not args.only or "window" in args.only.split(","):
        problems = window_case(args, work, saves)
        failed += bool(problems)
        print(f"SPELL_UI window {'OK' if not problems else 'FAIL'}")
        for pr in problems:
            print("    !", pr)
    print("SPELL_UI_TEST " + ("OK" if not failed else f"FAIL ({failed})"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
