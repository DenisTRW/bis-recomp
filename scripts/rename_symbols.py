"""Benennt Funktionen in allen dsd-symbols.txt um.

Aufruf: python -I rename_symbols.py <projektwurzel> alt=neu [alt=neu ...]
Gibt für jeden Namen aus, wie oft er ersetzt wurde (erwartet: genau 1).
"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
pairs = [a.split("=", 1) for a in sys.argv[2:]]
files = list((root / "config" / "eur" / "arm9").rglob("symbols.txt"))
counts = {old: 0 for old, _ in pairs}
for f in files:
    text = f.read_text()
    new_text = text
    for old, new in pairs:
        new_text, n = re.subn(rf"(?m)^{re.escape(old)} ", f"{new} ", new_text)
        counts[old] += n
    if new_text != text:
        f.write_text(new_text)
for old, new in pairs:
    print(f"{old} -> {new}: {counts[old]}")
