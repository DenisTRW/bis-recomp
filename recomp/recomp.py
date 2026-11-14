"""Prototyp eines statischen Recompilers: ARM (aus `dsd dis`) -> C.

Eingabe:  build/asm/*.s           (dsd dis, Funktionsgrenzen + Labels + Literal-Pools)
          config/eur/arm9/**/symbols.txt (Namen -> Adressen)
Ausgabe:  build/recomp/<modul>.c  (eine C-Funktion pro ARM-Funktion)
          build/recomp/funcs.h    (Prototypen)
          build/recomp/functable.c (Adresse -> Funktion, für indirekte Aufrufe)

Aufruf:   python -I recomp/recomp.py <projektwurzel> [--only NAME[,NAME...]]

Grundidee: Jede ARM-Funktion wird eine C-Funktion `void rc_<name>(CPU *c)`, die auf
einem gemeinsamen CPU-Kontext (r0-r15, Flags) arbeitet. Speicherzugriffe gehen über
rd8/rd16/rd32/wr8/... der Plattform-Schicht. Rücksprünge werden zu `return`,
Aufrufe zu direkten C-Aufrufen, indirekte Aufrufe zu rc_call_indirect().
Nicht unterstützte Befehle werden als TODO markiert und gezählt.
"""
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

# ---------------------------------------------------------------- Grundlagen

REGS = {f"r{i}": i for i in range(16)}
REGS.update(sb=9, sl=10, fp=11, ip=12, sp=13, lr=14, pc=15)

CONDS = {
    "eq": "c->z", "ne": "!c->z",
    "cs": "c->cf", "hs": "c->cf", "cc": "!c->cf", "lo": "!c->cf",
    "mi": "c->n", "pl": "!c->n", "vs": "c->v", "vc": "!c->v",
    "hi": "(c->cf && !c->z)", "ls": "(!c->cf || c->z)",
    "ge": "(c->n == c->v)", "lt": "(c->n != c->v)",
    "gt": "(!c->z && c->n == c->v)", "le": "(c->z || c->n != c->v)",
    "al": None,
}
COND_RE = "|".join(k for k in CONDS)

DP_LOGIC = {"and": "&", "eor": "^", "orr": "|", "bic": "& ~"}
DP_ARITH = {"add", "adc", "sub", "sbc", "rsb", "rsc"}
DP_TEST = {"cmp", "cmn", "tst", "teq"}
DP_MOVE = {"mov", "mvn"}

# Mnemonic-Familien: (Basis, Regex für den Rest im Vor-UAL-Syntax von dsd)
FAMILIES = [
    ("smlal", rf"(?P<xy>bb|bt|tb|tt)?(?P<cond>{COND_RE})?(?P<s>s)?"),
    ("smull", rf"(?P<cond>{COND_RE})?(?P<s>s)?"),
    ("umull", rf"(?P<cond>{COND_RE})?(?P<s>s)?"),
    ("umlal", rf"(?P<cond>{COND_RE})?(?P<s>s)?"),
    ("smlaw", rf"(?P<y>b|t)(?P<cond>{COND_RE})?"),
    ("smulw", rf"(?P<y>b|t)(?P<cond>{COND_RE})?"),
    ("smla", rf"(?P<xy>bb|bt|tb|tt)(?P<cond>{COND_RE})?"),
    ("smul", rf"(?P<xy>bb|bt|tb|tt)(?P<cond>{COND_RE})?"),
    ("swpb", rf"(?P<cond>{COND_RE})?"),
    ("swp", rf"(?P<cond>{COND_RE})?(?P<b>b)?"),
    ("blx", rf"(?P<cond>{COND_RE})?"),
    ("bx", rf"(?P<cond>{COND_RE})?"),
    ("bl", rf"(?P<cond>{COND_RE})?"),
    ("b", rf"(?P<cond>{COND_RE})?"),
    ("ldm", rf"(?P<cond>{COND_RE})?(?P<mode>ia|ib|da|db|fd|ed|fa|ea)?"),
    ("stm", rf"(?P<cond>{COND_RE})?(?P<mode>ia|ib|da|db|fd|ed|fa|ea)?"),
    ("ldr", rf"(?P<cond>{COND_RE})?(?P<sz>sb|sh|bt|b|h|d|t)?"),
    ("str", rf"(?P<cond>{COND_RE})?(?P<sz>bt|b|h|d|t)?"),
    ("mla", rf"(?P<cond>{COND_RE})?(?P<s>s)?"),
    ("mul", rf"(?P<cond>{COND_RE})?(?P<s>s)?"),
    ("clz", rf"(?P<cond>{COND_RE})?"),
    ("mrs", rf"(?P<cond>{COND_RE})?"),
    ("msr", rf"(?P<cond>{COND_RE})?"),
    ("mcr", rf"(?P<cond>{COND_RE})?"),
    ("mrc", rf"(?P<cond>{COND_RE})?"),
    ("swi", rf"(?P<cond>{COND_RE})?"),
    ("svc", rf"(?P<cond>{COND_RE})?"),
] + [(op, rf"(?P<cond>{COND_RE})?(?P<s>s)?")
     for op in sorted(set(DP_LOGIC) | DP_ARITH | DP_TEST | DP_MOVE, key=len, reverse=True)]


