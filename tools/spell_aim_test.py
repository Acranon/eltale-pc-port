#!/usr/bin/env python3
"""Click to aim spells, headless (port/p0/spell_aim.c; docs/SPELL_AIM.md).

    python tools/spell_aim_test.py --exe out/p1-windows/eltale-p1.exe --rom eltalemonsters.jp.z64
        [--saves C:/code/eltale/out/bugsaves] [--out out/spell_aim_test] [--only NAME,...] [--verify [SPELL ...]]

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

Also: `lift`: the same aim with the camera raised and not (ELTALE_SPELL_AIM_CAMERA=0): the same game (aim lines, sound);
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


def case(name, spell, target, first=370, pre=(), click=True, expect=(), pictures=None, env=None, extra=(), frames=None):
    """target: "e0"/"e1" (a monster), "g:DX,DY" (the ground: the player's place on the picture plus DX, DY)"""
    seq, sid = spell
    t_vi = first + 6 * len(seq) + 40       # pointing at the target (the camera has risen by then)
    c_vi = t_vi + 10
    rows = START + WALK + list(pre) + keys(seq, first) + [f"mouse {first + 6 * len(seq) + 6} 0 0 0 0 {{c:300}} 60",
                                                         f"mouse {t_vi} 0 0 0 0 {{T}}"]
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
]


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
    t = c["target"]
    if t.startswith("g:"):
        dx, dy = (float(v) for v in t[2:].split(","))
        return screen["player"][0] + dx, screen["player"][1] + dy
    return screen[t]


def aim_lines(out):
    keep = ("P1_AIM_CLICK", "P1_AIM_APPLY", "P1_AIM_CAST", "P1_AIM_HITS", "P1_AIM_PANEL", "P1_AIM_DROPPED")
    return [re.sub(r" ?vi=\d+", "", l) for l in out.splitlines() if l.startswith(keep)]


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
        elif what == "ui_off":
            if any("collapsed=1" in l for l in aim_lines(out)):
                problems.append(f"{label}: the panel folded with the spell UI off")
    return problems


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
        failed += not ok
        print(f"VERIFY {sid:5} {kind:7} effects={len(order)} worst={worst:.2f} hits={hits.group(1) + '/' + hits.group(2) if hits else '-'} "
              + " ".join(f"c{n}:{a[0]:.1f},{a[2]:.1f}>{b[0]:.1f},{b[2]:.1f}" for n, (a, b) in sorted(ends.items()))
              + ("" if ok else "  !"))
        cast = re.search(r"P1_AIM_CAST .*", out)
        if cast:
            print("      " + cast.group(0)[:400])
    print("SPELL_AIM_VERIFY " + ("OK" if not failed else f"FAIL ({failed})"))
    return failed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--saves", type=Path, default=Path("C:/code/eltale/out/bugsaves"))
    ap.add_argument("--out", type=Path, default=ROOT / "out/spell_aim_test")
    ap.add_argument("--only", help="cases, comma-separated (and lift, idle)")
    ap.add_argument("--verify", nargs="*", help="check the copy of the spell code against the game (spells: 0.5 1.3 ...)")
    args = ap.parse_args()
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
        if aim_lines(o43) != aim_lines(o169):
            problems.append("4:3 and 16:9 print different aim lines")
        audio = [field(o, r"P1_AUDIO .*hash=(\w+)") for o in (o43, o169)]
        if len(set(audio)) != 1:
            problems.append(f"the game's sound differs between 4:3 and 16:9: {audio}")
        hits = (re.findall(r"P1_AIM_HITS (.*)", o43) or [""])[-1]
        failed += bool(problems)
        print(f"SPELL_AIM {c['name']} {'OK' if not problems else 'FAIL'} target={c['target']}@{pt[0]:.0f},{pt[1]:.0f} {hits or ''}")
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
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
