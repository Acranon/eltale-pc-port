#!/usr/bin/env python3
"""The unified pause menu, headless (port/p0/pause_menu.c; docs/PAUSE_MENU.md).

    python tools/pause_menu_test.py --exe out/p1-windows/eltale-p1.exe --rom eltalemonsters.jp.z64
        [--saves C:/code/eltale/out/bugsaves] [--out out/pause_menu_test] [--only NAME,...]

Each case plays an input script (pad rows; `mouse` rows: the pointer and its clicks, drags and wheel; `spellkey` rows:
Esc 0x1000 and the menu's Options key 0x2000) from battle-stuck's quick save (in the field of the Holy Plain), with
ELTALE_PAUSE_MENU=1 and ELTALE_PAUSE_LOG=1: the game prints what the menu did (P1_PAUSE_OPEN, _VIEW: the region, zoom,
view and the Options dialog; _MARK: the marker placed or taken away, with its place in the world; _OPTION: the options
changed; _PRESS: the Start or B it pressed; _CLOSED: the options as the game has them after the menu closed, with the
music's volume as set; _COMPASS: the cyan dot's bearing, the camera's yaw and where it is drawn; _SAVE: the marker's
file). A case passes when the lines it expects are there in order, at 4:3 and at 16:9 (the same lines; pictures in
OUT/<case>-43 and -169/pictures).

- pad: the controller alone: Start opens it; R, Z, L, C right and C left change the region (all six with a map are
  listed: the marker file says they were seen); C up and down zoom (it stops at 2x); the D-pad and the stick pan; A puts
  the marker at the cross, A on it takes it away, A again; Y (the Options key) opens Options, the D-pad moves the hand
  and changes both values, B closes Options (the menu stays), B closes the menu.
- mouse: the arrows, the wheel, the zoom button, a drag of the map, right clicks (marker on, off, on), the Options
  button, a click on the music's slider, a drag of the text speed's knob, Back, Close.
- esc: Esc opens the menu, O opens Options, Esc closes Options, Esc closes the menu.
- compass: a marker north of the player (from the file): the dot is shown, its bearing and place agree with the
  player's and the marker's positions and the camera (checked here from the logged numbers), also after the view is
  turned (the mouse camera); then the marker is moved to another region: the dot goes.
- persist: the marker placed, an F2 quick save, then a new process: F4 brings it back.
- window: the host's path in a hidden window (the keys Esc and O, a right click, a click on Close).
- same: a walk without opening the menu, the switch on and off: the same quick save bytes and the same game sound (the
  menu changes nothing until it is used).
- bars-full, bars-empty, bars-over: the quick save's stats changed (HP, MP, Defense, Agility, the element levels, the
  exp to the next spirit, the two options): full, empty and partly full, and past the caps (HP over its max, Defense
  300, an element at 99). (The orb's fullest is 99%: at the level's whole exp the game levels up at once.) In every picture no bar's fill lies outside its frame's inside, a full bar fills it to the
  end and an empty one leaves it empty; the Next-spirit orb and the Options sliders draw nothing outside their places
  (the same pixels round them in all three cases); the Options button's label is inside the button.
- labels: the Options dialog's words (Text Speed, Sound, Slow, Fast, Soft, Loud) against the English pictures the build
  made (OUT/en_images.inc next to the exe): every texel, all rows, drawn in the dialog and on the game's own Options page
  (the switch off: Start, Z), at 4:3 and 16:9, and in the dialog at 16:9 at scale 4 with MSAA 4 (each texel a whole
  4 x 4 block); the four choices whole in their pictures (each letter's shadow, one right and down, inside it).
"""

import argparse
import math
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
OPEN = ["200 0x1000 0 0", "204 0 0 0"]

# the layout (pause_menu.c layout()): frame x, the map's width, the Options button, Close
LAYOUT = {"169": dict(W=426, fx=10, map_w=214, opt_x=321, opt_w=59, close_x=383, col_x=234, col_w=162,
                     stat_bar_x=38, stat_bar_w=58, elem_bar_x=51, elem_bar_w=60, orb_x=120, orb_w=42),
          "43": dict(W=320, fx=8, map_w=150, opt_x=232, opt_w=50, close_x=285, col_x=168, col_w=130,
                     stat_bar_x=38, stat_bar_w=38, elem_bar_x=51, elem_bar_w=36, orb_x=90, orb_w=40)}
FY, VP_X, VP_Y, VP_H = 12, 6, 22, 176
KNOB = [138, 159, 179, 199, 219]  # the Options page's knob places (menu_options_*_row_pos), dialog x + 0x28


