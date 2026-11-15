"""Ordnet die Stichproben von BIS_HOSTPROF (recomp/runtime/hostprof.c) den Funktionen des
PC-Programms zu (über nm) und zeigt die teuersten.

Aufruf: python -I recomp/tools/hostprof.py programm.exe profil.txt [anzahl]
"""
import bisect, subprocess, sys
from collections import Counter

exe, prof = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
syms = []
for line in subprocess.run(["nm", "-n", "-C", exe], capture_output=True, text=True).stdout.splitlines():
    p = line.split(maxsplit=2)
    if len(p) == 3 and p[1] in "tT":
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
image_base = 0x140000000   # Standard-Basis von MinGW-x64-Programmen (nm zeigt diese Adressen)
base, hits = None, Counter()
for line in open(prof):
    k, v = line.split()
    if k == "base":
        base = int(v, 16)
        continue
    if k.startswith("dll:"):
        hits["DLL " + k[4:]] += int(v)
        continue
    a = int(k, 16) - base + image_base
    i = bisect.bisect_right(addrs, a) - 1
    hits[syms[i][1] if i >= 0 and a - addrs[i] < 0x100000 else "(ausserhalb, z. B. DLL)"] += int(v)
total = sum(hits.values())
print(f"{total} Stichproben")
cum = 0
for name, n in hits.most_common(top):
    cum += n
    print(f"{n * 100 / total:6.2f}% {cum * 100 / total:6.1f}%  {name}")
