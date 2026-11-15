"""Updates the progress bars shown in the README (docs/progress/*.svg).

Two bars:
  - recompiled: share of the game's machine code that the recompiler translates to C, taken
    from a real recompiler run (needs build/asm, i.e. your own ROM; nothing of the game is
    stored, only the percentages)
  - playable: milestones below, maintained by hand

Usage: python -I scripts/progress.py          (runs the recompiler and writes the SVGs)
       python -I scripts/progress.py --no-run (only redraws with the last stored numbers)
"""
import json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "progress"

# (milestone, done) - keep in sync with the roadmap in the README
MILESTONES = [
    ("Boot, logos and intro", True),
    ("Title screen", True),
    ("File select", True),
    ("New game: opening scene", True),
    ("Walking and talking in the field", True),
    ("Battles", False),
    ("Sound and music", False),
    ("Touch screen and controller", False),
    ("Saving and loading in game", False),
    ("Full playthrough", False),
]


def run_recompiler():
    out = subprocess.run([sys.executable, "-I", str(ROOT / "recomp" / "recomp.py"), str(ROOT)],
                         capture_output=True, text=True, errors="replace").stdout
    f = re.search(r"Funktionen:\s*(\d+),[^:]*:\s*(\d+)", out)
    i = re.search(r"Befehle:\s*(\d+),[^:]*:\s*(\d+)", out)
    if not f or not i:
        sys.exit("could not read the recompiler statistics:\n" + out[-800:])
    return {"functions": int(f.group(1)), "functions_ok": int(f.group(2)),
            "instructions": int(i.group(1)), "instructions_ok": int(i.group(2))}


def bar(label, value, detail, color):
    """Small self-made progress bar (no external services)."""
    w, h, lw = 560, 28, 230
    fill = max(0.0, min(1.0, value)) * (w - lw)
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" role="img" aria-label="{label}: {value * 100:.1f}%">
  <title>{label}: {value * 100:.1f}% ({detail})</title>
  <rect width="{w}" height="{h}" rx="6" fill="#24292f"/>
  <rect x="{lw}" width="{w - lw}" height="{h}" rx="6" fill="#3d444d"/>
  <rect x="{lw}" width="{fill:.1f}" height="{h}" rx="6" fill="{color}"/>
  <text x="12" y="18.5" font-family="Segoe UI,Helvetica,Arial,sans-serif" font-size="13" font-weight="600" fill="#ffffff">{label}</text>
  <text x="{lw + 10}" y="18.5" font-family="Segoe UI,Helvetica,Arial,sans-serif" font-size="13" font-weight="600" fill="#ffffff">{value * 100:.1f}%  <tspan font-weight="400" fill="#e6e6e6">{detail}</tspan></text>
</svg>
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    stats_file = OUT / "stats.json"
    if "--no-run" in sys.argv and stats_file.exists():
        s = json.loads(stats_file.read_text())
    else:
        s = run_recompiler()
        stats_file.write_text(json.dumps(s, indent=2) + "\n")

    ins = s["instructions_ok"] / s["instructions"]
    fns = s["functions_ok"] / s["functions"]
    done = sum(1 for _, d in MILESTONES if d)
    (OUT / "recompiled.svg").write_text(
        bar("Game code recompiled", ins, f'{s["instructions_ok"]:,} / {s["instructions"]:,} instructions', "#2da44e"))
    (OUT / "functions.svg").write_text(
        bar("Functions fully recompiled", fns, f'{s["functions_ok"]:,} / {s["functions"]:,}', "#2f81f7"))
    (OUT / "playable.svg").write_text(
        bar("Playable", done / len(MILESTONES), f"{done} / {len(MILESTONES)} milestones", "#e5484d"))
    lines = "\n".join(f"| {'✅' if d else '⬜'} | {m} |" for m, d in MILESTONES)
    block = ("<!-- progress:start (written by scripts/progress.py) -->\n"
             '<p align="center">\n'
             '  <img src="docs/progress/recompiled.svg" alt="Game code recompiled"><br>\n'
             '  <img src="docs/progress/functions.svg" alt="Functions fully recompiled"><br>\n'
             '  <img src="docs/progress/playable.svg" alt="Playable">\n'
             "</p>\n\n"
             "**Recompiled** = how much of the game's machine code the recompiler already translates to C.\n"
             "**Playable** = how far the game runs on PC (milestones below).\n\n"
             "| | Milestone |\n|---|---|\n" + lines + "\n"
             "<!-- progress:end -->")
    readme = ROOT / "README.md"
    text = readme.read_text(encoding="utf-8")
    a, b = text.find("<!-- progress:start"), text.find("<!-- progress:end -->")
    if a < 0 or b < 0:
        sys.exit("README.md has no <!-- progress:start --> ... <!-- progress:end --> block")
    readme.write_text(text[:a] + block + text[b + len("<!-- progress:end -->"):], encoding="utf-8", newline="\n")
    print(f"recompiled {ins:.2%}, functions {fns:.2%}, playable {done}/{len(MILESTONES)}")


if __name__ == "__main__":
    main()