def where(name, kind):
    L = LAYOUT[kind]
    fx, mw = L["fx"], L["map_w"]
    vx, vy = fx + VP_X, FY + VP_Y
    mx = (L["W"] - 240) // 2
    cx, cy = fx + VP_X + 1 + (mw - 2) // 2, FY + VP_Y + 1 + (VP_H - 2) // 2  # the reticle
    table = {
        "prev": (fx + VP_X + 8, FY + 12), "next": (fx + VP_X + mw - 8, FY + 12),
        "zin": (vx + mw - 9, vy + VP_H - 22), "zout": (vx + mw - 9, vy + VP_H - 9),
        "opt": (fx + L["opt_x"] + L["opt_w"] // 2, FY + 11), "close": (fx + L["close_x"] + 6, FY + 11),
        "centre": (cx, cy), "back": (mx + 212, 66 + 94),
    }
    m = re.match(r"(centre)([+-]\d+)([+-]\d+)$", name)
    if m:
        return cx + int(m.group(2)), cy + int(m.group(3))
    m = re.match(r"(text|sound)(\d)$", name)
    if m:
        row = 0 if m.group(1) == "text" else 1
        return mx + KNOB[int(m.group(2))] - 0x28 + 4, 66 + (28, 71)[row] + 5
    return table[name]


def script(rows, kind):
    out = []
    for r in rows:
        def at(m):
            x, y = where(m.group(1), kind)
            return f"{x} {y}"
        out.append(re.sub(r"@(\S+)", at, r))
    return "\n".join(out) + "\n"


def mouse(vi, name, flags=0, wheel=0):
    return f"mouse {vi} 0 0 {wheel} {flags} @{name}"


CASES = [
    dict(name="pad", frames=420, rows=START + OPEN + [
        "230 0x0010 0 0", "234 0 0 0",   # R: region 1
        "250 0x0010 0 0", "254 0 0 0",   # R: region 2
        "270 0x2000 0 0", "274 0 0 0",   # Z: region 1
        "290 0x0020 0 0", "294 0 0 0",   # L: region 0
        "310 0x0002 0 0", "314 0 0 0",   # C left: region 5 (round)
        "330 0x0001 0 0", "334 0 0 0",   # C right: region 0
        "350 0x0008 0 0", "354 0 0 0",   # C up: 1.5x
        "370 0x0008 0 0", "374 0 0 0",   # C up: 2x
        "390 0x0008 0 0", "394 0 0 0",   # C up: (still 2x)
        "410 0x0004 0 0", "414 0 0 0",   # C down: 1.5x
        "430 0x0100 0 0", "470 0 0 0",   # D right held: pans (repeating)
        "480 0 0 60", "500 0 0 0",       # the stick up: pans
        "520 0x8000 0 0", "524 0 0 0",   # A: the marker at the cross
        "540 0x8000 0 0", "544 0 0 0",   # A: on it: taken away
        "560 0x8000 0 0", "564 0 0 0",   # A: placed again
        "spellkey 600 0x2000",           # Y / O: Options
        "620 0x0400 0 0", "624 0 0 0",   # D down: the music's row
        "640 0x0100 0 0", "644 0 0 0",   # D right: louder
        "660 0x0800 0 0", "664 0 0 0",   # D up: the text speed's row
        "680 0x0200 0 0", "684 0 0 0",   # D left: slower
        "700 0x4000 0 0", "704 0 0 0",   # B: Options closed, the menu stays
        "740 0x4000 0 0", "744 0 0 0",   # B: the menu closes (the game's own)
    ], expect=[("P1_PAUSE_OPEN", "region=0 spot=-1"), ("P1_PAUSE_VIEW", "region region=1"),
               ("P1_PAUSE_VIEW", "region region=2"), ("P1_PAUSE_VIEW", "region region=1"),
               ("P1_PAUSE_VIEW", "region region=0"), ("P1_PAUSE_VIEW", "region region=5"),
               ("P1_PAUSE_VIEW", "region region=0"), ("P1_PAUSE_VIEW", "zoom region=0 zoom=1"),
               ("P1_PAUSE_VIEW", "zoom region=0 zoom=2"), ("P1_PAUSE_VIEW", "zoom region=0 zoom=1"),
               ("P1_PAUSE_MARK", "reticle set=1 region=0"), ("P1_PAUSE_MARK", "reticle set=0"),
               ("P1_PAUSE_MARK", "reticle set=1 region=0"), ("P1_PAUSE_VIEW", "options .*modal=1"),
               ("P1_PAUSE_OPTION", "text_delay=2 bgm=3"), ("P1_PAUSE_OPTION", "text_delay=3 bgm=3"),
               ("P1_PAUSE_VIEW", "options-closed"), ("P1_PAUSE_CLOSED", "text_delay=3 bgm=3 seq_volume=28287 marker=1,0")],
         not_expect=[("P1_PAUSE_VIEW", "zoom region=0 zoom=3")],
         pictures=[118, 128, 148, 168, 188, 208, 238, 258, 273, 283, 318, 335, 345]),
    dict(name="mouse", frames=320, rows=START + OPEN + [
        mouse(230, "next"), mouse(232, "next", 2),           # region 1
        mouse(250, "prev"), mouse(252, "prev", 2),           # region 0
        mouse(270, "centre"), mouse(272, "centre", 0, 1),    # the wheel: 1.5x
        mouse(290, "centre", 0, -1),                         # 1x
        mouse(300, "zin"), mouse(302, "zin", 2),             # the + button: 1.5x
        mouse(330, "centre"), mouse(332, "centre", 18),      # a drag of the map
        mouse(336, "centre-30-20"), mouse(340, "centre-60-30"), mouse(344, "centre-60-30", 32),
        mouse(360, "centre+20+10"), mouse(362, "centre+20+10", 4),  # a right click: the marker there
        mouse(380, "centre+20+10", 4),                              # on it again: taken away
        mouse(400, "centre+20+10", 4),                              # and back
        mouse(420, "opt"), mouse(422, "opt", 2),             # Options
        mouse(440, "sound4"), mouse(442, "sound4", 2),       # the music: loudest
        mouse(460, "text2"), mouse(462, "text2", 18), mouse(466, "text1"), mouse(470, "text0"),
        mouse(474, "text0", 32),                             # the text speed's knob dragged to the slowest
        mouse(490, "back"), mouse(492, "back", 2),           # Back
        mouse(520, "close"), mouse(522, "close", 2),         # Close: the game's Start
        mouse(560, "close", 8),
    ], expect=[("P1_PAUSE_OPEN", "region=0"), ("P1_PAUSE_VIEW", "region region=1"), ("P1_PAUSE_VIEW", "region region=0"),
               ("P1_PAUSE_VIEW", "zoom region=0 zoom=1"), ("P1_PAUSE_VIEW", "zoom region=0 zoom=0"),
               ("P1_PAUSE_VIEW", "zoom region=0 zoom=1"), ("P1_PAUSE_VIEW", "drag region=0 zoom=1"),
               ("P1_PAUSE_MARK", "pointer set=1 region=0"), ("P1_PAUSE_MARK", "pointer set=0"),
               ("P1_PAUSE_MARK", "pointer set=1 region=0"), ("P1_PAUSE_VIEW", "options .*modal=1"),
               ("P1_PAUSE_OPTION", "bgm=4"), ("P1_PAUSE_OPTION", "text_delay=4"), ("P1_PAUSE_VIEW", "options-closed"),
               ("P1_PAUSE_PRESS", "buttons=1000 why=close"), ("P1_PAUSE_CLOSED", "text_delay=4 bgm=4 seq_volume=32767")],
         pictures=[118, 138, 152, 160, 168, 175, 185, 195, 205, 225, 240, 250]),
    dict(name="esc", frames=180, rows=START + [
        "spellkey 200 0x1000",   # Esc: the menu opens (Start)
        "spellkey 240 0x2000",   # O: Options
        "spellkey 260 0x1000",   # Esc: Options closed
        "spellkey 280 0x1000",   # Esc: the menu closes (Start)
    ], expect=[("P1_PAUSE_PRESS", "buttons=1000 why=esc-open"), ("P1_PAUSE_OPEN", "region=0"),
               ("P1_PAUSE_VIEW", "options .*modal=1"), ("P1_PAUSE_VIEW", "options-closed"),
               ("P1_PAUSE_PRESS", "buttons=1000 why=close"), ("P1_PAUSE_CLOSED", "")],
         pictures=[125, 135]),
    dict(name="compass", frames=330, marker="0 155 60", rows=START + [
        "mouse 200 120 0 0 0",                       # the view turned (the mouse camera): the dot moves round
        "300 0x1000 0 0", "304 0 0 0",               # the menu: region 1, the marker there (the cross)
        "330 0x0010 0 0", "334 0 0 0",
        "350 0x8000 0 0", "354 0 0 0",
        "380 0x1000 0 0", "384 0 0 0",               # closed: in region 0 the dot is gone
    ], expect=[("P1_PAUSE_COMPASS", "shown=1 region=0 marker=416,-728"), ("P1_PAUSE_COMPASS", "shown=1"),
               ("P1_PAUSE_MARK", "reticle set=1 region=1"), ("P1_PAUSE_CLOSED", "marker=1,1"),
               ("P1_PAUSE_COMPASS", "shown=0")],
         pictures=[90, 110, 140, 200]),
    # the bars: the quick save's stats changed (make_data); the menu, then Options (the sliders at the save's options)
    dict(name="bars-full", frames=180, rows=START + OPEN + ["spellkey 240 0x2000"], pictures=[118, 150],
         stats=dict(hp="max", mp="max", defense=255, agility=255, elements=(50, 50, 50, 50), level=97, exp=209103, text_delay=0, bgm=4),
         expect=[("P1_PAUSE_OPEN", "region=0"), ("P1_PAUSE_VIEW", "options .*modal=1")], check="bars"),
    dict(name="bars-empty", frames=180, rows=START + OPEN + ["spellkey 240 0x2000"], pictures=[118, 150],
         stats=dict(hp=1, mp=0, defense=0, agility=0, elements=(0, 0, 0, 0), level=0, exp=0, text_delay=4, bgm=0),
         expect=[("P1_PAUSE_OPEN", "region=0"), ("P1_PAUSE_VIEW", "options .*modal=1")], check="bars"),
    dict(name="bars-over", frames=180, rows=START + OPEN + ["spellkey 240 0x2000"], pictures=[118, 150],
         stats=dict(hp="max+20", mp="half", defense=300, agility=128, elements=(99, 25, 49, 1), level=0, exp=5, text_delay=2, bgm=2),
         expect=[("P1_PAUSE_OPEN", "region=0"), ("P1_PAUSE_VIEW", "options .*modal=1")], check="bars"),
    # the Options dialog's words (the English pictures): the same texels as the game's own Options page draws, every
    # row, at 1x and at 4x with MSAA; the four choices whole (check_labels)
    dict(name="labels", frames=180, rows=START + OPEN + ["spellkey 240 0x2000"], pictures=[150],
         expect=[("P1_PAUSE_VIEW", "options .*modal=1")], check="labels"),
    dict(name="persist", frames=180, quick_save=330, rows=START + OPEN + [
        "230 0x8000 0 0", "234 0 0 0",   # the marker at the cross
        "260 0x1000 0 0", "264 0 0 0",   # closed (F2 at VI 330 saves)
    ], expect=[("P1_PAUSE_MARK", "reticle set=1 region=0"), ("P1_PAUSE_SAVE", "write slot=0 .* marker=1,0")],
         then=dict(frames=150, rows=START + OPEN,
                   expect=[("P1_PAUSE_SAVE", "read .* result=1 marker=1,0"), ("P1_PAUSE_OPEN", "marker=1,0")]),
         pictures=[]),
]


def fnv(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def save_key(q: bytes) -> int:
    """the key the game hashes (pause_menu.c Port_PauseMenuSaveEvent, as the spell UI's): play time, day, map,
    sub-area as 32-bit little-endian numbers, then the 0xC8 bytes of the SaveGameStateBlock"""
    rec = bytes(q[RECORD:])
    play, day = struct.unpack(">HH", rec[0x38:0x3C])
    mapid, sub = struct.unpack(">ii", rec[0x3C:0x44])
    return fnv(struct.pack("<IIii", play, day, mapid, sub) + rec[0x80:0x80 + 0xC8])


def set_stats(q: bytes, st) -> bytes:
    """the quick save with the player's stats changed (the record's SaveHeader: ActorStats, big-endian as on the Pak)
    and its hash made good again (tools/quicksave.py)"""
    b = bytearray(q)
    max_hp, max_mp = struct.unpack_from(">H", b, RECORD + 6)[0], struct.unpack_from(">H", b, RECORD + 0xA)[0]

    def value(v, mx):
        return {"max": mx, "half": mx // 2, "max+20": mx + 20}[v] if isinstance(v, str) else v
    struct.pack_into(">H", b, RECORD + 4, value(st["hp"], max_hp))
    struct.pack_into(">H", b, RECORD + 8, value(st["mp"], max_mp))
    struct.pack_into(">HH", b, RECORD + 0xC, st["agility"], st["defense"])
    struct.pack_into(">I", b, RECORD + 0x10, st["exp"])
    b[RECORD + 0x34] = st["level"]
    b[RECORD + 0x24:RECORD + 0x28] = bytes(st["elements"])
    b[RECORD + 0x62], b[RECORD + 0x63] = st["text_delay"], st["bgm"]
    struct.pack_into("<Q", b, 16, fnv(bytes(b[0x18:])))
    return bytes(b)


def make_data(src: Path, dst: Path, marker=None, visited=0x3F, stats=None):
    shutil.copytree(src, dst, dirs_exist_ok=True)
    q = (dst / "quicksaves/quick0.elq").read_bytes()
    if q[:8] != b"ELTQUICK":
        raise SystemExit(f"{src}: not a quick save")
    if stats:
        q = set_stats(q, stats)
        (dst / "quicksaves/quick0.elq").write_bytes(q)
    (dst / "mapmarker").mkdir(exist_ok=True)
    text = f"ELTMAPMARK 1\nvisited {visited:x}\n" + (f"marker {marker}\n" if marker else "")
    (dst / f"mapmarker/save_{save_key(q):016x}.mark").write_text(text)


def run(case, args, kind, work: Path, saves: Path, rows=None, data_from=None, frames=None, extra_env=None, label=None):
    d = work / f"{case['name']}-{label or kind}"
    if d.exists():
        shutil.rmtree(d)
    (d / "data").mkdir(parents=True)
    if data_from:
        shutil.copytree(data_from, d / "data", dirs_exist_ok=True)
    else:
        make_data(saves, d / "data", case.get("marker"), stats=case.get("stats"))
    (d / "in.input").write_text(script(rows or case["rows"], kind if kind in LAYOUT else "43"))
    env = {k: v for k, v in os.environ.items() if not k.startswith("ELTALE_")}
    env.update({"ELTALE_DATA_DIR": str(d / "data"), "ELTALE_QUICK_LOAD_AT": "100", "ELTALE_LANG": "en",
                "ELTALE_PAUSE_MENU": "1", "ELTALE_PAUSE_LOG": "1"})
    env.update(extra_env or {})
    cmd = [str(args.exe), "--headless", "--fast", "--frames", str(frames or case["frames"]), "--input", str(d / "in.input")]
    env["ELTALE_WIDESCREEN"] = "16:9" if kind == "169" else "off"
    if kind in LAYOUT and case["pictures"]:
        (d / "pictures").mkdir()
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


def lines_of(out):
    return [l for l in out.splitlines() if l.startswith("P1_PAUSE_")]


def check_expect(expect, out, label, not_expect=()):
    lines = lines_of(out)
    problems, at = [], 0
    for tag, pattern in expect:
        while at < len(lines) and not (lines[at].startswith(tag + " ") and re.search(pattern, lines[at])):
            at += 1
        if at == len(lines):
            problems.append(f"{label}: no {tag} matching '{pattern}' (in order)")
            break
        at += 1
    for tag, pattern in not_expect:
        if any(l.startswith(tag + " ") and re.search(pattern, l) for l in lines):
            problems.append(f"{label}: unexpected {tag} matching '{pattern}'")
    if "P1_TEST_EXIT" in out or "P1_OS_OK" not in out:
        problems.append(f"{label}: the run did not finish")
    return problems


def is_fill(p):
    """a pixel of a bar's fill (sprite 0x33: yellow to red, (247,189,0) .. (214,0,24))"""
    r, g, b = p[:3]
    return r >= 190 and b <= 70 and g <= 205


def check_bars(pic: Path, kind, label, full: bool, empty: bool):
    """no fill pixel outside a bar frame's inside (the frame w x 5 at x, y: the inside x+1..x+w-2, y+1..y+3; looked at
    from 3 left of the frame to 4 past it, 2 above to 4 below); full: the inside's last column is fill; empty: no fill
    inside (but HP, at 1). And the Options button's label (its dark ink) stays 3 clear of the button's right edge."""
    from PIL import Image
    im = Image.open(pic).convert("RGB")
    px = im.load()
    L = LAYOUT[kind]
    cx = L["fx"] + L["col_x"]
    bars = [(f"stat{i}", cx + L["stat_bar_x"], FY + 36 + 20 * i + 12, L["stat_bar_w"]) for i in range(4)]
    bars += [(f"elem{e}", cx + L["elem_bar_x"], FY + 132 + 16 * e + 5, L["elem_bar_w"]) for e in range(4)]
    problems = []
    for name, x, y, w in bars:
        out = [(i, j) for j in range(y - 2, y + 9) for i in range(x - 3, x + w + 5)
               if is_fill(px[i, j]) and not (x + 1 <= i <= x + w - 2 and y + 1 <= j <= y + 3)]
        if out:
            problems.append(f"{label} {pic.name}: {name}: {len(out)} fill pixels outside the frame's inside, e.g. {out[:4]}")
        if full and not all(is_fill(px[x + w - 2, j]) for j in range(y + 1, y + 4)):
            problems.append(f"{label} {pic.name}: {name}: full, but the inside's last column is not fill")
        if empty and name != "stat0" and any(is_fill(px[i, j]) for j in range(y + 1, y + 4) for i in range(x + 1, x + w - 1)):
            problems.append(f"{label} {pic.name}: {name}: empty, but there is fill inside")
    ox, ow = L["fx"] + L["opt_x"], L["opt_w"]
    ink = [i for j in range(FY + 7, FY + 16) for i in range(ox + 2, ox + ow - 1) if sum(px[i, j][:3]) < 330]
    if not ink or max(ink) > ox + ow - 4:
        problems.append(f"{label} {pic.name}: the Options label is not clear of the button's right edge "
                        f"(ink to {max(ink) if ink else None}, edge {ox + ow - 1})")
    return problems


def ring(pic: Path, box):
    """the pixels in a 3-wide band round box (x0, y0, x1, y1)"""
    from PIL import Image
    im = Image.open(pic).convert("RGB")
    px = im.load()
    x0, y0, x1, y1 = box
    return [px[i, j] for j in range(y0 - 3, y1 + 3) for i in range(x0 - 3, x1 + 3)
            if not (x0 <= i < x1 and y0 <= j < y1)]


def check_places(work: Path, kind):
    """the Next-spirit orb (its 32 x 32) and the Options sliders (the track and the knobs' row): the pixels round them
    are the same in all the bars cases (whatever the fill or the place, nothing is drawn past them)"""
    L = LAYOUT[kind]
    cx = L["fx"] + L["col_x"]
    ox = cx + L["orb_x"] + (L["orb_w"] - 32) // 2
    mx = (L["W"] - 240) // 2
    places = {"orb": (118, (ox, FY + 148, ox + 32, FY + 180))}
    for row, ty in enumerate((32, 78)):  # the track (84 x 7 at dialog x 101) and the knobs (8 x 11 from ty - 5)
        places[f"slider{row}"] = (150, (mx + 98, 66 + ty - 6, mx + 188, 66 + ty + 8))
    problems = []
    for name, (frame, box) in places.items():
        rings = [ring(work / f"{c}-{kind}/pictures/frame_{frame}.png", box) for c in ("bars-full", "bars-empty", "bars-over")]
        # (a step or two of colour allowed: the dialog's fade-in blends over the menu behind, which differs)
        bad = sum(max(abs(u - v) for u, v in zip(p, q)) > 8 for r in rings[1:] for p, q in zip(rings[0], r))
        if bad:
            problems.append(f"{kind}: {name}: {bad} pixels round it differ between the cases")
    return problems


# the Options words: (en_images name, the unified dialog's place from its x / y, the game's page's from its x / y)
LABELS = {"menu_sprite_86": ((68, 30), (108, 60)), "menu_sprite_87": ((193, 30), (233, 60)),
          "menu_sprite_42": ((68, 77), (108, 107)), "menu_sprite_41": ((193, 77), (233, 107)),
          "menu_sprite_34": ((9, 29), (49, 59)), "menu_sprite_35": ((13, 74), (53, 104))}
CHOICES = ("menu_sprite_86", "menu_sprite_87", "menu_sprite_42", "menu_sprite_41")
GOLD = lambda c: c[0] > c[2] + 60 and c[0] > 120


def en_pictures(exe: Path, rom: Path):
    """the English pictures the build put in (OUT/en_images.inc next to the exe), in the menu sprites' palette"""
    sys.path.insert(0, str(ROOT / "tools/images"))
    import build_en_images as B
    inc = (exe.parent / "en_images.inc").read_text()
    data = rom.read_bytes()
    out = {}
    for name in LABELS:
        _, fmt, w, h, stride, shown, tlut, _ = B.TARGETS[name]
        pal = B.palette(data, tlut, 256)
        m = re.search(r"en_img_%s\[\d+\] = \{([^}]*)\}" % name, inc)
        idx = [int(v) for v in m.group(1).split(",") if v.strip()]
        out[name] = [[pal[idx[y * stride + x]] for x in range(shown)] for y in range(h)]
    return out


def labels_runs(args, work: Path, saves: Path):
    """the game's own Options page (the switch off: Start, then Z slides to it) at 4:3 and 16:9; the unified dialog
    at 16:9 dumped at scale 4 with MSAA 4"""
    page = dict(name="labels-page", frames=200, rows=START + OPEN + ["240 0x2000 0 0", "244 0 0 0"], pictures=[175])
    for kind in ("43", "169"):
        run(page, args, kind, work, saves, extra_env={"ELTALE_PAUSE_MENU": "0"})
    case = next(c for c in CASES if c["name"] == "labels")
    run(case, args, "169", work, saves, extra_env={"ELTALE_DUMP_SCALE": "4", "ELTALE_DUMP_MSAA": "4"}, label="169-s4m4")


def check_labels(args, work: Path, kind, pics):
    """every texel of the Options dialog's words drawn, all their rows (nothing cut by a rectangle, a tile or a
    scissor): the same as the build's picture and as the game's page draws it; at 16:9 also each texel a whole 4 x 4
    block at scale 4 with MSAA. And the four choices whole in the picture: each letter's shadow (one right, one down)
    inside it"""
    from PIL import Image
    W = LAYOUT[kind]["W"]
    mx, ox = (W - 240) // 2, (W - 320) // 2
    shots = [("dialog", work / f"labels-{kind}/pictures/frame_150.png", 1, lambda a, b: (mx + a[0], 66 + a[1])),
             ("page", work / f"labels-page-{kind}/pictures/frame_175.png", 1, lambda a, b: (ox + b[0], b[1]))]
    if kind == "169":
        shots.append(("dialog 4x", work / "labels-169-s4m4/pictures/frame_150.png", 4, shots[0][3]))
    problems = []
    for what, path, k, at in shots:
        px = Image.open(path).convert("RGB").load()
        for name, (a, b) in LABELS.items():
            x0, y0 = at(a, b)
            pic = pics[name]
            bad, rows = 0, set()
            for y, line in enumerate(pic):
                for x, c in enumerate(line):
                    if c[3] < 128:
                        continue
                    block = {px[(x0 + x) * k + i, (y0 + y) * k + j] for i in range(k) for j in range(k)}
                    if len(block) > 1 or max(abs(u - v) for u, v in zip(next(iter(block)), c)) > 8:
                        bad += 1
                        rows.add(y)
            if bad:
                problems.append(f"{kind}: {what}: {name}: {bad} texels not drawn as the picture's (rows {sorted(rows)})")
    for name in CHOICES if kind == "43" else ():  # (the pictures: once)
        pic = pics[name]
        H, Wd = len(pic), len(pic[0])
        ink = {(x, y) for y in range(H) for x in range(Wd) if pic[y][x][3] >= 128 and GOLD(pic[y][x])}
        rows = {y for _, y in ink}
        lost = [(x, y) for x, y in ink if (x + 1, y + 1) not in ink
                and not (x + 1 < Wd and y + 1 < H and pic[y + 1][x + 1][3] >= 128)]
        if not ink or lost:
            problems.append(f"{name}: letters cut at the picture's edge or without their shadow ({len(lost)} texels)")
        elif max(rows) - min(rows) + 2 > H or max(rows) + 1 >= H:
            problems.append(f"{name}: the letters and their shadow do not fit its {H} rows")
    return problems


def check_compass(out, label):
    """every shown dot: the bearing is atan2(dx, dz) from the player to the marker, the angle is it less the camera's
    yaw (wrapped), the dot 18 out at that angle (x = 18 sin, y = 18 cos: down the screen when the marker is behind)"""
    problems, yaws = [], set()
    for l in lines_of(out):
        m = re.match(r"P1_PAUSE_COMPASS .*shown=1 region=\d+ marker=(\S+),(\S+) player=(\S+),(\S+) bearing=(\S+) yaw=(\S+) "
                     r"angle=(\S+) dot=(\S+),(\S+)", l)
        if not m:
            continue
        mx, mz, px, pz, bearing, yaw, ang, dx, dy = map(float, m.groups())
        yaws.add(round(yaw, 2))
        want_b = math.atan2(mx - px, mz - pz)
        want_a = (want_b - yaw + math.pi) % (2 * math.pi) - math.pi
        if abs(bearing - want_b) > 0.02 or abs(((ang - want_a) + math.pi) % (2 * math.pi) - math.pi) > 0.02:
            problems.append(f"{label}: compass bearing/angle {bearing}/{ang}, wanted {want_b:.3f}/{want_a:.3f}: {l}")
        if abs(dx - 18 * math.sin(ang)) > 0.6 or abs(dy - 18 * math.cos(ang)) > 0.6:
            problems.append(f"{label}: the dot at {dx},{dy} is not 18 out at {ang}: {l}")
    if len(yaws) < 2:
        problems.append(f"{label}: the dot was not seen with the view turned (yaws {sorted(yaws)})")
    return problems


# The host's whole path in a hidden window (ELTALE_INPUT_TEST: SDL's own events by pad read, nothing reaches the
# desktop; 640x480, 4:3: a picture unit is 2 window pixels): Esc opens the menu, O opens Options, Esc closes them, a
# right click on the map puts the marker there, a click on Close closes the menu (Start).
WINDOW_EVENTS = ["3 key_down Return", "5 key_up Return",
                 "100 key_down Escape", "102 key_up Escape",
                 "120 key_down O", "122 key_up O",
                 "140 key_down Escape", "142 key_up Escape",
                 "150 pointer 178 244", "152 mouse_down right", "154 mouse_up right",
                 "170 pointer 598 46", "172 mouse_down left", "174 mouse_up left"]
WINDOW_EXPECT = [("P1_PAUSE_PRESS", "why=esc-open"), ("P1_PAUSE_OPEN", "width=320 region=0"),
                 ("P1_PAUSE_VIEW", "options .*modal=1"), ("P1_PAUSE_VIEW", "options-closed"),
                 ("P1_PAUSE_MARK", "pointer set=1 region=0"), ("P1_PAUSE_PRESS", "why=close"), ("P1_PAUSE_CLOSED", "marker=1,0")]


def window_case(args, work: Path, saves: Path):
    d = work / "window"
    if d.exists():
        shutil.rmtree(d)
    make_data(saves, d)
    (d / "settings.json").write_text('{"version": 2, "audio": {"volume": 0}, "game": {"language": "en"}}\n')
    (d / "in.events").write_text("\n".join(WINDOW_EVENTS) + "\n")
    env = {k: v for k, v in os.environ.items() if not k.startswith("ELTALE_")}
    env.update(ELTALE_HIDDEN_WINDOW="1", ELTALE_DATA_DIR=str(d), ELTALE_INPUT_TEST=str(d / "in.events"),
               ELTALE_QUICK_LOAD_AT="100", ELTALE_PAUSE_LOG="1", ELTALE_WINDOW_SIZE="640x480", ELTALE_WIDESCREEN="off")
    p = subprocess.run([str(args.exe), "--window", "--fast", "--frames", "220", "--rom", str(args.rom)], env=env, cwd=d,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout.decode("utf-8", "replace")
    (d / "log.txt").write_text(out)
    return check_expect(WINDOW_EXPECT, out, "window")


def same_case(args, work, saves):
    """a walk in the field without the menu: the switch on and off end with the same quick save and sound"""
    rows = START + ["200 0 0 80", "300 0 40 80", "360 0 0 0"]
    case = dict(name="same", frames=260, rows=rows, pictures=[])
    res = []
    for on in ("1", "0"):
        out, d = run(case, args, "43", work, saves, extra_env={"ELTALE_PAUSE_MENU": on, "ELTALE_QUICK_SAVE_AT": "500"},
                     label=f"on{on}")
        qs = d / "data/quicksaves/quick0.elq"
        audio = re.search(r"P1_AUDIO .*hash=(\w+)", out)
        res.append((qs.read_bytes() if qs.is_file() else None, audio.group(1) if audio else None))
    problems = []
    if res[0][0] is None or res[0][0] != res[1][0]:
        problems.append("same: the quick saves differ with the switch on and off")
    if res[0][1] != res[1][1]:
        problems.append(f"same: the sound differs: {res[0][1]} / {res[1][1]}")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--saves", type=Path, default=Path("C:/code/eltale/out/bugsaves"))
    ap.add_argument("--out", type=Path, default=ROOT / "out/pause_menu_test")
    ap.add_argument("--only", help="cases, comma-separated")
    args = ap.parse_args()
    args.exe = args.exe.resolve()
    args.rom = args.rom.resolve()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    saves = args.saves / "battle-stuck"
    if not (saves / "quicksaves/quick0.elq").is_file():
        raise SystemExit(f"pause_menu_test: needs the quick save {saves} (--saves)")
    only = args.only.split(",") if args.only else None
    failed = 0
    for case in CASES:
        if only and case["name"] not in only:
            continue
        problems = []
        extra = {"ELTALE_QUICK_SAVE_AT": str(case["quick_save"])} if case.get("quick_save") else None
        o43, _ = run(case, args, "43", work, saves, extra_env=extra)
        o169, d169 = run(case, args, "169", work, saves, extra_env=extra)
        problems += check_expect(case["expect"], o43, "4:3", case.get("not_expect", ()))
        problems += check_expect(case["expect"], o169, "16:9", case.get("not_expect", ()))
        # (the view's middle, and so the cross's map point, depends on the map's width: 214 or 150)
        strip = lambda o: [re.sub(r"vi=\d+ |width=\d+ |centre=\S+ |map=\S+ |world=\S+|marker=1,(\d),\S+",
                                  lambda m: f"marker=1,{m.group(1)}" if m.group(1) else "", l)
                           for l in lines_of(o) if not l.startswith("P1_PAUSE_COMPASS")]
        if case["name"] not in ("mouse",) and strip(o43) != strip(o169):
            problems.append("4:3 and 16:9 print different menu lines")
        if case["name"] == "compass":
            problems += check_compass(o43, "4:3") + check_compass(o169, "16:9")
        if case.get("check") == "bars":
            for kind in ("43", "169"):
                problems += check_bars(work / f"{case['name']}-{kind}/pictures/frame_{case['pictures'][0]}.png", kind,
                                       "4:3" if kind == "43" else "16:9", case["name"] == "bars-full",
                                       case["name"] == "bars-empty")
            if case["name"] == "bars-over":
                problems += check_places(work, "43") + check_places(work, "169")
        if case.get("check") == "labels":
            labels_runs(args, work, saves)
            pics = en_pictures(args.exe, args.rom)
            problems += check_labels(args, work, "43", pics) + check_labels(args, work, "169", pics)
        if case.get("then"):
            t = case["then"]
            ot, _ = run(case, args, "43", work, saves, rows=t["rows"], data_from=d169 / "data", frames=t["frames"], label="then")
            problems += check_expect(t["expect"], ot, "then")
        failed += bool(problems)
        print(f"PAUSE_MENU {case['name']} {'OK' if not problems else 'FAIL'}")
        for pr in problems:
            print("    !", pr)
    if not only or "window" in only:
        problems = window_case(args, work, saves)
        failed += bool(problems)
        print(f"PAUSE_MENU window {'OK' if not problems else 'FAIL'}")
        for pr in problems:
            print("    !", pr)
    if not only or "same" in only:
        problems = same_case(args, work, saves)
        failed += bool(problems)
        print(f"PAUSE_MENU same {'OK' if not problems else 'FAIL'}")
        for pr in problems:
            print("    !", pr)
    print("PAUSE_MENU_TEST " + ("OK" if not failed else f"FAIL ({failed})"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
