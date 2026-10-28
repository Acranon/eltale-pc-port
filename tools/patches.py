#!/usr/bin/env python3
"""Keep PC-only game changes as a patch against the pinned decomp submodule.

    python3 tools/patches.py apply
    # edit work/src/
    python3 tools/patches.py save

The generated work copy is a separate Git repository with a `pristine` tag.
The submodule is never edited. `apply` refuses to replace an existing work copy.
"""

import argparse
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "decomp"
WORK = ROOT / "work"
PATCH = ROOT / "patches" / "p0.patch"


def run(*args: str, cwd: Path, input_bytes: bytes | None = None) -> bytes:
    return subprocess.run(args, cwd=cwd, input=input_bytes, check=True,
                          stdout=subprocess.PIPE).stdout


def apply() -> None:
    if not (DECOMP / ".git").exists() and not (DECOMP / ".git").is_file():
        raise SystemExit("decomp/ submodule is missing")
    if WORK.exists():
        raise SystemExit("work/ already exists; save its changes before replacing it")
    WORK.mkdir()
    shutil.copytree(DECOMP / "src", WORK / "src")
    run("git", "init", "-q", cwd=WORK)
    run("git", "config", "user.name", "patches.py", cwd=WORK)
    run("git", "config", "user.email", "patches.py@localhost", cwd=WORK)
    run("git", "config", "core.autocrlf", "false", cwd=WORK)
    run("git", "add", "src", cwd=WORK)
    rev = run("git", "-C", str(DECOMP), "rev-parse", "--short", "HEAD", cwd=WORK).decode().strip()
    run("git", "commit", "-qm", f"pristine decomp {rev}", cwd=WORK)
    run("git", "tag", "pristine", cwd=WORK)
    if PATCH.is_file():
        run("git", "apply", "--binary", str(PATCH), cwd=WORK)
    print(f"work/ ready from decomp {rev}")


def save() -> None:
    if not (WORK / ".git").exists():
        raise SystemExit("run apply first")
    run("git", "rev-parse", "refs/tags/pristine", cwd=WORK)
    run("git", "add", "-N", "src", cwd=WORK)
    diff = run("git", "diff", "--binary", "pristine", "--", "src", cwd=WORK)
    PATCH.parent.mkdir(exist_ok=True)
    PATCH.write_bytes(diff)
    print(f"saved {PATCH.relative_to(ROOT)} ({len(diff)} bytes)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("apply", "save"))
    args = parser.parse_args()
    globals()[args.command]()
