"""Builds a release package that contains only tools - never game data or game code.

The package lets the RecompLauncher (or anyone) build the game from their own ROM:
recomplauncher.json runs recomp/setup_game.py as the setup step.

Contents:
  - all files tracked by git (recompiler, runtime, launcher sources, config, docs)
  - tools: dsd.exe, ninja.exe, w64devkit, SDL3 (x86_64 MinGW build), Python (embeddable)
  - THIRD_PARTY_NOTICES.md and the tools' license files

Safety: the script refuses to finish if anything that looks like game data or generated
game code ends up in the package (ROMs, extract/, build/, generated C, assembly).

Usage: python -I recomp/make_release.py --version 0.1.0
Output: build/release/bis-recomp-v<version>-Windows-x64.zip
"""
import argparse, re, subprocess, sys, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# ROMs and saves anywhere; generated output (build/), extracted data and local folders; anything
# in orig/ except the empty placeholder where the launcher later puts the player's ROM
FORBIDDEN = re.compile(r"(\.nds|\.srl|\.sav|\.dsv)$|^(extract|build|assets|ghidra|screenshots|saves)/|"
                       r"^orig/(?!(\.gitkeep|README\.md)$)", re.IGNORECASE)
# generated recompiler output never lives in the repo, but double-check its file names
GENERATED = re.compile(r"(^|/)(main|itcm|dtcm|ov\d{3})\.c$", re.IGNORECASE)


def git_files():
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return [f for f in out.splitlines() if f]


def tool_files(root):
    tools = root / "tools"
    sdl = sorted(tools.glob("SDL3-*"), reverse=True)
    picks = [tools / "dsd.exe", tools / "ninja.exe", tools / "w64devkit", tools / "python"]
    if sdl:
        picks += [sdl[0] / "x86_64-w64-mingw32", sdl[0] / "LICENSE.txt"]
    missing = [p for p in picks if not p.exists()]
    if missing:
        sys.exit("missing tools (see docs/release.md):\n  " + "\n  ".join(str(m) for m in missing))
    files = []
    for p in picks:
        files += [p] if p.is_file() else [f for f in p.rglob("*") if f.is_file()]
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    a = ap.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[A-Za-z]+\.\d+)?", a.version):
        sys.exit("version must look like 0.1.0")

    files = [(ROOT / f, f) for f in git_files()]
    files += [(f, f.relative_to(ROOT).as_posix()) for f in tool_files(ROOT)]

    rom_like = re.compile(r"\.(nds|srl|sav|dsv)$", re.IGNORECASE)
    bad = [arc for _, arc in files
           if rom_like.search(arc)                                   # anywhere, tools included
           or (not arc.startswith("tools/") and (FORBIDDEN.search(arc) or GENERATED.search(arc)))]
    if bad:
        sys.exit("refusing to package files that look like game data or game code:\n  " + "\n  ".join(bad[:20]))

    out = ROOT / "build" / "release" / f"bis-recomp-v{a.version}-Windows-x64.zip"
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for src, arc in files:
            z.write(src, arc)   # recomplauncher.json ends up at the top of the zip
    size = out.stat().st_size / (1 << 20)
    print(f"{out} ({len(files)} files, {size:.1f} MB)")


if __name__ == "__main__":
    main()
