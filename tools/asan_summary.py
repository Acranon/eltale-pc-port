#!/usr/bin/env python3
"""Summarise the sanitizer build's reports (docs/MEMORY_SAFETY.md): one line per distinct finding.

    python3 tools/asan_summary.py LOG... [--full]

A finding is keyed by its kind and the first stack frame in game or port code; the line gives how often
it fired, in which logs, and (ASan) the object it hit. --full also prints the first report of each in full.
"""
import re
import sys
from collections import OrderedDict

args = [a for a in sys.argv[1:] if not a.startswith("--")]
full = "--full" in sys.argv
findings = OrderedDict()

asan_head = re.compile(r"==\d+==ERROR: AddressSanitizer: (\S+)")
ubsan_head = re.compile(r"^(\S+?):(\d+):(\d+): runtime error: (.*)$")
own_head = re.compile(r"^\[asan\] (RDRAM .*runs past.*|host stub: (?!\d+ frames).*)$")
frame = re.compile(r"#\d+ 0x[0-9a-f]+ in (\S+) (\S+)")
located = re.compile(r"is located .*")


def short(path):
    """a source path from the repository's root (work/src/..., port/..., game/..., tools/...)"""
    for i in range(len(path)):
        if (i == 0 or path[i - 1] == "/") and path.startswith(("work/src/", "port/", "game/", "tools/"), i):
            return path[i:]
    return path


# Reports looked at and found harmless (docs/MEMORY_SAFETY.md): marked, not counted as open
KNOWN = {
    "work/src/game/env/env.c:150": "B1 benign: a row of map_backdrop_phase_tluts[4][256] reached as an offset from row 0",
    "work/src/game/camera/camera.c:733": "B2 benign: Camera_UpdateHeightOscillation writes only .y of the Vec2f",
    "work/src/libultra/audio/env.c:540": "A1 audio (port/audio is being replaced): float to s16 out of range, as the N64",
    # ELTALE_ASAN_RDRAM=loaded only: the port's own write, not a load the DMA hook sees
    "Port_ImagesUpdate port/text/en_images.c": "R1 port: the English pictures written into RDRAM's common block at boot",
    # SAN_EXTRA=-fstrict-flex-arrays=2 only: libultra audio's variable-length [1] arrays (ALBank.instArray,
    # ALInstrument.soundArray, ALSeqFile.seqArray, ...), indexed past 1 by design
    "port/audio/bank.c": "A2 audio: libultra's [1] variable-length arrays (strict flex arrays only)",
    "work/src/game/audio/audio.c": "A2 audio: libultra's [1] variable-length arrays (strict flex arrays only)",
    "work/src/libultra/audio/csplayer.c": "A2 audio: libultra's [1] variable-length arrays (strict flex arrays only)",
    "work/src/libultra/audio/seqplayer.c": "A2 audio: libultra's [1] variable-length arrays (strict flex arrays only)",
}


def where(lines):
    for ln in lines:
        m = frame.search(ln)
        if m and ("/work/src/" in m.group(2) or "/port/" in m.group(2) or "/game/" in m.group(2)):
            return f"{m.group(1)} {short(m.group(2))}"
    for ln in lines:
        m = frame.search(ln)
        if m:
            return f"{m.group(1)} {m.group(2)}"
    return "?"


def add(key, log, report, detail):
    f = findings.setdefault(key, {"count": 0, "logs": OrderedDict(), "report": report, "detail": detail})
    f["count"] += 1
    f["logs"][log] = f["logs"].get(log, 0) + 1


for log in args:
    try:
        lines = open(log, errors="replace").read().splitlines()
    except OSError as e:
        print(f"{log}: {e}", file=sys.stderr)
        continue
    i = 0
    while i < len(lines):
        ln = lines[i]
        m = asan_head.search(ln)
        u = ubsan_head.match(ln)
        o = own_head.match(ln)
        if m or u or o:
            j = i + 1
            while j < len(lines) and not (asan_head.search(lines[j]) or ubsan_head.match(lines[j]) or
                                          own_head.match(lines[j])) and not lines[j].startswith("SUMMARY:"):
                j += 1
            ends = j < len(lines) and lines[j].startswith("SUMMARY:")
            report = lines[i:j + 1] if ends else lines[i:j]
            if m:
                det = next((located.search(x).group(0) for x in report if located.search(x)), "")
                det = re.sub(r"'([^']*)'", lambda q: "'" + short(q.group(1)) + "'", det)
                det = re.sub(r"0x[0-9a-f]+", "", det).replace("(", "").replace(")", "")
                det = re.sub(r"located \d+ bytes", "located N bytes", det)
                key = ("asan " + m.group(1), where(report), det)
            elif u:
                path = short(u.group(1))
                msg = re.sub(r"-?\d+(\.\d+)?(e[+-]?\d+)?", "N", u.group(4))
                key = ("ubsan", f"{path}:{u.group(2)}", msg)
            else:
                kind = "rdram" if o.group(1).startswith("RDRAM") else "stub"
                key = (kind, where(report), re.sub(r"0x[0-9a-f]+|\d+", "N", o.group(1)))
            add(key, log, report, key[2])
            i = j + 1 if ends else j
            continue
        i += 1

known = 0
for n, (key, f) in enumerate(findings.items(), 1):
    logs = ", ".join(f"{l.split('/')[-1]}x{c}" for l, c in list(f["logs"].items())[:6])
    if len(f["logs"]) > 6:
        logs += f", ... {len(f['logs'])} logs"
    note = next((v for k, v in KNOWN.items() if k in key[1]), None)
    known += note is not None
    print(f"[{n}] {key[0]} at {key[1]}: {f['count']} ({logs})")
    if key[2]:
        print(f"     {key[2]}")
    if note:
        print(f"     known: {note}")
    if full:
        print("\n".join("     | " + x for x in f["report"][:40]))
print(f"{len(findings)} distinct findings ({known} known, {len(findings) - known} open)")
