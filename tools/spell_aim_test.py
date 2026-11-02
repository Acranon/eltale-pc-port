#!/usr/bin/env python3
"""Click to aim spells, headless (port/p0/spell_aim.c; docs/SPELL_AIM.md).

    python tools/spell_aim_test.py --exe out/p1-windows/eltale-p1.exe --rom eltalemonsters.jp.z64
        [--saves C:/code/eltale/out/bugsaves] [--out out/spell_aim_test] [--only NAME,...] [--verify [SPELL ...]]
        [--tubes 0|1] [--sheet]

Each case is a battle from battle-stuck's quick save with every spell learned (spirit levels 48, MP 60: a copy, only
numbers changed, as tools/spell_ui_test.py makes its saves; ELTALE_BATTLE_LOG=2 starts the battle). Its input script
builds a spell with the spell UI's keys, moves the pointer onto the battlefield (the panel folds up to its plate and the
camera rises), points at a monster or at the ground and clicks. Where a monster is on the raised view comes from a probe
run of the same script without the click (ELTALE_SPELL_AIM_LOG prints P1_AIM_SCREEN: each monster's place on the
picture, in CENTRE units; the run is deterministic, so the real run finds it there). The game prints:
- P1_AIM_CLICK: the preview at the click (the monsters that glow: predicted=);
- P1_AIM_CAST: the copy of the spell code run again for the facing the cast was given (Player_StartCastSpell);
- P1_AIM_HITS: the monsters the game's own effects touched, and whether that is what the preview said;
- P1_AIM_FX (log 2): the game's effects each tick, to check the preview's shapes against.
A case passes when what it expects is there (the monster hit, the area's place, the margin round a radius), the
preview equals the game's hits, and 4:3 and 16:9 (pictures: OUT/<case>-43|169/pictures, 3x) print the same aim lines and
the same game sound.

The tunnels and cones (docs/SPELL_AIM.md "Tunnels", "Cones"; cases t_*, cone_*, *_flight): every monster the game hits
is inside the tunnel the preview draws (P1_AIM_TUNNEL) and none it misses, each hit (P1_AIM_HITAT) came from a live
place of it, a Fire Ball hit is inside its cone (P1_AIM_CONE), and a rock hits in the air where its flight's tunnel says.
A target "b:DEG" points at the ground at that bearing from the player (P1_AIM_COMPASS).

The 3D paths (docs/SPELL_AIM.md "3D paths"; on unless --tubes 0): with every tunnel check, every monster's middle is
inside the drawn 3D volume (P1_AIM_TUBE: the ellipsoids round the places where the test runs) exactly when the game
hits it, and each hit's monster middle is inside the ellipsoid of the place it was hit from (P1_AIM_HITAT); for the
rocks, the same for the flight before the landing (Rock Lv1 passes over e1, Lv3 hits it in the air). `side_*`: every
moving spell from a low side view (the mouse camera turned, the raised camera off), pictures only. `tubes_off`: the
lob with the 3D paths off: the same aim lines, other pictures. --tubes 0 runs every case with them off (the ground-only
look: its pictures equal the ones before the 3D paths). --sheet: a contact sheet of the pictures (OUT/sheet).

Also: `lift`: the same aim with the camera raised and not (ELTALE_SPELL_AIM_CAMERA=0): the same game (aim lines, sound);
`pick`: the pointer on a monster's body, just off it and back (the pick and its hysteresis);
`idle`: aim on with no pointer against aim off: the same last frame list and sound.

--verify: one run per aimable spell (every kind), cast at a monster: each of the game's effects, every tick, against the
copy's path (P1_AIM_SIMPATH, log 3), and the hits. Prints the worst distance (0.00: the copy is exact; a monster killed
by an earlier bolt changes what follows) and the ends of each path (docs/SPELL_AIM.md "Verified in play").
"""

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spell_ui_test as sut  # noqa: E402 (the saves: make_save)

ROOT = Path(__file__).resolve().parent.parent

START = ["5 0x1000 0 0", "7 0 0 0"]
WALK = ["120 0 0 80", "200 0 0 0"]  # a battle (ELTALE_BATTLE_LOG=2); the player's first turn from VI ~360
NEUTRAL = "300 60"                  # a battlefield point (CENTRE): the panel folds, the camera rises
ELEMENT_KEY = {0: 0x1, 1: 0x2, 2: 0x4, 3: 0x8}
TUBES = "1"                         # ELTALE_SPELL_AIM_TUBES for every run (--tubes)


def keys(seq, at):
    return [f"spellkey {at + 6 * k} {ELEMENT_KEY[e]:#x}" for k, e in enumerate(seq)]


def turn2(stick, ticks):
    """turn 1: a few steps toward the monsters and Water Pillar on empty ground (the turn ends); turn 2: a walk"""
    return ["370 0 20 80", "430 0 0 0"] + keys([2], 440) + [
        "mouse 446 0 0 0 0 {c:300} 40", "mouse 470 0 0 0 2", "mouse 480 0 0 0 8",
        f"880 0 {stick}", f"{880 + 2 * ticks} 0 0 0"]


# spells: (element sequence, id)
WATER_PILLAR3 = ([2, 2, 2], "2.4")
WATER_PILLAR2 = ([2, 2], "2.1")
WEAKNESS = ([1, 3], "1.4")
HOT_STEAM = ([0, 2], "0.4")
WALKING_WATER = ([2, 0, 2], "2.12")
FIRE_BALL3 = ([0, 0, 0], "0.5")
HOMING_ARROW2 = ([0, 3, 3], "0.9")
ROCK3 = ([1, 1, 1], "1.5")
FIRE_BOMB = ([0, 0, 1], "0.10")
ROLLING_ROCK2 = ([1, 0, 0], "1.13")
MAGMA_BALL = ([0, 1, 3], "0.12")
ROCK_COMBO = ([1, 0, 1], "1.8")
WIDE_WEAKNESS = ([1, 3, 3], "1.14")
WIND_CUTTER3 = ([3, 3, 3], "3.5")
ROCK1 = ([1], "1.0")
ROCK2 = ([1, 1], "1.1")
CYCLONE = ([3, 1, 3], "3.10")
LARGE_CUTTER = ([3, 0, 3], "3.6")
ROLLING_ROCK1 = ([1, 0], "1.3")
FIRE_BALL1 = ([0], "0.0")
FIRE_BALL2 = ([0, 0], "0.1")
WIND_CUTTER1 = ([3], "3.0")
WIND_CUTTER2 = ([3, 3], "3.1")
HOMING_ARROW1 = ([0, 3], "0.3")

# Bearings (degrees, as Math_Atan2: sin x, cos z) from the player at the battle's start (398.3, -1629.4): the far
# monster e0 (72.6 away) at 35, the near one e1 (63.4 away) at 57.3. A target "b:DEG" points at the ground 40 away at
# that bearing (P1_AIM_COMPASS), so the facing is DEG.


