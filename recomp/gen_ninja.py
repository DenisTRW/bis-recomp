"""Erzeugt build/recomp/build.ninja zum Kompilieren des gesamten erzeugten Codes.

Aufruf: python -I recomp/gen_ninja.py <projektwurzel> [-O0|-O1|-O2]
Danach: tools\\ninja.exe -C build\\recomp
"""
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
opt = sys.argv[2] if len(sys.argv) > 2 else "-O1"
rc = root / "build" / "recomp"
inc = root / "recomp" / "include"
gcc = root / "tools" / "w64devkit" / "bin" / "gcc.exe"

def esc(p):
    return str(p).replace("$", "$$").replace(" ", "$ ").replace(":", "$:")

lines = [
    f"cflags = {opt} -w -fno-strict-aliasing -I{inc} -I{rc}",
    "rule cc",
    f"  command = {gcc} $cflags -c $in -o $out",
    "  description = CC $in",
    "",
]
objs = []
for c in sorted(rc.glob("*.c")):
    o = rc / "obj" / (c.stem + ".o")
    objs.append(o)
    lines.append(f"build {esc(o)}: cc {esc(c)}")
lines.append("")
lines.append(f"build all: phony {' '.join(esc(o) for o in objs)}")
lines.append("default all")
(rc / "build.ninja").write_text("\n".join(lines) + "\n")
print(f"{len(objs)} Dateien, Optimierung {opt}")
