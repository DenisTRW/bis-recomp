"""Befehls-Statistik über die von `dsd dis` erzeugten .s-Dateien.

Zeigt, welche ARM-/Thumb-Befehle vorkommen und wie oft die für einen
Recompiler schwierigen Fälle auftreten (indirekte Sprünge, Sprungtabellen,
Coprozessor, Systembefehle ...).

Aufruf: python -I insn_stats.py <asm-ordner>
"""
import re
import sys
from collections import Counter
from pathlib import Path

asm_dir = Path(sys.argv[1])

COND = "eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al"
# Basis-Mnemonics, die Bedingungs-/Größen-Suffixe tragen können (längste zuerst)
BASES = sorted("""
adc add and asr b bic bl blx bx cdp clz cmn cmp eor ldc ldm ldmia ldmib ldmda ldmdb
ldr ldrb ldrh ldrsb ldrsh ldrd ldrt ldrbt lsl lsr mcr mcrr mla mov mrc mrrc mrs msr
mul mvn neg orr pld pop push qadd qdadd qdsub qsub ror rsb rsc sbc smla smlal smlaw
smul smull smulw stc stm stmia stmib stmda stmdb str strb strh strd strt strbt sub
swi svc swp swpb teq tst umlal umull bkpt
""".split(), key=len, reverse=True)

def base_of(mn):
    for b in BASES:
        if mn.startswith(b):
            rest = mn[len(b):]
            if rest == "" or re.fullmatch(rf"(s)?({COND})?(s)?(b|h|sb|sh|t|bt|d)?(ia|ib|da|db|fd|ed|fa|ea)?"
                                          rf"(bb|bt|tb|tt|wb|wt)?(s)?({COND})?", rest):
                return b
    return mn

mnem = {"arm": Counter(), "thumb": Counter()}
features = Counter()
examples = {}
funcs = Counter()
mode = "arm"

def feat(name, where):
    features[name] += 1
    examples.setdefault(name, where)

for f in sorted(asm_dir.rglob("*.s")):
    cur = None
    for line in f.read_text(errors="replace").splitlines():
        s = line.strip()
        if s.startswith("arm_func_start"):
            mode, cur = "arm", s.split()[1]; funcs["arm"] += 1; continue
        if s.startswith("thumb_func_start"):
            mode, cur = "thumb", s.split()[1]; funcs["thumb"] += 1; continue
        if not s or s.startswith((".", ";", "@")) or s.endswith(":") or ":" in s.split()[0]:
            continue
        parts = s.split(None, 1)
        mn = parts[0].lower()
        ops = parts[1].split(";")[0].strip().lower() if len(parts) > 1 else ""
        b = base_of(mn)
        mnem[mode][b] += 1
        where = f"{cur} ({f.name})"

        if b != mn and mode == "arm" and re.search(rf"({COND})", mn[len(b):]) and b not in ("b", "bl"):
            features["ARM: bedingt ausgeführte Befehle (außer Sprünge)"] += 1
        if b == "bx" and ops != "lr":
            feat("bx <reg> (indirekter Sprung/Aufruf)", where)
        if b == "blx" and not ops.startswith(("#", "func_", "0x")) and re.match(r"r\d|ip|lr", ops):
            feat("blx <reg> (indirekter Aufruf)", where)
        if b == "blx" and not re.match(r"r\d|ip|lr", ops):
            feat("blx <ziel> (ARM<->Thumb-Wechsel)", where)
        if b == "ldr" and ops.startswith("pc,"):
            feat("ldr pc, ... (Sprungtabelle/indirekt)", where)
        if b in ("mov", "add") and ops.startswith("pc,"):
            feat(f"{b} pc, ... (berechneter Sprung)", where)
        if b.startswith("ldm") and "pc" in ops:
            feat("ldm ... {pc} (Rücksprung)", where)
        if b == "pop" and "pc" in ops:
            feat("pop {pc} (Rücksprung)", where)
        if b in ("mrc", "mcr", "cdp", "ldc", "stc", "mcrr", "mrrc"):
            feat(f"Coprozessor ({b})", where)
        if b in ("mrs", "msr"):
            feat(f"Statusregister ({b})", where)
        if b in ("swi", "svc"):
            feat("swi (BIOS-Aufruf)", where)
        if b in ("swp", "swpb"):
            feat("swp (atomar)", where)
        if b.startswith(("qadd", "qsub", "qdadd", "qdsub", "smla", "smul")) and b not in ("smull", "smlal"):
            feat(f"ARMv5TE-DSP ({b})", where)
        if b in ("ldrd", "strd", "pld", "clz"):
            feat(f"ARMv5TE ({b})", where)

print(f"Funktionen: ARM {funcs['arm']}, Thumb {funcs['thumb']}")
for m in ("arm", "thumb"):
    total = sum(mnem[m].values())
    print(f"\n== {m.upper()}: {total} Befehle, {len(mnem[m])} verschiedene")
    for k, v in mnem[m].most_common():
        print(f"  {k:8} {v:8}  {100*v/total:5.1f}%")
print("\n== Besondere Fälle")
for k, v in sorted(features.items(), key=lambda kv: -kv[1]):
    ex = examples.get(k, "")
    print(f"  {v:7}  {k}" + (f"   z. B. {ex}" if ex else ""))
