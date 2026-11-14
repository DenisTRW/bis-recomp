"""Checks recomplauncher.json against the rules of the RecompLauncher schema
(https://recomplauncher.com/schema/recomplauncher.json, format 1) that this project uses.
This is not a full JSON Schema validator; it covers the fields used here.
Usage: python -I recomp/tools/check_recomplauncher.py recomplauncher.json"""
import json, re, sys

TOP = {"inputs", "$schema", "format", "name", "repo", "game", "start", "prepare", "config", "folders",
       "exit_codes", "settings", "groups", "saves", "games", "looks", "mods", "actions", "requires", "min"}
errors = []


def err(msg):
    errors.append(msg)


def check_places(name, places):
    if not isinstance(places, list) or not places:
        err(f"{name}: places list expected")
        return
    for p in places:
        if not isinstance(p, dict) or "path" not in p or set(p) - {"path", "if"}:
            err(f"{name}: entries need 'path' (and optional 'if')")


def main(path):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    if set(d) - TOP:
        err(f"unknown top-level fields: {sorted(set(d) - TOP)}")
    if d.get("format") != 1:
        err("format must be 1")
    if "repo" in d and not re.fullmatch(r"[^/\s]+/[^/\s]+", d["repo"]):
        err("repo must be owner/name")

    ids = set()
    for i, inp in enumerate(d.get("inputs", [])):
        where = f"inputs[{i}]"
        for k in ("id", "label"):
            if not inp.get(k):
                err(f"{where}: '{k}' required")
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", inp.get("id", "")):
            err(f"{where}: bad id")
        ids.add(inp.get("id"))
        if inp.get("role", "game") not in {"game", "disc", "dlc", "update", "key", "runtime"}:
            err(f"{where}: bad role")
        dest = inp.get("destination")
        if dest is not None and (dest.startswith("/") or dest.endswith("/") or any(c in dest for c in "\\:{}")
                                 or ".." in dest or "//" in dest):
            err(f"{where}: destination breaks the path rules")
        if "shared" not in inp:
            fm = inp.get("formats")
            if not fm:
                err(f"{where}: formats required without 'shared'")
            if inp.get("role", "game") not in {"game", "disc", "dlc", "update"}:
                err(f"{where}: role must be game/disc/dlc/update without 'shared'")
            for j, fmt in enumerate(fm or []):
                if set(fmt) - {"type", "extensions", "requires"} or fmt.get("type") not in {"file", "folder"}:
                    err(f"{where}.formats[{j}]: bad fields or type")
                ext = fmt.get("extensions", [])
                if len(ext) != len(set(ext)) or any(not re.fullmatch(r"[A-Za-z0-9]+", e) for e in ext):
                    err(f"{where}.formats[{j}]: extensions must be unique and alphanumeric")
    if len(d.get("inputs", [])) > 32:
        err("inputs: at most 32")

    st = d.get("start")
    if st is not None:
        if not isinstance(st.get("program"), list) or not st["program"]:
            err("start.program: list with at least one path required")
        for k in st.get("env", {}):
            if not re.fullmatch(r"[A-Za-z0-9_]+", k) or k.upper() in {"PATH"}:
                err(f"start.env: bad name {k}")
    pr = d.get("prepare")
    if pr is not None:
        if not pr.get("program"):
            err("prepare.program required")
        if "timeout" in pr and (not isinstance(pr["timeout"], int) or pr["timeout"] < 1):
            err("prepare.timeout must be an integer >= 1")

    for i, s in enumerate(d.get("settings", [])):
        where = f"settings[{i}]"
        if not s.get("id") or not s.get("label"):
            err(f"{where}: id and label required")
        if not any(k in s for k in ("choices", "slider", "bindings")):
            err(f"{where}: needs choices, slider or bindings")
        ch = s.get("choices")
        if ch is not None:
            if len(ch) < 2:
                err(f"{where}: at least two choices")
            for j, c in enumerate(ch):
                if not c.get("label"):
                    err(f"{where}.choices[{j}]: label required")
                if s.get("in") == "args" and not isinstance(c.get("args"), list):
                    err(f"{where}.choices[{j}]: args list expected for in=args")
        if "advanced" in s and not isinstance(s["advanced"], bool):
            err(f"{where}: advanced must be boolean")

    for name, places in d.get("folders", {}).items():
        check_places(f"folders.{name}", places)
    if not all(isinstance(x, str) for x in d.get("saves", [])):
        err("saves: list of strings")
    if not all(isinstance(k, str) and isinstance(v, str) for k, v in d.get("exit_codes", {}).items()):
        err("exit_codes: map of strings")

    if errors:
        print("FEHLER:")
        for e in errors:
            print("  - " + e)
        return 1
    print(f"OK: {path} passt zu den geprüften Schema-Regeln")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "recomplauncher.json"))