def case(name, spell, target, first=370, pre=(), click=True, expect=(), pictures=None, env=None, extra=(), frames=None,
         turn=None):
    """target: "e0"/"e1" (a monster), "g:DX,DY" (the ground: the player's place on the picture plus DX, DY), "b:DEG" (the
    ground at that bearing from the player)"""
    seq, sid = spell
    t_vi = first + 6 * len(seq) + 40       # pointing at the target (the camera has risen by then)
    c_vi = t_vi + 10
    rows = START + WALK + list(pre) + keys(seq, first)
    if turn:
        # the mouse camera turned (dx, dy counts: 0.006 and 0.004 radians each) and zoomed (the wheel) before aiming starts
        rows.append(f"mouse {first + 6 * len(seq) + 2} {turn[0]} {turn[1]} {turn[2]} 0 {{c:300}} 60")
    rows += [f"mouse {first + 6 * len(seq) + 6} 0 0 0 0 {{c:300}} 60", f"mouse {t_vi} 0 0 0 0 {{T}}"]
    if click:
        rows.append(f"mouse {c_vi} 0 0 0 2")
    rows += list(extra)
    return dict(name=name, spell=sid, target=target, rows=rows, t_vi=t_vi, c_vi=c_vi, click=click, expect=list(expect),
                pictures=pictures or [t_vi + 6, c_vi + 26], env=env or {}, frames=frames or c_vi // 2 + 160)


CASES = [
    # Water Pillar Lv3 in two directions from the same place (turn 2, the first monster 19 away): at it, then away
    case("pillar_at", WATER_PILLAR3, "e0", first=950, pre=turn2("-20 80", 20),
         expect=[("hit", "e0"), ("fx_at_preview", None)]),
    case("pillar_away", WATER_PILLAR3, "g:-45,10", first=950, pre=turn2("-20 80", 20),
         expect=[("miss", "e0"), ("fx_at_preview", None), ("differs_from", "pillar_at")]),
    # an auto-target spell (Weakness Lv1): the monster further away clicked, not the one the facing would pick
    case("auto_far", WEAKNESS, "e0", expect=[("hit", "e0"), ("not_hit", "e1"), ("facing_pick_differs", "e0")]),
    # Hot Steam Lv1 (15 round the player): the first monster just inside, then (a longer walk aside) just outside
    case("steam_in", HOT_STEAM, "g:0,-30", first=950, pre=turn2("-20 80", 20), expect=[("hit", "e0"), ("margin", "e0:-2.5:0")]),
    case("steam_out", HOT_STEAM, "g:0,-30", first=950, pre=turn2("20 80", 16), expect=[("miss", "e0"), ("margin", "e0:0:3")]),
    # a line along the ground (Walking Water) at the far monster
    case("line", WALKING_WATER, "e0", expect=[("hit", "e0")]),
    # Fire Ball Lv3: three bolts, each its own target (the facing's monster and the nearest to its 45 degrees)
    case("fireball", FIRE_BALL3, "e0", expect=[("hit", "e0"), ("hit", "e1"), ("casts", 3)]),
    # undo while aiming: Water Pillar Lv2 folded, a right click (Lv1: the panel opens again), aiming again, cast
    case("undo", WATER_PILLAR2, "e1", extra=[],
         pre=[], expect=[("undo", None)]),
    # the game's own spell panel (the spell UI off): the C button, then the aim
    case("noui", ([2], "2.0"), "g:20,-20", env={"ELTALE_SPELL_UI": "0"}, expect=[("ui_off", None)]),
    # pictures of the other kinds (hover only, then the cast)
    case("homing", HOMING_ARROW2, "e0"),
    case("lob", ROCK3, "e0"),
    case("bomb", FIRE_BOMB, "e1"),
    case("sweep", ROLLING_ROCK2, "e1"),
    case("magma", MAGMA_BALL, "e0"),
    case("random", ROCK_COMBO, "g:0,-30"),
    case("arena", WIDE_WEAKNESS, "g:0,-30"),
    case("cutter", WIND_CUTTER3, "e1"),
    # The tunnels (docs/SPELL_AIM.md "Tunnels"): every monster the game hits is inside the drawn tunnel (its middle
    # nearer than the place's disc w plus its radius to a live place) and none it misses is; each hit happened at a live
    # place (P1_AIM_HITAT). Each spell at a monster and at a bearing that passes one by.
    case("t_rock1", ROCK1, "e0", expect=[("tunnel", None)]),
    case("t_rock1_side", ROCK1, "b:46", expect=[("tunnel", None)]),
    case("t_rock2", ROCK2, "e1", expect=[("tunnel", None)]),
    case("t_rock3_side", ROCK3, "b:46", expect=[("tunnel", None)]),
    case("t_bomb", FIRE_BOMB, "b:40", expect=[("tunnel", None)]),
    case("t_water", WALKING_WATER, "b:47", expect=[("tunnel", None)]),
    case("t_cyclone", CYCLONE, "e1", expect=[("tunnel", None)]),
    case("t_cyclone_side", CYCLONE, "b:45", expect=[("tunnel", None)]),
    case("t_cutter", LARGE_CUTTER, "e1", expect=[("tunnel", None)]),
    case("t_magma", MAGMA_BALL, "b:15", expect=[("tunnel", None)]),
    case("t_roll1", ROLLING_ROCK1, "b:20", expect=[("tunnel", None)]),
    case("t_roll2", ROLLING_ROCK2, "b:0", expect=[("tunnel", None)]),
    case("t_fire1", FIRE_BALL1, "e1", expect=[("tunnel", None), ("cone", None)]),
    case("t_fire2", FIRE_BALL2, "b:80", expect=[("tunnel", None), ("cone", None)]),
    case("t_fire3", FIRE_BALL3, "b:46", expect=[("tunnel", None), ("cone", None)]),
    case("t_wind1", WIND_CUTTER1, "e0", expect=[("tunnel", None)]),
    case("t_wind2", WIND_CUTTER2, "b:100", expect=[("tunnel", None)]),
    # (a cutter kills the near monster: the copy, which keeps the monsters alive, has later cutters meet it; the hits
    # up to the kill are checked)
    case("t_wind3", WIND_CUTTER3, "e0", expect=[("tunnel", None)]),
    case("t_arrow1", HOMING_ARROW1, "b:46", expect=[("tunnel", None)]),
    case("t_arrow2", HOMING_ARROW2, "e1", expect=[("tunnel", None)]),
    # Fire Ball's cones: facing 97 the near monster (40 off) is the bolt's: it turns and hits it, well off its straight
    # line and inside its cone; facing 327 (the bolt's monster 68 off, 70 from its start) and 215 (both behind, ~72):
    # outside, missed
    case("cone_in1", FIRE_BALL1, "b:75", expect=[("cone", None), ("hit", "e1"), ("off_line", "e1")]),
    case("cone_out1", FIRE_BALL1, "b:327", expect=[("cone", None), ("miss", "e0"), ("miss", "e1"), ("outside", "e0")]),
    case("cone_in3", FIRE_BALL3, "b:90", expect=[("cone", None), ("hit", "e1"), ("off_line", "e1")]),
    case("cone_out3", FIRE_BALL3, "b:215", expect=[("cone", None), ("miss", "e0"), ("miss", "e1"), ("outside", "e0"),
                                                   ("outside", "e1")]),
    # Rock in flight: Lv3 facing between the monsters (e0 its target, 11 off; e1 11 off the other way, under its flight)
    # and Lv1 the same: whether e1 is hit before the rock lands is what the tunnel shows
    case("rock3_flight", ROCK3, "b:46", expect=[("tunnel", None), ("flight", "e1")]),
    case("rock1_flight", ROCK1, "b:46", expect=[("tunnel", None), ("flight", "e1")]),
]

# The 3D paths from a low side view (docs/SPELL_AIM.md "3D paths"): the raised camera off and the mouse camera turned
# nearly a quarter round to the right, down a little and zoomed out, so the paths are seen across; the pointer where the t_ cases aim, no
# click (the preview). Pictures only (their aim lines are the hover's).
SIDE = {"ELTALE_SPELL_AIM_CAMERA": "0"}
SIDE_TURN = (230, -40, -6)  # (dx, dy, the wheel: zoomed out; the eye stays clear of the walls there)
for _name, _spell, _target in [
        ("rock1", ROCK1, "b:46"), ("rock2", ROCK2, "e1"), ("rock3", ROCK3, "b:46"), ("bomb", FIRE_BOMB, "b:40"),
        ("water", WALKING_WATER, "b:47"), ("cyclone", CYCLONE, "e1"), ("cutter", LARGE_CUTTER, "e1"),
        ("magma", MAGMA_BALL, "b:15"), ("roll1", ROLLING_ROCK1, "b:20"), ("roll2", ROLLING_ROCK2, "b:0"),
        ("fire1", FIRE_BALL1, "e1"), ("fire2", FIRE_BALL2, "b:80"), ("fire3", FIRE_BALL3, "b:46"),
        ("wind1", WIND_CUTTER1, "e0"), ("wind2", WIND_CUTTER2, "b:100"), ("wind3", WIND_CUTTER3, "e0"),
        ("arrow1", HOMING_ARROW1, "b:46"), ("arrow2", HOMING_ARROW2, "e1")]:
    _c = case("side_" + _name, _spell, _target, click=False, env=SIDE, turn=SIDE_TURN)
    _c["pictures"] = [_c["t_vi"] + 6]
    _c["frames"] = _c["t_vi"] // 2 + 20
    _c["side"] = True
    CASES.append(_c)


def undo_rows(c):
    # after the target: a right click (Lv2 -> Lv1: the panel opens), the pointer on the battlefield (it stays open: an
    # undo keeps it until the pointer has moved on it), onto the panel, off it (folds again), to the target, click
    t = c["t_vi"]
    rows = [r for r in c["rows"] if not r.startswith(f"mouse {c['c_vi']} ")]
    rows += [f"mouse {t + 6} 0 0 0 4", f"mouse {t + 16} 0 0 0 0 {{c:300}} 62", f"mouse {t + 24} 0 0 0 0 {{c:160}} 140",
             f"mouse {t + 32} 0 0 0 0 {{c:300}} 62", f"mouse {t + 40} 0 0 0 0 {{T}}", f"mouse {t + 50} 0 0 0 2"]
    return rows


for c in CASES:
    if c["name"] == "undo":
        c["rows"] = undo_rows(c)
        c["c_vi"] = c["t_vi"] + 50
        c["pictures"] = [c["t_vi"] + 2, c["t_vi"] + 20, c["t_vi"] + 28, c["t_vi"] + 48]  # folded, open, on it, folded
        c["frames"] = c["c_vi"] // 2 + 160
    if c["name"] == "noui":
        # the game's C down (water) instead of the spell UI's key
        c["rows"] = [r for r in c["rows"] if not r.startswith("spellkey")] + ["370 0x0004 0 0", "372 0 0 0"]


def substitute(rows, wide, points):
    side = 53 if wide else 0
    out = []
    for r in rows:
        if "{T}" in r:
            r = r.replace("{T}", f"{{c:{points[0]:.0f}}} {points[1]:.0f}")
        r = re.sub(r"\{c:(-?[\d.]+)\}", lambda m: f"{float(m.group(1)) + side:.0f}", r)
        out.append(r)
    return "\n".join(out) + "\n"


def run(c, args, kind, work, saves, rows, frames, pictures=None, env=None):
    d = work / f"{c['name']}-{kind}"
    if d.exists():
        shutil.rmtree(d)
    (d / "data").mkdir(parents=True)
    shutil.copytree(saves, d / "data", dirs_exist_ok=True)
    (d / "in.input").write_text(rows)
    e = {k: v for k, v in os.environ.items() if not k.startswith("ELTALE_")}
    e.update({"ELTALE_DATA_DIR": str(d / "data"), "ELTALE_QUICK_LOAD_AT": "100", "ELTALE_LANG": "en", "ELTALE_BATTLE_LOG": "2",
              "ELTALE_SPELL_UI": "1", "ELTALE_SPELLUI_LOG": "1", "ELTALE_SPELL_AIM": "1", "ELTALE_SPELL_AIM_LOG": "2"})
    e["ELTALE_SPELL_AIM_TUBES"] = TUBES
    e.update(c["env"])
    e.update(env or {})
    cmd = [str(args.exe), "--headless", "--fast", "--frames", str(frames), "--input", str(d / "in.input")]
    if pictures:
        (d / "pictures").mkdir()
        e["ELTALE_WIDESCREEN"] = "16:9" if kind.endswith("169") else "off"
        e["ELTALE_DUMP_FRAMES"] = ",".join(str(f) for f in pictures)
        e["ELTALE_DUMP_SCALE"] = "3"
        cmd += ["--dump-frames", str(d / "pictures")]
    cmd += ["--rom", str(args.rom)]
    p = subprocess.run(cmd, cwd=d, env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = p.stdout.decode("utf-8", "replace")
    (d / "log.txt").write_text(out)
    (d / "err.txt").write_bytes(p.stderr)
    if p.returncode:
        out += f"\nP1_TEST_EXIT {p.returncode}\n"
    return out


def probe(c, args, work, saves, env=None):
    """where the target is on the raised view at the target's read: the last P1_AIM_SCREEN before it"""
    rows = [r for r in c["rows"] if "{T}" not in r and not r.startswith(f"mouse {c['c_vi']} ")]
    kind = "probe" + ("-flat" if env else "")
    out = run(c, args, kind, work, saves, substitute(rows, False, (0, 0)), c["t_vi"] // 2 + 40, env=env)
    if c.get("side") and not args.keep_probes:
        shutil.rmtree(work / f"{c['name']}-{kind}", ignore_errors=True)
    # the pictures' VIs as graphics frames (--dump-frames counts frames: P1_GFX_TASK frame= vi=)
    tasks = [(int(f), int(v)) for f, v in re.findall(r"P1_GFX_TASK frame=(\d+) vi=(\d+)", out)]
    frames = []
    for want in c.get("pictures", []):
        f = max((f for f, v in tasks if v <= want), default=1)
        last_f, last_v = tasks[-1] if tasks else (1, 1)
        frames.append(f if want <= last_v else last_f + (want - last_v) // 2)
    c["frames_shot"] = frames
    screen = None
    for m in re.finditer(r"P1_AIM_SCREEN vi=(\d+)(.*)", out):
        if int(m.group(1)) <= c["t_vi"]:
            screen = dict((k, tuple(float(v) for v in xy.split(","))) for k, xy in re.findall(r"(\w+)=([-\d.]+,[-\d.]+)", m.group(2)))
    if not screen:
        raise SystemExit(f"{c['name']}: the probe saw no P1_AIM_SCREEN (the spell was not ready?)")
    c["compass"] = compass(out, c["t_vi"])
    t = c["target"]
    if t.startswith("b:"):
        return bearing_point(c["compass"], float(t[2:]))
    if t.startswith("g:"):
        dx, dy = (float(v) for v in t[2:].split(","))
        return screen["player"][0] + dx, screen["player"][1] + dy
    return screen[t]


def compass(out, before):
    """the last P1_AIM_COMPASS at or before VI `before`: {"b": {deg: (x, y)}, "body": {i: (fx, fy, tx, ty, half)}}"""
    found = None
    for m in re.finditer(r"P1_AIM_COMPASS vi=(\d+)(.*)", out):
        if int(m.group(1)) <= before:
            found = m.group(2)
    if not found:
        return None
    b = {int(k): (float(x), float(y)) for k, x, y in re.findall(r" b(\d+)=([-\d.]+),([-\d.]+)", found)}
    body = {int(k): tuple(float(v) for v in vals.split(",")) for k, vals in re.findall(r" body(\d+)=([-\d.,]+)", found)}
    return {"b": b, "body": body}


def bearing_point(cp, deg):
    """the picture's point (CENTRE) over the ground 40 from the player at that bearing (between the 5-degree marks)"""
    deg %= 360
    k0 = int(deg // 5) * 5
    k1 = (k0 + 5) % 360
    t = (deg - k0) / 5
    (x0, y0), (x1, y1) = cp["b"][k0], cp["b"][k1]
    return x0 + (x1 - x0) * t, y0 + (y1 - y0) * t


def shapes(out):
    """the cast's drawn shapes and the monsters at the cast (the lines after the last P1_AIM_CAST)"""
    cast = list(re.finditer(r"P1_AIM_CAST .*", out))
    if not cast:
        return None
    text = out[cast[-1].start():]
    sh = {"tunnels": {}, "lands": {}, "cones": {}, "enemies": {}, "rot": None, "targets": {}, "tubes": {}, "middles": {}}
    for n, t in re.findall(r" c(\d+)=t(-?\d+)", cast[-1].group(0)):
        sh["targets"][int(n)] = int(t)
    for m in re.finditer(r"P1_AIM_TUNNEL vi=\d+ spell=\S+ cast=(\d+) land=(-?\d+) beads=(\S*)", text):
        sh["tunnels"][int(m[1])] = [tuple(float(v) for v in b.replace(":", ",").split(",")) for b in m[3].split(";") if b]
        sh["lands"][int(m[1])] = int(m[2])
    for m in re.finditer(r"P1_AIM_CONE vi=\d+ spell=\S+ cast=(\d+) start=([-\d.]+),([-\d.]+) bins=(\d+) r=(\S*)", text):
        bins = {int(i): (float(a), float(b)) for i, a, b in re.findall(r"(\d+):([-\d.]+):([-\d.]+);", m[5])}
        sh["cones"][int(m[1])] = (float(m[2]), float(m[3]), int(m[4]), bins)
    for m in re.finditer(r"P1_AIM_TUBE vi=\d+ spell=\S+ cast=(\d+) a=([-\d.]+) b=([-\d.]+) beads=(\S*)", text):
        beads = [tuple(float(v) for v in b.replace(":", ",").split(",")) for b in m[4].split(";") if b]
        sh["tubes"][int(m[1])] = (float(m[2]), float(m[3]), beads)
    en = re.search(r"P1_AIM_ENEMIES .*", text)
    if en:
        for i, x, y, z, r in re.findall(r" e(\d+)=([-\d.]+),([-\d.]+),([-\d.]+),r([\d.]+)", en.group(0)):
            sh["enemies"][int(i)] = (float(x), float(y), float(z), float(r))
        for i, mid in re.findall(r" e(\d+)=\S*?,m([-\d.]+)", en.group(0)):
            sh["middles"][int(i)] = float(mid)
    ap = re.search(r"P1_AIM_APPLY .*rot=[-\d.]+>([-\d.]+)", out)
    if ap:
        sh["rot"] = float(ap.group(1))
    sh["hitat"] = [(int(m[2]), int(m[3]), float(m[4]), float(m[5]), int(m[6]), int(m[7], 16), int(m[1]))
                   for m in re.finditer(r"P1_AIM_HITAT vi=(\d+) slot=(\d+) enemy=(\d+) fx=([-\d.]+),[-\d.]+,([-\d.]+) "
                                        r"left=(\d+) flags=(\w+)", text)]
    # (each hit's effect height and the monster's place then)
    sh["hitat3"] = [(float(m[1]), float(m[2]), float(m[3]), float(m[4]), float(m[5]), float(m[6]))
                    for m in re.finditer(r"P1_AIM_HITAT vi=\d+ slot=\d+ enemy=\d+ fx=([-\d.]+),([-\d.]+),([-\d.]+) .*?"
                                         r"at=([-\d.]+),([-\d.]+),([-\d.]+)", text)]
    return sh


def tunnel_margin(sh, e, casts=None):
    """how far monster e's edge is outside the tunnels (under 0: inside), and the place (cast, k) nearest"""
    x, _, z, r = sh["enemies"][e]
    best, where = 1e9, None
    for n, beads in sh["tunnels"].items():
        if casts is not None and n not in casts:
            continue
        for k, bx, by, bz, w in beads:
            d = math.hypot(x - bx, z - bz) - w - r
            if d < best:
                best, where = d, (n, int(k))
    return best, where


def tube_margin(sh, e, before_land=False, r_own=False):
    """how far monster e's middle is outside the drawn 3D paths (under 0: inside), in units across: the union of the
    ellipsoids (half-axes a across, b up) round the logged places, (q - 1) a where q is the ellipsoid's own distance
    (1 on its surface). before_land: the flight only (the places before the landing). r_own: with the monster's own
    radius (the drawn ones take the widest of the battle)"""
    x, y, z, r = sh["enemies"][e]
    mid = sh["middles"].get(e, y)
    best = 1e9
    for n, (a, b, beads) in sh["tubes"].items():
        land = sh["lands"].get(n, -1)
        if r_own:
            widest = max(rr for _, _, _, rr in sh["enemies"].values())
            a = a - widest + r
            b = 2 * a
        for k, bx, by, bz in beads:
            if before_land and land >= 0 and k >= land:
                continue
            q = math.sqrt(((x - bx) / a) ** 2 + ((mid - by) / b) ** 2 + ((z - bz) / a) ** 2)
            best = min(best, (q - 1) * a)
    return best


def tube_hit_margin(sh, e, fx, fy, fz, at):
    """a hit from the effect at (fx, fy, fz) on monster e standing at `at`: how far its middle is outside the drawn
    ellipsoid round that place (under 0: inside); None when no place of the drawn paths is there"""
    for a, b, beads in sh["tubes"].values():
        for k, bx, by, bz in beads:
            if abs(bx - fx) < 0.01 and abs(by - fy) < 0.01 and abs(bz - fz) < 0.01:
                x, y, z, r = sh["enemies"][e]
                mid = at[1] + (sh["middles"].get(e, y) - y)
                q = math.sqrt(((at[0] - bx) / a) ** 2 + ((mid - by) / b) ** 2 + ((at[2] - bz) / a) ** 2)
                return (q - 1) * a
    return None


def cone_margin(sh, e):
    """how far monster e's edge is outside every cone (under 0: inside one)"""
    x, _, z, r = sh["enemies"][e]
    best = 1e9
    for sx, sz, nb, bins in sh["cones"].values():
        for i, (r0, r1) in bins.items():
            th = i * 2 * math.pi / nb
            ux, uz = math.sin(th), math.cos(th)
            t = (x - sx) * ux + (z - sz) * uz
            t = min(max(t, r0), r1)
            best = min(best, math.hypot(x - (sx + ux * t), z - (sz + uz * t)) - r)
    return best


def aim_lines(out):
    """the aim's lines, without their VIs; numbers to 1 decimal (a ground aim's point, from the pointer's ray, differs in
    its last bits between 4:3 and 16:9)"""
    keep = ("P1_AIM_CLICK", "P1_AIM_APPLY", "P1_AIM_CAST", "P1_AIM_HITS", "P1_AIM_PANEL", "P1_AIM_DROPPED")
    return [re.sub(r"-?\d+\.\d+", lambda m: f"{float(m.group(0)):.1f}".replace("-0.0", "0.0"), re.sub(r" ?vi=\d+", "", l))
            for l in out.splitlines() if l.startswith(keep)]


def field(out, pattern):
    m = re.search(pattern, out)
    return m.group(1) if m else None


def bits(mask, i):
    return bool(int(mask, 16) & (1 << i))


def check(c, out, label, results):
    problems = []
    if "P1_TEST_EXIT" in out or "P1_OS_OK" not in out:
        problems.append(f"{label}: the run did not finish")
    def last(pattern, text):
        found = list(re.finditer(pattern, text))
        return found[-1] if found else None
    # (the case's own: the last; a turn before it may have had a cast of its own)
    click = last(r"P1_AIM_CLICK .*predicted=(\w+)", out)
    cast = last(r"P1_AIM_CAST .*", out)
    hits = last(r"P1_AIM_HITS .*spell=([\d.]+) predicted=(\w+) actual=(\w+).*", out)
    after = out[cast.start():] if cast else out
    if c["click"]:
        if not (click and cast and hits):
            problems.append(f"{label}: no click, cast or hits line")
            return problems
        if hits.group(1) != c["spell"] and c["name"] != "undo":
            problems.append(f"{label}: cast {hits.group(1)}, wanted {c['spell']}")
        if click.group(1) != hits.group(2):
            problems.append(f"{label}: the preview at the click ({click.group(1)}) is not the cast's ({hits.group(2)})")
        if hits.group(2) != hits.group(3) and not hits.group(0).endswith(" random"):  # (Rock Combo: random places)
            problems.append(f"{label}: predicted {hits.group(2)}, the game hit {hits.group(3)}")
    for what, arg in c["expect"]:
        if what in ("hit", "miss", "not_hit"):
            i = int(arg[1])
            hit = bool(hits) and bits(hits.group(3), i)
            if (what == "hit") != hit:
                problems.append(f"{label}: {arg} {'not ' if what == 'hit' else ''}hit (actual {hits.group(3) if hits else '-'})")
        elif what == "casts":
            n = len(re.findall(r" c\d+=t", cast.group(0))) if cast else 0
            if n != arg:
                problems.append(f"{label}: {n} casts, wanted {arg}")
        elif what == "margin":
            e, lo, hi = arg.split(":")
            m = field(cast.group(0) if cast else "", rf"near=.*{e}:([-\d.]+)")
            if m is None or not (float(lo) <= float(m) <= float(hi)):
                problems.append(f"{label}: {e}'s margin {m} not in [{lo}, {hi}]")
            results.setdefault(c["name"], {})["margin"] = m
        elif what == "fx_at_preview":
            # the game's effect stands where the preview's circle is
            pre = re.search(r"P1_AIM_CAST .* c0=t\d+,([-\d.]+),([-\d.]+)>", after)
            fx = re.search(rf"P1_AIM_FX vi=\d+ slot=\d+ spell={re.escape(c['spell'])} .*pos=([-\d.]+),[-\d.]+,([-\d.]+)", after)
            if not (pre and fx) or math.dist((float(pre[1]), float(pre[2])), (float(fx[1]), float(fx[2]))) > 0.11:
                problems.append(f"{label}: the effect at {fx.group(1, 2) if fx else '-'}, the preview at {pre.group(1, 2) if pre else '-'}")
            elif pre:
                results.setdefault(c["name"], {})["centre"] = (float(pre[1]), float(pre[2]))
        elif what == "differs_from":
            a = results.get(arg, {}).get("centre")
            b = results.get(c["name"], {}).get("centre")
            if a and b and math.dist(a, b) < 5:
                problems.append(f"{label}: the two directions put the pillar at the same place {a} {b}")
        elif what == "facing_pick_differs":
            # the preview for the player's own facing (before pointing) picks the other monster
            first = re.search(r"P1_AIM_HOVER .*active=0 .*hits=(\w+)", out)
            if first and bits(first.group(1), int(arg[1])):
                problems.append(f"{label}: the facing alone would pick {arg} too (hits {first.group(1)})")
        elif what == "undo":
            seq = [l for l in aim_lines(out) if l.startswith("P1_AIM_PANEL")]
            want = ["collapsed=1 why=aim", "collapsed=0 why=undo", "collapsed=1 why=aim"]
            got = [l.split(" ", 1)[1] for l in seq[:3]]
            if got != want:
                problems.append(f"{label}: the panel went {got}, wanted {want}")
            if not re.search(r"P1_SPELLUI_STATE .*count=1 dirs=2", out):
                problems.append(f"{label}: no undo to one element")
            if not (hits and hits.group(1) == "2.0"):
                problems.append(f"{label}: Water Pillar Lv1 not cast after the undo")
        elif what in ("tunnel", "cone", "off_line", "outside", "flight"):
            sh = shapes(out)
            if not sh or not hits:
                problems.append(f"{label}: no shapes or hits")
                continue
            actual = int(hits.group(3), 16)
            info = results.setdefault(c["name"], {}).setdefault(label, {})
            if what == "tunnel":
                # every monster: hit exactly when its edge is inside the drawn tunnel (0.02: the log's rounding)
                for e in sh["enemies"]:
                    d, where = tunnel_margin(sh, e)
                    hit = bool(actual >> e & 1)
                    info[f"e{e}"] = f"{d:+.2f}{' hit' if hit else ''}"
                    if abs(d) > 0.02 and hit != (d < 0):
                        problems.append(f"{label}: e{e} {'hit' if hit else 'missed'} but {d:+.2f} {'outside' if d > 0 else 'inside'} "
                                        f"the tunnel (nearest place {where})")
                # and every hit at a live place of the tunnel, touching it there; unless a monster was hit fewer times than
                # the copy said (killed by an earlier bolt: the later ones fly elsewhere, docs/SPELL_AIM.md)
                beads = [(n, b) for n, bl in sh["tunnels"].items() for b in bl]
                counts = re.search(r"P1_AIM_CAST .*counts=(\d+)", out)
                got = [sum(1 for h in sh["hitat"] if h[1] == e) for e in range(6)]
                until = 1 << 62
                if counts and any(got[e] < int(counts.group(1)[e]) for e in range(6)):
                    # (the hits up to the last one on a monster hit fewer times than predicted: up to its death)
                    until = min(max(h[6] for h in sh["hitat"] if h[1] == e) for e in range(6) if got[e] < int(counts.group(1)[e]))
                    info["paths"] = (f"differ after a kill at vi {until} (hits per monster " + "".join(map(str, got)) +
                                     " predicted " + counts.group(1) + "): the hits up to it checked")
                for slot, e, fx, fz, left, flags, hvi in sh["hitat"]:
                    if hvi > until:
                        continue
                    near = min(beads, key=lambda nb: math.hypot(nb[1][1] - fx, nb[1][3] - fz), default=None)
                    if near is None or math.hypot(near[1][1] - fx, near[1][3] - fz) > 0.05:
                        problems.append(f"{label}: e{e} hit at {fx:.2f},{fz:.2f}: no live place of the tunnel there")
                    elif e in sh["enemies"]:
                        x, _, z, r = sh["enemies"][e]
                        if math.hypot(x - fx, z - fz) - near[1][4] - r > 0.02:
                            problems.append(f"{label}: e{e} hit from {fx:.2f},{fz:.2f}, outside that place's disc")
                info["hitat"] = len(sh["hitat"])
                if TUBES == "1":
                    # the 3D paths: every monster's middle inside the drawn volume exactly when it is hit (0.02: the log's
                    # rounding), and each hit (up to a kill, as above) inside the ellipsoid of the place it came from
                    if not sh["tubes"]:
                        problems.append(f"{label}: no 3D path logged")
                    widest = max(rr for _, _, _, rr in sh["enemies"].values())
                    for e in sh["enemies"]:
                        d = tube_margin(sh, e)
                        own = tube_margin(sh, e, r_own=True)
                        hit = bool(actual >> e & 1)
                        info[f"tube_e{e}"] = f"{d:+.2f}"
                        if hit and d > 0.02:
                            problems.append(f"{label}: e{e} hit but its middle {d:+.2f} outside the 3D path")
                        if not hit and own < -0.02 and not until < (1 << 62):
                            problems.append(f"{label}: e{e} missed but its middle {-own:.2f} inside the 3D path")
                        if not hit and sh["enemies"][e][3] < widest:
                            info[f"tube_e{e}"] += f" (own radius {own:+.2f})"
                    for (slot, e, fx, fz, left, flags, hvi), (hx, hy, hz, ax, ay, az) in zip(sh["hitat"], sh["hitat3"]):
                        if hvi > until:
                            continue
                        d = tube_hit_margin(sh, e, hx, hy, hz, (ax, ay, az))
                        if d is None:
                            problems.append(f"{label}: e{e} hit from {hx:.2f},{hy:.2f},{hz:.2f}: no place of the 3D path there")
                        elif d > 0.02:
                            problems.append(f"{label}: e{e}'s middle {d:+.2f} outside the 3D path's ellipsoid it was hit from")
            elif what == "cone":
                if not sh["cones"]:
                    problems.append(f"{label}: no cone")
                for e in sh["enemies"]:
                    d = cone_margin(sh, e)
                    hit = bool(actual >> e & 1)
                    info[f"cone_e{e}"] = f"{d:+.2f}"
                    # (1.0: the cone is kept per 1.5 degrees of bearing)
                    if hit and d > 1.0:
                        problems.append(f"{label}: e{e} hit {d:.2f} outside the cones")
            elif what == "outside":
                e = int(arg[1])
                d = cone_margin(sh, e)
                if d < 1.0:
                    problems.append(f"{label}: {arg} {d:+.2f} from the cones' edge, not outside")
            elif what == "off_line":
                # the monster is well off each bolt's straight line (its launch bearing from its first place)
                e = int(arg[1])
                x, _, z, r = sh["enemies"][e]
                for n, (sx, sz, nb, bins) in sh["cones"].items():
                    th = sh["rot"] + math.radians({0: 0, 1: -45, 2: 45}.get(n, 0))
                    ux, uz = math.sin(th), math.cos(th)
                    t = max(0.0, (x - sx) * ux + (z - sz) * uz)
                    d = math.hypot(x - (sx + ux * t), z - (sz + uz * t))
                    info[f"line{n}_e{e}"] = f"{d:.1f}"
                    if sh["targets"].get(n) == e and d < 6 + r + 2:
                        problems.append(f"{label}: {arg} only {d:.1f} off bolt {n}'s straight line")
            elif what == "flight":
                # monster e: hit before the rock lands exactly when a place of the tunnel before the landing reaches it
                e = int(arg[1])
                x, _, z, r = sh["enemies"][e]
                land = sh["lands"].get(0, -1)
                flight = [b for b in sh["tunnels"].get(0, []) if land < 0 or b[0] < land]
                d = min((math.hypot(x - b[1], z - b[3]) - b[4] - r for b in flight), default=1e9)
                hit_air = any(he == e and not (fl & 1) for _, he, _, _, _, fl, _ in sh["hitat"])
                info["flight"] = f"e{e} {'hit in flight' if hit_air else 'not hit in flight'}, tunnel {d:+.2f}"
                if abs(d) > 0.02 and hit_air != (d < 0):
                    problems.append(f"{label}: e{e} {'hit' if hit_air else 'not hit'} in flight, the flight's tunnel {d:+.2f}")
                if TUBES == "1":
                    # and its middle against the flight's 3D path (hanging in the air: Lv1 over it, Lv3 down on it)
                    d3 = tube_margin(sh, e, before_land=True)
                    info["flight"] += f", 3D path {d3:+.2f}"
                    if abs(d3) > 0.02 and hit_air != (d3 < 0):
                        problems.append(f"{label}: e{e} {'hit' if hit_air else 'not hit'} in flight, its middle {d3:+.2f} from "
                                        "the flight's 3D path")
        elif what == "ui_off":
            if any("collapsed=1" in l for l in aim_lines(out)):
                problems.append(f"{label}: the panel folded with the spell UI off")
    return problems


def body_distance(px, py, body):
    """how far (CENTRE units) a point is outside a monster's body on the picture (P1_AIM_COMPASS body: its feet, its top,
    its half-width; spell_aim.c body_distance)"""
    fx, fy, tx, ty, half = body
    ex, ey = tx - fx, ty - fy
    ln = math.hypot(ex, ey)
    ux, uy = ex / ln, ey / ln
    along = (px - fx) * ux + (py - fy) * uy
    side = abs((px - fx) * uy - (py - fy) * ux)
    past = -along if along < 0 else along - ln if along > ln else 0.0
    return max(side - half, past)


def pick_test(args, work, saves):
    """The monster pick: the pointer on e0's body picks it; 2 units off its side (between PICK_IN 1 and PICK_STAY 3)
    keeps it once picked; 4 off is the ground; back to 2 off stays the ground, as does 2.5 above its head; on the body
    again picks it. Where the old pick (the radius and 3 more) would have taken the monster, the ground. At 4:3 and
    16:9."""
    seq = [2, 2]
    t0 = 370 + 6 * len(seq) + 40
    base = START + WALK + keys(seq, 370) + [f"mouse {370 + 6 * len(seq) + 6} 0 0 0 0 {{c:300}} 60"]
    c = dict(name="pick", env={}, rows=base, t_vi=t0, c_vi=-1, target="e0", pictures=[t0 + 16])
    probe(c, args, work, saves)
    body = c["compass"]["body"][0]
    fx, fy, tx, ty, half = body
    mx, my = (fx + tx) / 2, (fy + ty) / 2
    # the steps: (offset to the right of the column, the pick wanted); each point's real distance from the column
    want = [(0, 0, 0), (half + 2, 0, 0), (half + 4, 0, -1), (half + 2, 0, -1), (0, ty - my - 2.5, -1), (0, 0, 0)]
    steps, rows = [], list(base)
    for k, (off, up, tgt) in enumerate(want):
        x, y = round(mx + off), round(my + up)
        d = body_distance(x, y, body)
        steps.append((t0 + 10 * k, x, y, d, tgt))
        rows.append(f"mouse {t0 + 10 * k} 0 0 0 0 {{c:{x}}} {y}")
    problems = []
    for aspect, wide in (("43", False), ("169", True)):
        out = run(c, args, aspect, work, saves, substitute(rows, wide, (0, 0)), (t0 + 60) // 2 + 40, c["frames_shot"])
        hov = [(int(m[1]), int(m[2])) for m in re.finditer(r"P1_AIM_HOVER vi=(\d+) active=1 \S+ target=(-?\d+)", out)]
        got = []
        for vi_, x, y, d, tgt in steps:
            now = [t for v, t in hov if v <= vi_ + 6]
            g = now[-1] if now else None
            got.append(g)
            if g != tgt:
                problems.append(f"{aspect}: the pointer {d:+.1f} from e0's body's side picked {g}, wanted {tgt}")
        old = half / 0.6 + 3  # the old pick: the radius and 3 more round the column
        print(f"SPELL_AIM pick {aspect}: e0 body half-width {half:.1f} (the old pick reached {old:.1f}); "
              + " ".join(f"{d:+.1f}>{g}" for (_, _, _, d, _), g in zip(steps, got)))
    if steps[2][3] + half > half / 0.6 + 3:
        problems.append("the ground step is outside the old pick too: it shows nothing")
    for pr in problems:
        print("    !", pr)
    print(f"SPELL_AIM pick {'OK' if not problems else 'FAIL'}")
    return bool(problems)


def verify(args, work, saves, which):
    """every aimable spell at the first monster: the game's effects against the copy's paths, and the hits"""
    out0 = run(dict(name="table", env={}), args, "table", work, saves,
               substitute(START + WALK + keys([2], 370), False, (0, 0)), 200)
    defs = {}
    for m in re.finditer(r"P1_AIM_SPELLDEF (\d+\.\d+) kind=(\w+) .* seq=(\d)(\d)(\d) len=(\d)", out0):
        defs[m.group(1)] = (m.group(2), [int(m.group(k)) for k in (3, 4, 5)][:int(m.group(6))])
    failed = 0
    for sid, (kind, seq) in defs.items():
        if kind == "self" or (which and sid not in which):
            continue
        c = case("v" + sid.replace(".", "_"), (seq, sid), "e0")
        c["env"] = {"ELTALE_SPELL_AIM_LOG": "3"}
        pt = probe(c, args, work, saves)
        out = run(c, args, "verify", work, saves, substitute(c["rows"], False, pt), c["frames"] + 60)
        full = out
        cut = out.find("P1_AIM_HITS")
        if cut >= 0:
            out = out[:out.find("\n", cut) + 1]
        paths = {}
        for m in re.finditer(r"P1_AIM_SIMPATH cast=(\d+) k=\d+ pos=([-\d.]+),([-\d.]+),([-\d.]+)", out):
            paths.setdefault(int(m[1]), []).append(tuple(float(x) for x in m.group(2, 3, 4)))
        order, current, worst, ends = [], {}, 0.0, {}
        for m in re.finditer(rf"P1_AIM_FX vi=\d+ slot=(\d+) spell={re.escape(sid)} left=(\d+) delay=(\d+) flags=(\w+) "
                             r"pos=([-\d.]+),([-\d.]+),([-\d.]+)", out):
            s, left, delay, flags = int(m[1]), int(m[2]), int(m[3]), int(m[4], 16)
            cur = current.get(s)
            if cur is None or delay > cur[2] or (left > cur[1] and not flags & 1):
                cur = (len(order), left, delay)
                order.append(s)
            current[s] = (cur[0], left, delay)
            pos = tuple(float(x) for x in m.group(5, 6, 7))
            ends.setdefault(cur[0], [pos, pos])[1] = pos
            if kind != "random" and cur[0] in paths:
                worst = max(worst, min(math.dist(pos, q) for q in paths[cur[0]]))
        hits = re.search(r"P1_AIM_HITS .*predicted=(\w+) actual=(\w+)", out)
        ok = hits and hits.group(1) == hits.group(2)
        # the moving spells: every monster hit exactly when inside the drawn tunnel, every hit at a live place of it
        # (and Fire Ball's hits inside its cone)
        tunnel = []
        if kind in ("line", "lob", "fan", "homing"):
            c["expect"] = [("tunnel", None)] + ([("cone", None)] if kind == "fan" else [])
            info = {}
            tunnel = check(c, full, "tunnel", info)
            ok = ok and not tunnel
        failed += not ok
        print(f"VERIFY {sid:5} {kind:7} effects={len(order)} worst={worst:.2f} hits={hits.group(1) + '/' + hits.group(2) if hits else '-'} "
              + " ".join(f"c{n}:{a[0]:.1f},{a[2]:.1f}>{b[0]:.1f},{b[2]:.1f}" for n, (a, b) in sorted(ends.items()))
              + ("" if ok else "  !"))
        cast = re.search(r"P1_AIM_CAST .*", out)
        if cast:
            print("      " + cast.group(0)[:400])
        if kind in ("line", "lob", "fan", "homing"):
            print("      tunnel: " + " ".join(f"{k}={v}" for k, v in info.get(c["name"], {}).get("tunnel", {}).items()))
        for pr in tunnel:
            print("      !", pr)
    print("SPELL_AIM_VERIFY " + ("OK" if not failed else f"FAIL ({failed})"))
    return failed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--saves", type=Path, default=Path("C:/code/eltale/out/bugsaves"))
    ap.add_argument("--out", type=Path, default=ROOT / "out/spell_aim_test")
    ap.add_argument("--only", help="cases, comma-separated (and lift, pick, idle)")
    ap.add_argument("--verify", nargs="*", help="check the copy of the spell code against the game (spells: 0.5 1.3 ...)")
    ap.add_argument("--tubes", choices=("0", "1"), default="1", help="the 3D spell paths (ELTALE_SPELL_AIM_TUBES) in every run")
    ap.add_argument("--sheet", action="store_true", help="only make the contact sheet of OUT's pictures")
    ap.add_argument("--keep-probes", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()
    global TUBES
    TUBES = args.tubes
    if args.sheet:
        sheet(args.out.resolve())
        return
    args.exe = args.exe.resolve()
    args.rom = args.rom.resolve()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    src = args.saves / "battle-stuck"
    if not (src / "quicksaves/quick0.elq").is_file():
        raise SystemExit(f"spell_aim_test: needs the quick save {src} (--saves)")
    saves = work / "save"
    if saves.exists():
        shutil.rmtree(saves)
    sut.make_save(src, saves, 60, [48, 48, 48, 48])
    if args.verify is not None:
        sys.exit(1 if verify(args, work, saves, set(args.verify)) else 0)
    only = args.only.split(",") if args.only else None
    failed = 0
    results = {}
    for c in CASES:
        if only and c["name"] not in only:
            continue
        pt = probe(c, args, work, saves)
        o43 = run(c, args, "43", work, saves, substitute(c["rows"], False, pt), c["frames"], c["frames_shot"])
        o169 = run(c, args, "169", work, saves, substitute(c["rows"], True, pt), c["frames"], c["frames_shot"])
        # the pictures under names that say what they are (OUT/pictures/<case>-<43|169>-<n>.png)
        shots = work / "pictures"
        shots.mkdir(exist_ok=True)
        for aspect in ("43", "169"):
            for n, f in enumerate(c["frames_shot"]):
                src = work / f"{c['name']}-{aspect}" / "pictures" / f"frame_{f}.png"
                if src.is_file():
                    shutil.copyfile(src, shots / f"{c['name']}-{aspect}-{n}.png")
        problems = check(c, o43, "4:3", results) + check(c, o169, "16:9", {})
        if c.get("side"):
            problems = [pr for pr in problems if "the run did not finish" in pr]
        if aim_lines(o43) != aim_lines(o169):
            problems.append("4:3 and 16:9 print different aim lines")
        audio = [field(o, r"P1_AUDIO .*hash=(\w+)") for o in (o43, o169)]
        if len(set(audio)) != 1:
            problems.append(f"the game's sound differs between 4:3 and 16:9: {audio}")
        hits = (re.findall(r"P1_AIM_HITS (.*)", o43) or [""])[-1]
        failed += bool(problems)
        print(f"SPELL_AIM {c['name']} {'OK' if not problems else 'FAIL'} target={c['target']}@{pt[0]:.0f},{pt[1]:.0f} {hits or ''}")
        for label, info in results.get(c["name"], {}).items():
            if isinstance(info, dict):
                print(f"    {label}: " + " ".join(f"{k}={v}" for k, v in info.items()))
        for pr in problems:
            print("    !", pr)
    # the 3D paths change only the picture: the lob with them off, the same aim lines and sound, other pictures
    if (not only or "tubes_off" in only) and TUBES == "1":
        c = dict(next(x for x in CASES if x["name"] == "lob"), name="tubes_off")
        pt = probe(c, args, work, saves)
        problems = []
        shots = work / "pictures"
        shots.mkdir(exist_ok=True)
        for aspect, wide in (("43", False), ("169", True)):
            on = run(c, args, aspect + "-on", work, saves, substitute(c["rows"], wide, pt), c["frames"], c["frames_shot"])
            off = run(c, args, aspect + "-off", work, saves, substitute(c["rows"], wide, pt), c["frames"], c["frames_shot"],
                      env={"ELTALE_SPELL_AIM_TUBES": "0"})
            if aim_lines(on) != aim_lines(off):
                problems.append(f"{aspect}: the aim lines differ with the 3D paths on and off")
            if field(on, r"P1_AUDIO .*hash=(\w+)") != field(off, r"P1_AUDIO .*hash=(\w+)"):
                problems.append(f"{aspect}: the game's sound differs with the 3D paths on and off")
            if "P1_AIM_TUBE " not in on or "P1_AIM_TUBE " in off:
                problems.append(f"{aspect}: the 3D path logged {'not ' if 'P1_AIM_TUBE ' not in on else ''}with them on, "
                                f"{'' if 'P1_AIM_TUBE ' in off else 'not '}with them off")
            for n, f in enumerate(c["frames_shot"]):
                pa = work / f"tubes_off-{aspect}-on" / "pictures" / f"frame_{f}.png"
                pb = work / f"tubes_off-{aspect}-off" / "pictures" / f"frame_{f}.png"
                if pa.is_file() and pb.is_file():
                    shutil.copyfile(pa, shots / f"tubes_off-{aspect}-{n}-on.png")
                    shutil.copyfile(pb, shots / f"tubes_off-{aspect}-{n}-off.png")
                    if n == 0 and pa.read_bytes() == pb.read_bytes():
                        problems.append(f"{aspect}: the preview's pictures are the same with the 3D paths on and off")
                else:
                    problems.append(f"{aspect}: no picture {f}")
        failed += bool(problems)
        print(f"SPELL_AIM tubes_off {'OK' if not problems else 'FAIL'}")
        for pr in problems:
            print("    !", pr)
    # the raised camera changes only the picture: the same aim (its own probe: the monsters are elsewhere on a flat view)
    if not only or "lift" in only:
        c = next(x for x in CASES if x["name"] == "fireball")
        c = dict(c, name="lift")
        flat = {"ELTALE_SPELL_AIM_CAMERA": "0"}
        up = run(c, args, "up", work, saves, substitute(c["rows"], False, probe(c, args, work, saves)), c["frames"])
        down = run(c, args, "flat", work, saves, substitute(c["rows"], False, probe(c, args, work, saves, flat)), c["frames"],
                   env=flat)
        problems = []
        a, b = aim_lines(up), aim_lines(down)
        if [l for l in a if not l.startswith("P1_AIM_CLICK")] != [l for l in b if not l.startswith("P1_AIM_CLICK")]:
            problems.append("the aim lines differ with the camera raised and not")
        if field(up, r"P1_AUDIO .*hash=(\w+)") != field(down, r"P1_AUDIO .*hash=(\w+)"):
            problems.append("the game's sound differs with the camera raised and not")
        failed += bool(problems)
        print(f"SPELL_AIM lift {'OK' if not problems else 'FAIL'}")
        for pr in problems:
            print("    !", pr)
    if not only or "pick" in only:
        failed += pick_test(args, work, saves)
    # aim on with no pointer: the game as with it off (the keys build the spell, the controller's A casts it)
    if not only or "idle" in only:
        c = dict(name="idle", env={})
        rows = substitute(START + WALK + keys([2, 2], 370) + ["400 0x8000 0 0", "402 0 0 0"], False, (0, 0))
        on = run(c, args, "on", work, saves, rows, 500)
        off = run(c, args, "off", work, saves, rows, 500, env={"ELTALE_SPELL_AIM": "0"})
        problems = []
        for what in (r"P1_OS_OK .*last_hash=(\w+)", r"P1_AUDIO .*hash=(\w+)"):
            if field(on, what) != field(off, what) or field(on, what) is None:
                problems.append(f"aim on and off differ: {field(on, what)} {field(off, what)}")
        failed += bool(problems)
        print(f"SPELL_AIM idle {'OK' if not problems else 'FAIL'} {field(on, r'P1_OS_OK .*last_hash=(\w+)')}")
        for pr in problems:
            print("    !", pr)
    print("SPELL_AIM_TEST " + ("OK" if not failed else f"FAIL ({failed})"))
    sheet(work)
    sys.exit(1 if failed else 0)


def sheet(work):
    """OUT/sheet/*.png: each moving spell's preview from the raised camera (4:3 and 16:9) and from the side, and the
    lob with the 3D paths on and off; captioned with the pictures' names, their full paths in index.txt"""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("SPELL_AIM sheet: no PIL, no contact sheet")
        return
    pics = work / "pictures"
    rows = [("rock1", "t_rock1_side"), ("rock2", "t_rock2"), ("rock3", "t_rock3_side"), ("bomb", "t_bomb"),
            ("water", "t_water"), ("cyclone", "t_cyclone"), ("cutter", "t_cutter"), ("magma", "t_magma"),
            ("roll1", "t_roll1"), ("roll2", "t_roll2"), ("fire1", "t_fire1"), ("fire2", "t_fire2"), ("fire3", "t_fire3"),
            ("wind1", "t_wind1"), ("wind2", "t_wind2"), ("wind3", "t_wind3"), ("arrow1", "t_arrow1"), ("arrow2", "t_arrow2"),
            ("off", "tubes_off")]
    out = work / "sheet"
    out.mkdir(exist_ok=True)
    index = []
    w, h, cap = 480, 270, 14
    per = 5
    for page in range(0, len(rows), per):
        chunk = rows[page:page + per]
        img = Image.new("RGB", (3 * w, len(chunk) * (h + cap)), (24, 24, 24))
        d = ImageDraw.Draw(img)
        for r, (side, raised) in enumerate(chunk):
            files = [pics / f"{raised}-43-0.png", pics / f"{raised}-169-0.png", pics / f"side_{side}-169-0.png"]
            if side == "off":  # (the lob, Rock Lv3, with the 3D paths on and off)
                files = [pics / "tubes_off-169-0-on.png", pics / "tubes_off-169-0-off.png", pics / "tubes_off-43-0-off.png"]
            for col, f in enumerate(files):
                x, y = col * w, r * (h + cap)
                if f.is_file():
                    im = Image.open(f).convert("RGB")
                    im.thumbnail((w, h))
                    img.paste(im, (x + (w - im.width) // 2, y))
                    index.append(str(f))
                d.text((x + 4, y + h), f.name if f.is_file() else f"(no {f.name})", fill=(230, 230, 230))
        name = out / f"sheet_{page // per + 1}.png"
        img.save(name)
        index.append(f"== {name}")
    (out / "index.txt").write_text("\n".join(index) + "\n")
    print(f"SPELL_AIM sheet {out}")


if __name__ == "__main__":
    main()
