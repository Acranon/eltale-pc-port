#!/usr/bin/env python3
"""List every integer-to-pointer conversion in the game's C sources, from clang's AST.

    python3 tools/find_addr_casts.py COMPILE_CMDS [--json out.json] [--check]

COMPILE_CMDS holds one compile command per line (`make -n` output: `clang ... -c file.c -o file.o`).
Each file is parsed with `-fsyntax-only -Xclang -ast-dump=json`, and every IntegralToPointer cast is
classed by what it converts:

  narrow   a 32-bit (or narrower) integer: an N64/image address that must go through PORT_PTR
  literal  an integer constant (not 0)
  wide     a 64-bit integer that did not come from a pointer (a uintptr_t variable, field or result)
  compare  a pointer as a 64-bit integer compared with, or offset by, a 32-bit address or a constant at
           or above 0x10000000 (Port_InRdram's kind of range check): on arm64 macOS the pointer has
           PORT_ADDR_HI in it and the 32-bit side does not

The address model (docs/MACOS.md): on arm64 macOS the image and RDRAM sit in a 4 GiB window higher up, so a
32-bit address only becomes a pointer through PORT_PTR. A cast written inside port/addr.h's macros is not
listed, nor is a comparison written there. Locations are where the cast is written, or where the macro
that holds it is used.

--check exits 1 when something needs looking at: a narrow cast, a literal in 0x10000000..0xFFFFFFFF, or
a compare, unless its line says it is not an address (a comment holding "not an address" or "a ROM
address"; MAP_FAILED is known). Wide casts are listed but not checked: follow where the value came from.
"""
import argparse
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OURS = ("work/src/", "port/", "game/")
NARROW = {"unsigned int", "int", "unsigned short", "short", "unsigned char", "char", "signed char",
          "_Bool", "bool", "unsigned"}
CASTS = {"ImplicitCastExpr", "CStyleCastExpr", "ParenExpr", "CXXFunctionalCastExpr", "CXXStaticCastExpr",
         "ConstantExpr"}


class Walker:
    def __init__(self):
        self.file = None
        self.line = None
        self.spelled = None
        self.hits = []

    def see_loc(self, loc):
        """Follow clang's JSON location compression: file and line are printed only when they change.
        self.spelled is the file the last location was written in (a macro's body, for an expansion)."""
        if not loc:
            return None
        if "spellingLoc" in loc or "expansionLoc" in loc:
            self.see_loc(loc.get("spellingLoc"))
            spelled = self.file
            at = self.see_loc(loc.get("expansionLoc"))
            self.spelled = spelled
            return at
        self.spelled = None
        if "file" in loc:
            self.file = loc["file"]
        if "line" in loc:
            self.line = loc["line"]
        if "col" not in loc:
            return None
        return (self.file, self.line, loc["col"])

    def walk(self, node):
        where = self.see_loc(node.get("loc"))
        spelled = self.spelled or self.file
        rng = node.get("range") or {}
        begin = self.see_loc(rng.get("begin"))
        self.see_loc(rng.get("end"))
        in_addr_h = bool(spelled) and spelled.endswith("port/addr.h")
        if node.get("castKind") == "IntegralToPointer" and not in_addr_h:
            self.classify(node, begin or where)
        if node.get("kind") == "BinaryOperator" and node.get("opcode") in COMPARE_OPS and not in_addr_h:
            self.compare(node, begin or where)
        for child in node.get("inner", ()):
            self.walk(child)

    def compare(self, node, where):
        if len(node.get("inner", ())) != 2:
            return
        a, b = node["inner"]
        for ptr_side, other in ((a, b), (b, a)):
            if qual(ptr_side) in NARROW or not came_from_pointer(ptr_side):
                continue
            lit = literal_of(other)
            if (lit is not None and 0x10000000 <= lit <= 0xFFFFFFFF) or (lit is None and has_narrow(other)):
                if where and where[0]:
                    self.hits.append(("compare", where[0], where[1], where[2], node.get("opcode"), None))
                return

    def classify(self, node, where):
        kinds = []
        x = node["inner"][0] if node.get("inner") else None
        from_pointer = False
        literal = None
        while x is not None:
            t = qual(x)
            if x.get("castKind") == "PointerToIntegral":
                from_pointer = True
            if t in NARROW or t.startswith("enum "):
                kinds.append("narrow")
            if x["kind"] == "IntegerLiteral":
                literal = x.get("value")
            if x["kind"] == "CallExpr" and callee(x) == "Port_HostPtr":
                return
            if x["kind"] in CASTS and x.get("inner"):
                x = x["inner"][0]
            else:
                break
        if literal is not None and literal != "0":
            kind = "literal"
        elif "narrow" in kinds:
            kind = "narrow"
        elif from_pointer or literal == "0":
            return
        else:
            kind = "wide"
        if where and where[0]:
            self.hits.append((kind, where[0], where[1], where[2], node.get("type", {}).get("qualType", ""), literal))