def parse_mnemonic(mn):
    for base, rest_re in FAMILIES:
        if mn.startswith(base):
            m = re.fullmatch(rest_re, mn[len(base):])
            if m:
                d = {k: v for k, v in m.groupdict().items() if v}
                return base, d
    return None, {}


def split_ops(s):
    """Operanden an Kommas trennen, aber nicht innerhalb von [] oder {}."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def imm(s):
    s = s.strip().lstrip("#")
    return int(s, 0) & 0xFFFFFFFF if not s.startswith("-") else (-int(s[1:], 0)) & 0xFFFFFFFF


def cname(name):
    return "rc_" + re.sub(r"[^0-9A-Za-z_]", "_", name)


class Unsupported(Exception):
    pass


# ---------------------------------------------------------------- Symbole

def load_symbols(cfg):
    """Name -> (Adresse, Modul, Art) aus allen symbols.txt."""
    syms = {}
    pat = re.compile(r"^(\S+) kind:(\w+)(?:\(([^)]*)\))? addr:0x([0-9a-fA-F]+)")
    for f in cfg.rglob("symbols.txt"):
        mod = f.parent.name if f.parent.name.startswith("ov") else (
            "main" if f.parent.name == "arm9" else f.parent.name)
        for line in f.read_text().splitlines():
            m = pat.match(line)
            if m:
                name, kind, args, addr = m.groups()
                syms[name] = (int(addr, 16), mod, kind, args or "")
    return syms


# ---------------------------------------------------------------- Einlesen

class Insn:
    __slots__ = ("addr", "mn", "ops", "text")

    def __init__(self, addr, mn, ops, text):
        self.addr, self.mn, self.ops, self.text = addr, mn, ops, text


class Func:
    def __init__(self, name, mode, addr, src):
        self.name, self.mode, self.addr, self.src = name, mode, addr, src
        self.insns = []
        self.labels = {}      # Label-Name -> Adresse
        self.pool = {}        # Adresse -> Wert (int) oder Symbolname (str)
        self.end = addr


def read_functions(asm_dir):
    funcs = []
    for f in sorted(asm_dir.rglob("*.s")):
        cur = None
        addr = 0
        for raw in f.read_text(errors="replace").splitlines():
            line = raw.split(";")[0].rstrip()
            s = line.strip()
            if not s:
                continue
            if s.startswith(("arm_func_start", "thumb_func_start")):
                mode = "thumb" if s.startswith("thumb") else "arm"
                cur = Func(s.split()[1], mode, 0, f.name)
                continue
            if s.startswith(("arm_func_end", "thumb_func_end")):
                if cur:
                    cur.end = addr
                    funcs.append(cur)
                cur = None
                continue
            if cur is None:
                continue
            # Label (evtl. mit Inhalt in derselben Zeile)
            m = re.match(r"^([.\w@$]+):\s*(.*)$", s)
            if m:
                lab, rest = m.groups()
                lm = re.search(r"([0-9a-fA-F]{8})$", lab)
                if lab == cur.name:
                    am = re.search(r"; 0x([0-9a-fA-F]+)", raw)
                    addr = int(am.group(1), 16) if am else addr
                    cur.addr = addr
                elif lm:
                    addr = int(lm.group(1), 16)
                cur.labels[lab] = addr
                s = rest.strip()
                if not s:
                    continue
            if s.startswith(".word"):
                for v in split_ops(s[5:]):
                    try:
                        cur.pool[addr] = int(v, 0)
                    except ValueError:
                        cur.pool[addr] = v
                    addr += 4
                continue
            if s.startswith(".short") or s.startswith(".hword"):
                addr += 2 * len(split_ops(s.split(None, 1)[1]))
                continue
            if s.startswith(".byte"):
                addr += len(split_ops(s[5:]))
                continue
            if s.startswith("."):
                continue
            parts = s.split(None, 1)
            cur.insns.append(Insn(addr, parts[0].lower(),
                                  parts[1].strip() if len(parts) > 1 else "", s))
            addr += 4 if cur.mode == "arm" else 2
    return funcs


# ---------------------------------------------------------------- Übersetzung

class Translator:
    def __init__(self, syms, func):
        self.syms = syms
        self.f = func
        self.targets = set()
        self.todo = Counter()
        self.lines = []

    # --- Operanden
    def reg(self, s, insn, read=True):
        s = s.strip().rstrip("!")
        if s not in REGS:
            raise Unsupported(f"Register {s}")
        n = REGS[s]
        if n == 15 and read:
            return f"0x{(insn.addr + 8) & 0xFFFFFFFF:08x}u"
        return f"c->r[{n}]"

    def label_addr(self, s):
        s = s.strip()
        if s in self.f.labels:
            return self.f.labels[s]
        m = re.fullmatch(r"\.L_([0-9a-fA-F]{8})", s)
        if m:
            return int(m.group(1), 16)
        return None

    def shifter(self, ops, insn, want_carry=False):
        """Operand 2 -> (C-Ausdruck, Carry-Ausdruck oder None)."""
        if ops[0].startswith("#"):
            return f"0x{imm(ops[0]):08x}u", None
        val = self.reg(ops[0], insn)
        if len(ops) == 1:
            return val, None
        sh = ops[1].strip().split()
        kind = sh[0].lower()
        if kind == "rrx":
            return (f"(({val} >> 1) | (c->cf << 31))", f"({val} & 1)")
        amt = sh[1]
        if amt.startswith("#"):
            n = imm(amt)
            if kind == "lsl":
                return (f"({val} << {n})" if n else val,
                        f"(({val} >> {32 - n}) & 1)" if n else None)
            if kind == "lsr":
                n = n or 32
                return (f"({val} >> {n})" if n < 32 else "0u",
                        f"(({val} >> {n - 1}) & 1)")
            if kind == "asr":
                n = n or 32
                return (f"(uint32_t)((int32_t){val} >> {min(n, 31)})",
                        f"(({val} >> {min(n - 1, 31)}) & 1)")
            if kind == "ror":
                return (f"ROR32({val}, {n})", f"(({val} >> {n - 1}) & 1)")
        else:  # Verschiebung um Register
            r = self.reg(amt, insn)
            fn = {"lsl": "LSL_R", "lsr": "LSR_R", "asr": "ASR_R", "ror": "ROR_R"}[kind]
            return (f"{fn}({val}, {r})", f"{fn}_C({val}, {r}, c->cf)")
        raise Unsupported(f"Shift {ops[1]}")

    def address(self, mem, insn):
        """[rn, #off] / [rn, rm, lsl #k] / [rn], #off -> (Adress-Ausdruck, Writeback-Code)."""
        post = None
        m = re.fullmatch(r"\[(.*)\](!?)(?:\s*,\s*(.*))?", mem.strip())
        if not m:
            raise Unsupported(f"Adressierung {mem}")
        inner, wb, post = m.groups()
        parts = split_ops(inner)
        if REGS.get(parts[0].strip()) == 15 and (wb or post):
            raise Unsupported("Writeback auf pc")
        base = self.reg(parts[0], insn)
        off = "0"
        def offset(p):
            neg = p[0].startswith("-")
            if neg:
                p = [p[0][1:]] + p[1:]
            if p[0].startswith("#"):
                v = imm(p[0])
                return f"-0x{v:x}u" if neg else f"0x{v:x}u"
            e, _ = self.shifter(p, insn)
            return f"-({e})" if neg else f"{e}"
        if post:
            o = offset(split_ops(post))
            return base, f"{base} = {base} + {o};"
        if len(parts) > 1:
            off = offset(parts[1:])
        a = f"({base} + {off})" if off != "0" else base
        if wb:
            return a, f"{base} = {a};"
        return a, None

    def reglist(self, s):
        s = s.strip()
        user = s.endswith("^")
        s = s.rstrip("^").strip().strip("{}")
        regs = []
        for part in split_ops(s):
            if "-" in part:
                a, b = part.split("-")
                regs += list(range(REGS[a.strip()], REGS[b.strip()] + 1))
            else:
                regs.append(REGS[part.strip()])
        return sorted(regs), user

    # --- Ausgabe
    def emit(self, s):
        self.lines.append(s)

    def call(self, target, insn, tail=False):
        tgt = target.strip()
        if tgt in self.syms and self.syms[tgt][2] == "function":
            code = f"{cname(tgt)}(c);"
        else:
            a = self.label_addr(tgt)
            if a is not None and self.f.addr <= a < self.f.end:
                return f"goto I_{a:08x};"
            raise Unsupported(f"Sprungziel {tgt}")
        if tail:
            return code + " return;"
        return f"c->r[14] = 0x{insn.addr + 4:08x}u; " + code

    # --- Ein Befehl
    def translate(self, insn):
        base, d = parse_mnemonic(insn.mn)
        if base is None:
            raise Unsupported(f"Mnemonic {insn.mn}")
        ops = split_ops(insn.ops)
        cond = d.get("cond", "al")
        body = self.body(base, d, ops, insn)
        if CONDS[cond]:
            return [f"if ({CONDS[cond]}) {{ {body} }}"]
        return [body]

    def body(self, base, d, ops, insn):
        S = "s" in d
        # ---- Sprünge
        if base == "b":
            a = self.label_addr(ops[0])
            if a is not None and self.f.addr <= a < self.f.end:
                self.targets.add(a)
                return f"goto I_{a:08x};"
            if ops[0].startswith("#"):
                raise Unsupported("b mit Roh-Offset")
            return self.call(ops[0], insn, tail=True)
        if base == "bl":
            return self.call(ops[0], insn)
        if base == "blx":
            if ops[0] in REGS:
                return (f"{{ uint32_t t = {self.reg(ops[0], insn)}; "
                        f"c->r[14] = 0x{insn.addr + 4:08x}u; rc_call_indirect(c, t); }}")
            return self.call(ops[0], insn)
        if base == "bx":
            if ops[0] == "lr":
                return "return;"
            return f"rc_call_indirect(c, {self.reg(ops[0], insn)}); return;"

        # ---- Datenverarbeitung
        if base in DP_MOVE or base in DP_LOGIC or base in DP_ARITH or base in DP_TEST:
            return self.dataproc(base, S, ops, insn)

        # ---- Multiplikation
        if base == "mul":
            rd, rm, rs = (self.reg(x, insn) for x in ops)
            return f"{rd} = {rm} * {rs};" + (f" NZ({rd});" if S else "")
        if base == "mla":
            rd, rm, rs, rn = (self.reg(x, insn) for x in ops)
            return f"{rd} = {rm} * {rs} + {rn};" + (f" NZ({rd});" if S else "")
        if base in ("umull", "smull", "umlal", "smlal") and "xy" not in d:
            lo, hi, rm, rs = (self.reg(x, insn) for x in ops)
            t = "uint64_t" if base[0] == "u" else "int64_t"
            cast = "(uint64_t)" if base[0] == "u" else "(int64_t)(int32_t)"
            acc = f" + (({t}){hi} << 32 | {lo})" if base.endswith("lal") else ""
            s = (f"{{ {t} p = ({t})({cast}{rm} * {cast}{rs}){acc}; "
                 f"{lo} = (uint32_t)p; {hi} = (uint32_t)((uint64_t)p >> 32);")
            if S:
                s += f" c->n = {hi} >> 31; c->z = ({lo} | {hi}) == 0;"
            return s + " }"
        if base in ("smul", "smla"):
            x, y = d["xy"][0], d["xy"][1]
            half = lambda r, h: (f"(int16_t)({r} >> 16)" if h == "t" else f"(int16_t){r}")
            rd, rm, rs = (self.reg(v, insn) for v in ops[:3])
            p = f"((int32_t){half(rm, x)} * (int32_t){half(rs, y)})"
            if base == "smla":
                return f"{rd} = (uint32_t)({p} + (int32_t){self.reg(ops[3], insn)});"
            return f"{rd} = (uint32_t){p};"
        if base in ("smulw", "smlaw"):
            y = d["y"]
            rd, rm, rs = (self.reg(v, insn) for v in ops[:3])
            h = f"(int16_t)({rs} >> 16)" if y == "t" else f"(int16_t){rs}"
            p = f"(int32_t)(((int64_t)(int32_t){rm} * {h}) >> 16)"
            if base == "smlaw":
                return f"{rd} = (uint32_t)({p} + (int32_t){self.reg(ops[3], insn)});"
            return f"{rd} = (uint32_t){p};"
        if base == "clz":
            rd, rm = (self.reg(x, insn) for x in ops)
            return f"{rd} = CLZ32({rm});"

        # ---- Speicher
        if base in ("ldr", "str"):
            return self.loadstore(base, d.get("sz", ""), ops, insn)
        if base in ("ldm", "stm"):
            return self.multi(base, d.get("mode", "ia"), ops, insn)
        if base in ("swp", "swpb"):
            rd, rm = self.reg(ops[0], insn), self.reg(ops[1], insn)
            rn = self.reg(ops[2].strip("[]"), insn)
            b = base == "swpb" or "b" in d
            rd_, wr = ("rd8", "wr8") if b else ("rd32", "wr32")
            return f"{{ uint32_t t = {rd_}({rn}); {wr}({rn}, {rm}); {rd} = t; }}"

        # ---- System
        if base in ("swi", "svc"):
            return f"rc_swi(c, 0x{imm(ops[0]):x});"
        if base == "mrs":
            return f"{self.reg(ops[0], insn)} = rc_mrs(c, {int(ops[1].lower() == 'spsr')});"
        if base == "msr":
            field = ops[0].lower()
            src = (f"0x{imm(ops[1]):08x}u" if ops[1].startswith("#")
                   else self.reg(ops[1], insn))
            return f'rc_msr(c, "{field}", {src});'
        if base in ("mcr", "mrc"):
            # mcr p15, 0, r0, c7, c10, 4  -> Cache/MPU: auf dem PC ohne Wirkung
            cp = ops[0]
            rd = self.reg(ops[2], insn, read=(base == "mcr"))
            regs_ = ", ".join(str(int(re.sub(r"\D", "", x) or 0)) for x in ops[1:2] + ops[3:])
            if base == "mrc":
                return f"{rd} = rc_mrc(c, {regs_});"
            return f"rc_mcr(c, {regs_}, {rd});"
        raise Unsupported(f"Befehl {base}")

    def dataproc(self, op, S, ops, insn):
        if op in DP_TEST:
            rn = self.reg(ops[0], insn)
            o2, carry = self.shifter(ops[1:], insn, want_carry=True)
            if op == "cmp":
                return f"SUB_FLAGS({rn}, {o2});"
            if op == "cmn":
                return f"ADD_FLAGS({rn}, {o2});"
            e = f"({rn} & {o2})" if op == "tst" else f"({rn} ^ {o2})"
            return f"{{ uint32_t t = {e}; NZ(t);" + (f" c->cf = {carry};" if carry else "") + " }"

        rd_name = ops[0].strip()
        writes_pc = REGS.get(rd_name) == 15
        if op in DP_MOVE:
            o2, carry = self.shifter(ops[1:], insn, want_carry=S)
            e = o2 if op == "mov" else f"~({o2})"
            if writes_pc:
                if op == "mov" and ops[1].strip() == "lr":
                    return "return;"
                return f"rc_call_indirect(c, {e}); return;"
            rd = self.reg(rd_name, insn, read=False)
            if S:
                return (f"{{ uint32_t t = {e};" + (f" c->cf = {carry};" if carry else "")
                        + f" {rd} = t; NZ(t); }}")
            return f"{rd} = {e};"

        rn = self.reg(ops[1], insn)
        o2, carry = self.shifter(ops[2:], insn, want_carry=S)
        if writes_pc:
            # Sprungtabelle:  addls pc, pc, rN, lsl #2
            if op == "add" and ops[1].strip() == "pc" and len(ops) == 4 and ops[3].replace(" ", "") == "lsl#0x2":
                return self.jumptable(ops[2].strip(), insn)
            raise Unsupported("Schreiben auf pc")
        rd = self.reg(rd_name, insn, read=False)
        if op in DP_LOGIC:
            e = f"({rn} {DP_LOGIC[op]}({o2}))"
            if S:
                return (f"{{ uint32_t t = {e};" + (f" c->cf = {carry};" if carry else "")
                        + f" {rd} = t; NZ(t); }}")
            return f"{rd} = {e};"
        # arithmetisch
        a, b = (rn, o2) if op not in ("rsb", "rsc") else (o2, rn)
        if op in ("add", "adc"):
            cin = "c->cf" if op == "adc" else "0"
            if S:
                return f"{rd} = ADC_FLAGS({a}, {b}, {cin});"
            return f"{rd} = {a} + {b}" + (" + c->cf;" if op == "adc" else ";")
        cin = "c->cf" if op in ("sbc", "rsc") else "1"
        if S:
            return f"{rd} = SBC_FLAGS({a}, {b}, {cin});"
        if op in ("sbc", "rsc"):
            return f"{rd} = {a} - {b} - !c->cf;"
        return f"{rd} = {a} - {b};"

    def jumptable(self, idx_reg, insn):
        """mwcc: cmp rN, #k / addls pc, pc, rN, lsl #2 / b default / b case0 ..."""
        # Obergrenze aus dem vorherigen cmp holen
        prev = [i for i in self.f.insns if i.addr < insn.addr][-4:]
        limit = None
        for p in reversed(prev):
            if p.mn == "cmp":
                o = split_ops(p.ops)
                if o[0].strip() == idx_reg and o[1].startswith("#"):
                    limit = imm(o[1])
                break
        if limit is None:
            raise Unsupported("Sprungtabelle ohne Grenze")
        r = self.reg(idx_reg, insn)
        cases = []
        for k in range(limit + 1):
            a = insn.addr + 8 + 4 * k
            self.targets.add(a)
            cases.append(f"case {k}: goto I_{a:08x};")
        return f"switch ({r}) {{ {' '.join(cases)} }}"

    def loadstore(self, op, sz, ops, insn):
        rt_name = ops[0].strip()
        # Literal-Pool:  ldr rX, .L_xxx
        if op == "ldr" and len(ops) == 2 and not ops[1].startswith("["):
            a = self.label_addr(ops[1])
            if a is None or a not in self.f.pool:
                raise Unsupported(f"Pool {ops[1]}")
            v = self.f.pool[a]
            if isinstance(v, str):
                if v in self.syms:
                    v = self.syms[v][0]
                else:
                    m = re.search(r"([0-9a-fA-F]{8})$", v)
                    if not m:
                        raise Unsupported(f"Pool-Symbol {v}")
                    v = int(m.group(1), 16)
            if REGS.get(rt_name) == 15:
                raise Unsupported("ldr pc aus Pool")
            return f"{self.reg(rt_name, insn, read=False)} = 0x{v:08x}u;"
        mem = ",".join(ops[1:]) if sz != "d" else ",".join(ops[2:])
        addr, wb = self.address(mem, insn)
        wb = (" " + wb) if wb else ""
        if sz == "d":
            r1 = self.reg(ops[0], insn, read=False)
            r2 = self.reg(ops[1], insn, read=False)
            if op == "ldr":
                return f"{{ uint32_t a = {addr}; {r1} = rd32(a); {r2} = rd32(a + 4);{wb} }}"
            return f"{{ uint32_t a = {addr}; wr32(a, {r1}); wr32(a + 4, {r2});{wb} }}"
        if op == "ldr":
            fn = {"": "rd32", "b": "rd8", "h": "rd16", "sb": "(uint32_t)(int8_t)rd8",
                  "sh": "(uint32_t)(int16_t)rd16", "t": "rd32", "bt": "rd8"}[sz]
            if REGS.get(rt_name) == 15:
                return f"{{ uint32_t t = {fn}({addr});{wb} rc_jump_indirect(c, t); return; }}"
            rt = self.reg(rt_name, insn, read=False)
            return f"{{ uint32_t t = {fn}({addr});{wb} {rt} = t; }}"
        fn = {"": "wr32", "b": "wr8", "h": "wr16", "t": "wr32", "bt": "wr8"}[sz]
        return f"{fn}({addr}, {self.reg(rt_name, insn)});{wb}"

    def multi(self, op, mode, ops, insn):
        base_s = ops[0].strip()
        wb = base_s.endswith("!")
        base = self.reg(base_s.rstrip("!"), insn)
        regs, _user = self.reglist(",".join(ops[1:]))
        n = len(regs)
        # Stack-Aliase
        mode = {("ldm", "fd"): "ia", ("ldm", "ed"): "ib", ("ldm", "fa"): "da", ("ldm", "ea"): "db",
                ("stm", "fd"): "db", ("stm", "ed"): "da", ("stm", "fa"): "ib", ("stm", "ea"): "ia"
                }.get((op, mode), mode)
        start = {"ia": "0", "ib": "4", "da": f"-{4 * n - 4}", "db": f"-{4 * n}"}[mode]
        new_base = f"{base} + {4 * n}" if mode in ("ia", "ib") else f"{base} - {4 * n}"
        out = [f"{{ uint32_t a = {base} + ({start});"]
        returns = False
        for i, r in enumerate(regs):
            if op == "stm":
                val = f"0x{insn.addr + 12:08x}u" if r == 15 else f"c->r[{r}]"
                out.append(f"wr32(a + {4 * i}, {val});")
            elif r == 15:
                returns = True
            else:
                out.append(f"c->r[{r}] = rd32(a + {4 * i});")
        if wb and not (op == "ldm" and REGS[base_s.rstrip("!")] in regs):
            out.append(f"{base} = {new_base};")
        if returns:
            if REGS[base_s.rstrip("!")] != 13:
                out.append(f"rc_jump_indirect(c, rd32(a + {4 * (n - 1)})); return;")
            else:
                out.append("return;")
        out.append("}")
        return " ".join(out)

    # --- Ganze Funktion
    def run(self):
        f = self.f
        body = []
        if f.mode == "thumb":
            # Nur BIOS-Stubs:  swi N / bx lr
            sw = [i for i in f.insns if i.mn == "swi"]
            if sw and all(i.mn in ("swi", "bx") for i in f.insns):
                body.append(f"rc_swi(c, 0x{imm(sw[0].ops):x}); return;")
            else:
                self.todo["Thumb-Funktion"] += 1
                body.append("RC_TODO(\"Thumb-Funktion\");")
        else:
            for insn in f.insns:
                try:
                    code = self.translate(insn)
                    body.append((insn, code))
                except (Unsupported, KeyError, ValueError, IndexError) as e:
                    msg = str(e) if isinstance(e, Unsupported) else f"{type(e).__name__}"
                    self.todo[msg.split(" ")[0] if not isinstance(e, Unsupported) else msg] += 1
                    body.append((insn, [f'RC_TODO("{insn.text}");']))
        self.emit(f"/* {f.name} @ 0x{f.addr:08x} ({f.src}) */")
        self.emit(f"void {cname(f.name)}(CPU *c) {{")
        for item in body:
            if isinstance(item, str):
                self.emit("    " + item)
                continue
            insn, code = item
            if insn.addr in self.targets:
                self.emit(f"I_{insn.addr:08x}:")
            self.emit(f"    /* {insn.addr:08x}: {insn.text:<40} */ " + " ".join(code))
        # Ziele hinter dem letzten Befehl / Fall-through
        self.emit(f"    rc_fallthrough(c, 0x{f.end:08x}u);")
        self.emit("}")
        return self.lines


# ---------------------------------------------------------------- Hauptprogramm

def module_of(func, syms):
    s = syms.get(func.name)
    return s[1] if s else "unknown"


def main():
    root = Path(sys.argv[1])
    only = None
    if "--only" in sys.argv:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
    syms = load_symbols(root / "config" / "eur" / "arm9")
    funcs = read_functions(root / "build" / "asm")
    out = root / "build" / "recomp"
    if "--out" in sys.argv:
        out = Path(sys.argv[sys.argv.index("--out") + 1])
    out.mkdir(parents=True, exist_ok=True)

    per_mod = defaultdict(list)
    todo_total = Counter()
    stats = Counter()
    for fn in funcs:
        if only and fn.name not in only:
            continue
        t = Translator(syms, fn)
        lines = t.run()
        per_mod[module_of(fn, syms)].append((fn, lines))
        n_todo = sum(t.todo.values())
        stats["funcs"] += 1
        stats["insns"] += len(fn.insns)
        stats["todo_insns"] += n_todo
        if n_todo == 0:
            stats["funcs_ok"] += 1
        todo_total.update(t.todo)

    protos = []
    table = []
    for mod, items in sorted(per_mod.items()):
        with open(out / f"{mod}.c", "w", encoding="utf-8") as fh:
            fh.write('#include "recomp.h"\n#include "funcs.h"\n\n')
            for fn, lines in items:
                fh.write("\n".join(lines) + "\n\n")
                protos.append(f"void {cname(fn.name)}(CPU *c);")
                table.append((mod, fn.addr, cname(fn.name)))
    (out / "funcs.h").write_text("#pragma once\n#include \"recomp.h\"\n\n" + "\n".join(protos) + "\n")
    with open(out / "functable.c", "w", encoding="utf-8") as fh:
        fh.write('#include "recomp.h"\n#include "funcs.h"\n\n')
        fh.write("const RcFuncEntry rc_functable[] = {\n")
        for mod, addr, name in table:
            ov = -1 if not mod.startswith("ov") else int(mod[2:])
            fh.write(f"    {{ 0x{addr:08x}u, {ov}, {name} }},\n")
        fh.write("};\nconst unsigned rc_functable_count = sizeof(rc_functable) / sizeof(rc_functable[0]);\n")

    ok = stats["insns"] - stats["todo_insns"]
    print(f"Funktionen: {stats['funcs']}, davon vollständig übersetzt: {stats['funcs_ok']} "
          f"({100 * stats['funcs_ok'] / max(1, stats['funcs']):.1f} %)")
    print(f"Befehle: {stats['insns']}, übersetzt: {ok} ({100 * ok / max(1, stats['insns']):.2f} %)")
    print("Häufigste TODOs:")
    for k, v in todo_total.most_common(20):
        print(f"  {v:7}  {k}")


if __name__ == "__main__":
    main()
