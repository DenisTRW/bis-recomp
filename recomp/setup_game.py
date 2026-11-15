"""Builds the PC version of the game from the player's own ROM (no window).

Used as the RecompLauncher "prepare" step and usable by hand. It does what
recomp/runtime/run_pc.bat does, without starting the game:

  1. extract the ROM with dsd            -> extract/eur
  2. disassemble with dsd                -> build/asm
  3. translate ARM to C (recomp.py)      -> build/recomp/*.c
  4. compile with Ninja + GCC            -> build/recomp/obj
  5. link the PC program with SDL3       -> build/pc/bis_pc.exe

Progress is printed as "progress <0..1> <text>" lines (RecompLauncher protocol);
the exit code is 0 on success. Nothing from the game is downloaded or shipped:
everything is made from the ROM given with --rom.

Usage: python -I recomp/setup_game.py --rom <path to .nds> [--root <project folder>]
"""
import argparse, hashlib, os, re, shutil, subprocess, sys, time
from pathlib import Path

SUPPORTED_SHA1 = "5F35BFBF79A0947740795DD770D99E8EEA3AC85E"   # Europe, CLJP, Revision 0


def progress(value, text):
    print(f"progress {value:.2f} {text}", flush=True)


def fail(msg, code=1):
    print(f"error: {msg}", file=sys.stderr, flush=True)
    sys.exit(code)


def run(cmd, cwd, env=None, on_line=None):
    """Runs a command without a window; returns its output. Fails with the output on error."""
    flags = 0x08000000 if os.name == "nt" else 0   # CREATE_NO_WINDOW
    p = subprocess.Popen([str(c) for c in cmd], cwd=cwd, env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, errors="replace", creationflags=flags)
    out = []
    for line in p.stdout:
        out.append(line)
        if on_line:
            on_line(line)
    p.wait()
    if p.returncode != 0:
        tail = "".join(out[-30:])
        fail(f"{Path(str(cmd[0])).name} failed (exit {p.returncode}):\n{tail}")
    return "".join(out)


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest().upper()


def find_sdl(root):
    for d in sorted((root / "tools").glob("SDL3-*"), reverse=True):
        mingw = d / "x86_64-w64-mingw32"
        if (mingw / "include" / "SDL3" / "SDL.h").exists():
            return mingw
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rom", required=True, help="your own ROM (.nds)")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent), help="project folder")
    ap.add_argument("--skip-check", action="store_true", help="do not check the ROM version")
    a = ap.parse_args()

    root = Path(a.root).resolve()
    rom = Path(a.rom).resolve()
    tools = root / "tools"
    dsd = tools / "dsd.exe"
    ninja = tools / "ninja.exe"
    devkit = tools / "w64devkit" / "bin"
    gcc = devkit / "gcc.exe"
    sdl = find_sdl(root)
    for need, what in ((rom, "ROM"), (dsd, "dsd"), (ninja, "Ninja"), (gcc, "GCC (w64devkit)")):
        if not need.exists():
            fail(f"{what} not found: {need}")
    if sdl is None:
        fail("SDL3 (MinGW build) not found in tools/SDL3-*")

    env = dict(os.environ)
    env["PATH"] = str(devkit) + os.pathsep + env.get("PATH", "")
    py = sys.executable
    t0 = time.time()

    progress(0.00, "Checking your game")
    if not a.skip_check and sha1_of(rom) != SUPPORTED_SHA1:
        fail("This ROM is not the supported version (Europe, CLJP, Revision 0).", 3)

    extract = root / "extract" / "eur"
    if not (extract / "arm9" / "arm9.bin").exists():
        progress(0.02, "Extracting game data")
        run([dsd, "rom", "extract", "--rom", rom, "--output-path", extract], root, env)

    progress(0.08, "Reading the game code")
    run([dsd, "dis", "--config-path", root / "config" / "eur" / "arm9" / "config.yaml",
         "--asm-path", root / "build" / "asm"], root, env)

    progress(0.15, "Translating the game code to C")
    run([py, "-I", root / "recomp" / "recomp.py", root], root, env)
    run([py, "-I", root / "recomp" / "gen_ninja.py", root, "-O1"], root, env)

    # Ninja prints "[done/total] ..." for every compiled file
    step = re.compile(r"\[(\d+)/(\d+)\]")
    last = [0.0]

    def on_ninja(line):
        m = step.search(line)
        if m:
            frac = int(m.group(1)) / max(1, int(m.group(2)))
            v = 0.20 + 0.70 * frac
            if v - last[0] >= 0.01:
                last[0] = v
                progress(v, f"Compiling ({m.group(1)}/{m.group(2)})")

    progress(0.20, "Compiling")
    run([ninja, "-C", root / "build" / "recomp"], root, env, on_ninja)

    progress(0.92, "Linking the PC version")
    rt = root / "recomp" / "runtime"
    out = root / "build" / "pc"
    out.mkdir(parents=True, exist_ok=True)
    sources = ["boot_test.c", "sched.c", "threads_hle.c", "arm7_hle.c", "card.c", "vram.c",
               "render2d.c", "gpu3d.c", "smc.c", "png.c", "hostprof.c", "sound.c", "frontend_sdl.c"]
    objs = sorted((root / "build" / "recomp" / "obj").glob("*.o"))
    if not objs:
        fail("no compiled objects found in build/recomp/obj")
    run([gcc, "-O2", "-w", "-DRC_TRACE", "-DPLATFORM_SDL",
         "-I", root / "recomp" / "include", "-I", root / "build" / "recomp", "-I", rt, "-I", sdl / "include",
         "-o", out / "bis_pc.exe", *[rt / s for s in sources], *objs, "-L", sdl / "lib", "-lSDL3", "-lwinmm"], root, env)
    shutil.copy2(sdl / "bin" / "SDL3.dll", out / "SDL3.dll")
    if (root / "recomplauncher.json").exists():
        shutil.copy2(root / "recomplauncher.json", out / "recomplauncher.json")

    progress(1.00, f"Done in {time.time() - t0:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
