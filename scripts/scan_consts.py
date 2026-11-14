"""Sucht 32-bit-Konstanten (z. B. Hardware-Register) in ARM9 + Overlays
und ordnet jeden Fund der Funktion zu, in der er liegt (laut dsd symbols.txt).

Aufruf: python -I scan_consts.py <projektwurzel> 0x04000130 0x027FFFA8 ...
"""
import re, struct, sys
from pathlib import Path

root = Path(sys.argv[1])
targets = {int(x, 16): x for x in sys.argv[2:]}
ext = root / "extract" / "eur"
cfg = root / "config" / "eur" / "arm9"

sym_re = re.compile(r"^(\S+) kind:function\((\w+),size=(0x[0-9a-fA-F]+)\) addr:(0x[0-9a-fA-F]+)")

def load_funcs(path):
    funcs = []
    for line in path.read_text().splitlines():
        m = sym_re.match(line)
        if m:
            name, mode, size, addr = m.groups()
            funcs.append((int(addr, 16), int(size, 16), name, mode))
    return funcs

modules = [("main", ext / "arm9" / "arm9.bin", 0x02004000, cfg / "symbols.txt")]
ov_yaml = (ext / "arm9_overlays" / "overlays.yaml").read_text()
for m in re.finditer(r"- id: (\d+)\n\s+base_address: (\d+)", ov_yaml):
    oid, base = int(m.group(1)), int(m.group(2))
    modules.append((f"ov{oid:03d}", ext / "arm9_overlays" / f"ov{oid:03d}.bin", base,
                    cfg / "overlays" / f"ov{oid:03d}" / "symbols.txt"))

for name, binp, base, symp in modules:
    data = binp.read_bytes()
    funcs = load_funcs(symp) if symp.exists() else []
    for off in range(0, len(data) - 3, 4):
        v = struct.unpack_from("<I", data, off)[0]
        if v in targets:
            addr = base + off
            owner = next((f for f in funcs if f[0] <= addr < f[0] + f[1]), None)
            # Literal-Pools liegen oft direkt hinter der Funktion
            if owner is None:
                prev = [f for f in funcs if f[0] + f[1] <= addr]
                owner = max(prev, key=lambda f: f[0]) if prev else None
                where = f"nach {owner[2]} ({owner[3]}, 0x{owner[0]:08x})" if owner else "?"
            else:
                where = f"in {owner[2]} ({owner[3]}, 0x{owner[0]:08x}, size 0x{owner[1]:x})"
            print(f"{targets[v]:>11}  {name:6} 0x{addr:08x}  {where}")
