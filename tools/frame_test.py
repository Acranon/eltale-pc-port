#!/usr/bin/env python3
"""Dump the reference frames (tests/render_frames.txt), draw them with eltale-rdpref, and check them.

    python tools/frame_test.py --rom ROM.z64 [--runs ng,tour] [--twice] [--write-baseline] [--sheet X.png]

For each run in the list: a headless eltale-p1 with ELTALE_FRAME_IMAGE=1 ELTALE_DUMP_IMAGE=<frames>,
in its own scratch folder (and its own ELTALE_DATA_DIR), writes the frame images; eltale-rdpref draws
them into <work>/<run>/png/frame_N.png (+ .vi.png with --vi). Checks:
  - every picture has more than one colour;
  - --twice: the whole dump runs again and every frame image (bytes) and picture hash must match
    (determinism of the game, the frame image and the reference);
  - the picture hashes against tests/render_ref.txt (or write them with --write-baseline).
The frame images (8 MiB each) are deleted at the end unless --keep-bins. PNGs stay in <work>
(default out/frame_test), outside git. Prints FRAME_TEST lines and FRAME_TEST_OK / FRAME_TEST_FAIL.

--native [KINDS] (docs/renderer/NATIVE_DRAW.md): the native renderer's pass instead. Each run is one game:
the kinds drawn natively (default "all": every kind done, NATIVE_KINDS below) keep their display-list commands too (ELTALE_ND_COMPARE=1) while
every other kind's are dropped (ELTALE_DL_KINDS), so the frame image holds only what the native renderer
draws; eltale-rdpref draws that reference into <work>/<run>/png and the native renderer writes its own
pictures (--dump-frames, 1x, the N64 look) into <work>/<run>/native. tools/render_compare.py counts the
differences per frame against tests/render_baselines/<set>/<run>.txt (<set> = "all", or the kinds joined
by "+", e.g. map+shadow; --baselines overrides; --write-baseline writes them); diff pictures go to
<work>/<run>/diff. The native pass runs tests/render_frames.txt and tests/render_frames_actor.txt by
default (--list, repeatable, overrides). Work folder default out/frame_test_native. Needs eltale_host.dll and
SDL3.dll next to the exe (tools/build_host.py, then copy out/host/*.dll).
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ".exe" if sys.platform == "win32" else ""
# the kinds the native renderer draws so far (render.h EL_KIND_* names): what "--native all" means
NATIVE_KINDS = ["map", "billboard", "backdrop", "actor", "shadow", "effect", "screen"]


def parse_list(path: Path):
    runs = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, args, frames = (p.strip() for p in line.split("|"))
        env, save, argv = {}, None, []
        for tok in args.split():
            if tok.startswith("@save="):
                save = tok[len("@save="):]
            elif re.match(r"^[A-Z][A-Z0-9_]*=", tok):
                k, v = tok.split("=", 1)
                env[k] = v
            else:
                argv.append(tok)
        argv = [str(ROOT / a) if a.startswith("tests/") else a for a in argv]
        runs.append({"name": name, "env": env, "save": save, "argv": argv, "frames": [int(f) for f in frames.split()]})
    return runs


def sha1(path: Path) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def dump(run, work: Path, args) -> Path:
    d = work / run["name"]
    if d.exists():
        shutil.rmtree(d)
    (d / "out").mkdir(parents=True)
    (d / "data").mkdir()
    if run["save"]:
        src = args.saves / run["save"]
        if not src.is_dir():
            raise SystemExit(f"frame_test: run {run['name']} needs the quick save {src} (see --saves)")
        shutil.copytree(src, d / "data", dirs_exist_ok=True)
    env = dict(os.environ)
    env.update(run["env"])
    env["ELTALE_DATA_DIR"] = str(d / "data")
    env["ELTALE_FRAME_IMAGE"] = "1"
    env["ELTALE_DUMP_IMAGE"] = ",".join(str(f) for f in run["frames"])
    cmd = [str(args.exe), "--headless", "--fast", *run["argv"], "--rom", str(args.rom)]
    if args.native:
        (d / "native").mkdir()
        env["ELTALE_ND_COMPARE"] = "1"
        env["ELTALE_DL_KINDS"] = args.native
        env["ELTALE_NATIVE_KINDS"] = args.native
        cmd += ["--dump-frames", str(d / "native")]
    with open(d / "log.txt", "wb") as log, open(d / "err.txt", "wb") as err:
        p = subprocess.run(cmd, cwd=d, env=env, stdout=subprocess.PIPE, stderr=err)
        # the per-frame P1_FRAME_IMAGE lines are long; keep the rest
        for line in p.stdout.splitlines(keepends=True):
            if not line.startswith(b"P1_FRAME_IMAGE") and not line.startswith(b"P1_GFX_TASK"):
                log.write(line)
    ok = re.search(rb"P1_OS_OK[^\n]*", (d / "log.txt").read_bytes())
    print(f"FRAME_TEST_RUN {run['name']} {ok.group(0).decode() if ok else 'no P1_OS_OK (exit %d)' % p.returncode}")
    return d


def render(run, d: Path, args):
    bins = [d / "out" / f"frame_{f}.bin" for f in run["frames"]]
    missing = [b.name for b in bins if not b.is_file()]
    if missing:
        print(f"FRAME_TEST {run['name']} missing dumps: {' '.join(missing)}")
    bins = [b for b in bins if b.is_file()]
    (d / "png").mkdir(exist_ok=True)
    cmd = [str(args.rdpref), "--rom", str(args.rom), "--out", str(d / "png")] + (["--vi"] if args.vi else []) + [str(b) for b in bins]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if p.returncode:
        print(p.stderr, file=sys.stderr)
    (d / "rdpref.txt").write_text(p.stdout, newline="\n")
    out = {}
    for line in p.stdout.splitlines():
        m = re.match(r"RDPREF frame=frame_(\d+) .*? hash=([0-9a-f]{16})", line)
        if m:
            out[int(m.group(1))] = {"hash": m.group(2), "line": line, "bin": sha1(d / "out" / f"frame_{m.group(1)}.bin")}
    return out


def colours(png: Path) -> int:
    from PIL import Image
    return len(Image.open(png).convert("RGB").getcolors(1 << 20) or [0, 0])


def sheet(results, work: Path, path: Path):
    from PIL import Image, ImageDraw
    items = [(r, f) for r in results for f in sorted(results[r])]
    cols = 6
    w, h = 320, 254
    im = Image.new("RGB", (cols * w, ((len(items) + cols - 1) // cols) * h), (32, 32, 32))
    draw = ImageDraw.Draw(im)
    for i, (r, f) in enumerate(items):
        x, y = (i % cols) * w, (i // cols) * h
        im.paste(Image.open(work / r / "png" / f"frame_{f}.png").convert("RGB"), (x, y + 14))
        draw.text((x + 3, y + 1), f"{r} {f}", fill=(255, 230, 0))
    path.parent.mkdir(parents=True, exist_ok=True)
    im.save(path)
    print(f"frame_test: contact sheet {path}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--exe", type=Path, default=ROOT / f"out/p1-windows/eltale-p1{EXE}")
    ap.add_argument("--rdpref", type=Path, default=ROOT / f"out/rdpref/eltale-rdpref{EXE}")
    ap.add_argument("--list", type=Path, action="append",
                    help="run list (repeatable; default tests/render_frames.txt, with --native also "
                         "tests/render_frames_actor.txt, then tests/render_frames_effects.txt)")
    ap.add_argument("--baseline", type=Path, default=ROOT / "tests/render_ref.txt")
    ap.add_argument("--work", type=Path, default=ROOT / "out/frame_test")
    ap.add_argument("--saves", type=Path, default=ROOT / "out/bugsaves",
                    help="folder holding the quick saves the list names (e.g. battle-stuck)")
    ap.add_argument("--runs", help="comma list of run names (default all)")
    ap.add_argument("--jobs", type=int, default=2, help="game runs at once")
    ap.add_argument("--twice", action="store_true", help="dump everything twice and compare (determinism)")
    ap.add_argument("--vi", action="store_true", help="also write the VI pictures")
    ap.add_argument("--keep-bins", action="store_true")
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--sheet", type=Path, help="write a contact sheet of every picture here")
    ap.add_argument("--native", nargs="?", const="all", default=None, metavar="KINDS",
                    help="the native renderer's pass: these kinds (default all done kinds) against a reference "
                         "of them alone")
    ap.add_argument("--baselines", type=Path,
                    help="baseline folder of the native pass (default tests/render_baselines/<set>)")
    args = ap.parse_args()
    if args.native:
        kinds = NATIVE_KINDS if args.native == "all" else args.native.split(",")
        bad = [k for k in kinds if k not in NATIVE_KINDS]
        if bad:
            raise SystemExit(f"frame_test: not a native kind: {' '.join(bad)} (done: {' '.join(NATIVE_KINDS)})")
        args.native = ",".join(kinds)
        if args.baselines is None:
            s = "all" if sorted(kinds) == sorted(NATIVE_KINDS) else "+".join(kinds)
            args.baselines = ROOT / "tests/render_baselines" / s
    if not args.list:
        args.list = [ROOT / "tests/render_frames.txt"]
        if args.native:
            args.list.append(ROOT / "tests/render_frames_actor.txt")
        args.list.append(ROOT / "tests/render_frames_effects.txt")
    if args.native and args.work == ROOT / "out/frame_test":
        args.work = ROOT / "out/frame_test_native"
    args.exe, args.rdpref, args.work = args.exe.resolve(), args.rdpref.resolve(), args.work.resolve()
    args.saves = args.saves.resolve()

    runs = [r for path in args.list for r in parse_list(path)]
    if args.runs:
        names = args.runs.split(",")
        runs = [r for r in runs if r["name"] in names]
    fails = 0

    def one(work: Path):
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            dirs = list(pool.map(lambda r: dump(r, work, args), runs))
        return {r["name"]: render(r, d, args) for r, d in zip(runs, dirs)}

    results = one(args.work)
    for r in runs:
        got = results[r["name"]]
        for f in r["frames"]:
            if f not in got:
                print(f"FRAME_TEST {r['name']} {f} NO PICTURE")
                fails += 1
            elif not args.native and colours(args.work / r["name"] / "png" / f"frame_{f}.png") < 2:
                print(f"FRAME_TEST {r['name']} {f} ONE COLOUR")
                fails += 1

    if args.twice:
        again = one(args.work / "second")
        for r in runs:
            for f, v in results[r["name"]].items():
                w = again[r["name"]].get(f)
                if not w or w["bin"] != v["bin"] or w["hash"] != v["hash"]:
                    what = "picture" if not w or w["hash"] != v["hash"] else "frame image only"
                    print(f"FRAME_TEST_NONDETERMINISTIC {r['name']} {f} ({what})")
                    fails += 1
        print(f"FRAME_TEST_TWICE {'OK' if not fails else 'FAIL'}")

    if args.native:
        # the native pictures against the reference of the same kinds, per run, with its baseline
        for r in runs:
            d = args.work / r["name"]
            base = args.baselines / f"{r['name']}.txt"
            cmd = [sys.executable, str(ROOT / "tools/render_compare.py"), str(d / "png"), str(d / "native"),
                   "--out", str(d / "diff"), "--name", r["name"], "--baseline", str(base)]
            if args.write_baseline:
                args.baselines.mkdir(parents=True, exist_ok=True)
                cmd.append("--write-baseline")
            elif not base.is_file():
                cmd = cmd[:-2]
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print(p.stdout, end="")
            if p.returncode:
                fails += 1
            missing = [f for f in r["frames"] if not (d / "native" / f"frame_{f}.png").is_file()]
            if missing:
                print(f"FRAME_TEST {r['name']} no native picture for {' '.join(map(str, missing))}")
                fails += 1
    elif args.write_baseline:
        with open(args.baseline, "w", newline="\n") as fo:
            fo.write("# eltale-rdpref picture hashes (fnv1a-64 of the 320x240 RGBA5551 colour image) per run and frame\n"
                     "# of tests/render_frames.txt; written by tools/frame_test.py --write-baseline. Numbers only.\n")
            for r in runs:
                for f in r["frames"]:
                    if f in results[r["name"]]:
                        fo.write(f"{r['name']} {f} {results[r['name']][f]['hash']}\n")
        print(f"frame_test: wrote {args.baseline}")
    elif args.baseline.is_file():
        base = {}
        for line in args.baseline.read_text().splitlines():
            p = line.split("#", 1)[0].split()
            if len(p) == 3:
                base[(p[0], int(p[1]))] = p[2]
        for r in runs:
            for f, v in sorted(results[r["name"]].items()):
                b = base.get((r["name"], f))
                state = "OK" if b == v["hash"] else ("NEW" if b is None else "DIFF")
                if state == "DIFF":
                    fails += 1
                print(f"FRAME_TEST {r['name']} {f} {v['hash']} {state}")

    if args.sheet:
        sheet({r["name"]: results[r["name"]] for r in runs}, args.work, args.sheet)
    if not args.keep_bins:
        for b in list(args.work.glob("*/out/frame_*.bin")) + list(args.work.glob("second/*/out/frame_*.bin")):
            b.unlink()
    print("FRAME_TEST_OK" if not fails else f"FRAME_TEST_FAIL {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