COMPARE_OPS = {"<", ">", "<=", ">=", "==", "!=", "-", "+"}


def chain(node):
    """node and the casts/parens under it, down to the first thing that is not a cast"""
    while node is not None:
        yield node
        if node["kind"] in CASTS and node.get("inner"):
            node = node["inner"][0]
        else:
            return


def came_from_pointer(node):
    return any(x.get("castKind") == "PointerToIntegral" for x in chain(node))


def has_narrow(node):
    return any(qual(x) in NARROW or qual(x).startswith("enum ") for x in chain(node))


def literal_of(node):
    for x in chain(node):
        if x["kind"] == "IntegerLiteral":
            try:
                return int(x.get("value"), 0)
            except (TypeError, ValueError):
                return None
    return None


def qual(node):
    t = node.get("type") or {}
    return t.get("desugaredQualType", t.get("qualType", ""))


def callee(call):
    x = call.get("inner", [None])[0]
    while x is not None:
        if x["kind"] == "DeclRefExpr":
            return x.get("referencedDecl", {}).get("name")
        x = x.get("inner", [None])[0]
    return None


MARKED = ("not an address", "a ROM address", "MAP_FAILED")


def needs_look(r):
    if any(m in r["text"] for m in MARKED):
        return False
    if r["kind"] in ("narrow", "compare"):
        return True
    if r["kind"] == "literal":
        try:
            v = int(r["value"], 0)
        except (TypeError, ValueError):
            return True
        return 0x10000000 <= v <= 0xFFFFFFFF
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("commands")
    ap.add_argument("--json")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    seen = set()
    rows = []
    for line in Path(args.commands).read_text().splitlines():
        argv = shlex.split(line)
        if "-c" not in argv:
            continue
        i = argv.index("-c")
        src = argv[i + 1]
        o = argv.index("-o")
        cmd = argv[:o] + argv[o + 2:]
        cmd.remove("-c")
        cmd = [a for a in cmd if a not in ("-MMD", "-MP", "-MD")]  # no .d files beside the tree
        cmd += ["-fsyntax-only", "-Xclang", "-ast-dump=json"]
        out = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if not out.stdout:
            print(f"[skip] {src}: no AST", file=sys.stderr)
            continue
        w = Walker()
        w.walk(json.loads(out.stdout))
        for kind, f, ln, col, ty, value in w.hits:
            rel = str(Path(f).resolve().relative_to(ROOT)) if Path(f).resolve().is_relative_to(ROOT) else f
            if not rel.startswith(OURS) and not rel.startswith("out/"):
                continue
            key = (kind, rel, ln, col)
            if key in seen:
                continue
            seen.add(key)
            text = ""
            try:
                text = (ROOT / rel).read_text(errors="replace").splitlines()[ln - 1]
            except (OSError, IndexError):
                pass
            rows.append({"kind": kind, "file": rel, "line": ln, "col": col, "type": ty, "text": text, "value": value})
    rows.sort(key=lambda r: (r["kind"], r["file"], r["line"], r["col"]))
    for r in rows:
        flag = "!" if needs_look(r) else " "
        print(f'{flag} {r["kind"]:8} {r["file"]}:{r["line"]}:{r["col"]}  {r["text"].strip()[:150]}')
    counts = {}
    for r in rows:
        counts[r["kind"]] = counts.get(r["kind"], 0) + 1
    print(counts, file=sys.stderr)
    if args.json:
        Path(args.json).write_text(json.dumps(rows, indent=1))
    if args.check:
        bad = [r for r in rows if needs_look(r)]
        print(f"{len(bad)} to look at (marked !)", file=sys.stderr)
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
