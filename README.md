# bis-recomp

<p align="center">
  <img src="docs/logo.png" alt="Mario & Luigi BIS Recomp" width="460">
</p>

> [!IMPORTANT]
> **Built with AI:** The large majority of this project (recompiler, runtime, 2D/3D renderers,
> launcher, tests and documentation) was written by **Claude Code (Claude Opus 5.5)** by Anthropic,
> working together with and directed by [DenisTRW](https://github.com/DenisTRW).

A native PC port of **Mario & Luigi: Bowser's Inside Story** (Nintendo DS), built through
**static recompilation**: the game's ARM9 machine code is translated to C, compiled for the PC
and linked against a platform layer that reimplements the DS hardware (graphics, input, timers,
card, the ARM7 co-processor, …).

> **Status: early work in progress.** The game boots, shows the intro, the title screen and the
> file select screen (fully rendered, including 3D), and reacts to keyboard input. Starting a new
> game plays the opening scene in Toad Town. There is no sound yet.

## Progress

<!-- progress:start (written by scripts/progress.py) -->
<p align="center">
  <img src="docs/progress/recompiled.svg" alt="Game code recompiled"><br>
  <img src="docs/progress/functions.svg" alt="Functions fully recompiled"><br>
  <img src="docs/progress/playable.svg" alt="Playable">
</p>

**Recompiled** = how much of the game's machine code the recompiler already translates to C.
**Playable** = how far the game runs on PC (milestones below).

| | Milestone |
|---|---|
| ✅ | Boot, logos and intro |
| ✅ | Title screen |
| ✅ | File select |
| ✅ | New game: opening scene |
| ✅ | Walking and talking in the field |
| ⬜ | Battles |
| ⬜ | Sound and music |
| ⬜ | Touch screen and controller |
| ⬜ | Saving and loading in game |
| ⬜ | Full playthrough |
<!-- progress:end -->

> [!NOTE]
> **Early test release – not a finished game.** Releases contain only the tools (recompiler,
> runtime, launcher). There is no game data and no game code in this repository or in any release:
> the game is built on your PC from **your own ROM** during setup (about 2–3 minutes).

## Which ROM you need

Only this exact version works. The launcher and the setup check it and refuse anything else.

| | |
|---|---|
| Game | Mario & Luigi: Bowser's Inside Story (Nintendo DS) |
| Region | **Europe** (languages: English, French, German, Spanish, Italian) |
| Game code | `CLJP` (internal title `MARIO&LUIGI3`) |
| Revision | **0** |
| File size | **134,217,728 bytes** (128 MiB, untrimmed) |
| CRC32 | `9C2F90A6` |
| MD5 | `3C2334E0DEA72313D36B51A5418501F9` |
| SHA-1 | `5F35BFBF79A0947740795DD770D99E8EEA3AC85E` |

Not supported: the US version (`CLJE`), the Japanese version, other revisions and **trimmed**
ROMs (smaller files). Make the ROM from your own game card. To check your file on Windows:

```
certutil -hashfile "path\to\your.nds" SHA1
```
**This repository contains no game data.** You need your own copy of the game.

### Why there is no game to download

The recompiler translates the game's own machine code from **your** ROM into C, and that is
compiled into the PC program (`bis_pc.exe`). So the finished program contains the translated
game code – it is made from the game, not just from this project. This repository only
contains the translator (recompiler, runtime, launcher), never the result.

That is why everyone needs their own ROM, and why there will never be a ready-made game to
download. Releases contain only the tools; the game is built on your PC from your own ROM during
setup.

## Features so far

- Own ARM → C recompiler (`recomp/recomp.py`) working on the output of
  [dsd](https://github.com/AetiasHax/ds-decomp); 99.6 % of the ~12,200 functions translate fully
- Runtime with a deterministic thread scheduler (SDK threads run as fibers), interrupts, DMA,
  hardware divider/sqrt, card bus and VRAM banking
- ARM7 replacement (card/save, RTC, touch, sound command acknowledgement)
- 2D renderer (text backgrounds, sprites, blending, master brightness)
- Software 3D renderer (matrix stack, clipping, lighting, all 7 texture formats, alpha, Z/W buffer)
- SDL3 window at 60 fps with keyboard controls
- Unit tests for tricky recompiler cases (computed jumps, multi-entry functions, 64-bit division)
- **Launcher**: checks your ROM, shows your cover and screenshots, game and port info, has a settings
  page with options for the game, and starts it

## Launcher

![Launcher](docs/launcher.png)

`BISLauncher.exe` is a small launcher made just for this port:

- **Overview**: the game's title on top of a blurred cover backdrop, a large gallery with the
  cover and screenshots (arrows, thumbnails, arrow keys), **Launch game**, and below it the
  game description, information about the port and the controls.
- **ROM check**: verifies your copy (SHA-1), copies it to where the port expects it and runs the
  one-time setup (extracting the game data).
- **Settings** (gear icon, top left): window size, start in fullscreen, sharp or smooth pixels,
  card timing (now on by default), and launcher animations on/off. They are passed to the game as environment variables
  (`BIS_SCALE`, `BIS_FULLSCREEN`, `BIS_FILTER`, `BIS_CARD_DELAY`). Graphics enhancements, audio,
  controller support and the touch screen are listed as planned.
- **Animations**: the window fades in and builds up step by step, smooth scrolling, sections
  slide in when scrolled into view, gallery images slide in the direction you browse, and a slow
  zoom on the cover backdrop.

The cover (`screenshots/bis/cover.jpg` or `.png`) and screenshots are read from
`screenshots/bis` and are **not part of the repository** – take a photo of your own game case for
the cover; screenshots come from your own copy of the game (for example frames rendered by the
headless build in `build/frames`). Game output is written to `build/logs/bis.log`.

Build it with:

```
launcher\build.bat
```

It uses the C# compiler and WPF that ship with Windows (.NET Framework 4.x), so no SDK is needed.
The result is `BISLauncher.exe` in the repository root. Texts, ROM details, setup steps and the
launch command live in `launcher/game.json` (copied next to the executable). Paths are relative
to the launcher folder, `{rom}` expands to the absolute ROM target path, and arguments that name
an existing file or folder are passed as absolute paths. `BISLauncher.exe --snapshot out.png`  
renders the window to an image (used for the picture above).

## Building the game

Requirements (placed in `tools/`, not committed):

- [w64devkit](https://github.com/skeeto/w64devkit) (GCC for Windows) in `tools/w64devkit`
- [Ninja](https://ninja-build.org/) as `tools/ninja.exe`
- [dsd](https://github.com/AetiasHax/ds-decomp) as `tools/dsd.exe`
- [SDL 3](https://github.com/libsdl-org/SDL/releases) (MinGW build) in `tools/SDL3-<version>`
- Python 3

Steps:

1. Put your ROM at `orig/baserom.nds` (or select it in the launcher).
2. Extract it: `tools\dsd.exe rom extract --rom orig\baserom.nds --output-path extract\eur`
3. Build and run the PC version: `recomp\runtime\run_pc.bat`
4. Optional: run the tests with `recomp\test\run_tests.bat`

A headless build for debugging renders PNG frames to `build/frames` and prints a detailed
report: `recomp\runtime\run_boot.bat <frames> <memory-access budget in millions>`.

## Controls

| Key | DS |
|---|---|
| Arrow keys | D-Pad |
| X / C | A / B |
| S / D | X / Y |
| Q / W | L / R |
| Enter / Backspace | Start / Select |
| F11 / Esc | Fullscreen / Quit |

## How it works

1. **dsd** splits the ROM into the main program and its 141 overlays and finds function
   boundaries, symbols and relocations (`config/eur`).
2. **recomp.py** translates every ARM function into a C function operating on a shared CPU
   context. Indirect calls go through a function table; functions in overlays that share an
   address are resolved at runtime by checking which overlay is loaded.
3. The **runtime** (`recomp/runtime`) provides memory, I/O registers, DMA, interrupts, the thread
   scheduler, the ARM7 replacement, the card, VRAM, the 2D and 3D renderers and the SDL frontend.
   Every hardware replacement is reported as ORIGINAL (same behaviour), SIMULATED (simplified) or
   MISSING.

Detailed development notes (currently in German) are in [`docs/`](docs): boot process, threads,
ARM7, graphics, recompiler and copy protection.

## Roadmap

- [x] Recompiler, runtime, threads, card access
- [x] 2D and 3D rendering, SDL window, keyboard input
- [x] Title screen and file select screen
- [x] Launcher
- [x] Start a new game (opening scene in Toad Town)
- [x] RecompLauncher description (`recomplauncher.json`) for local builds, save files
- [ ] Touch screen via mouse, controller support
- [ ] Setup that builds the game on the player's PC from their own ROM (releases with tools only)
- [ ] Sound
- [ ] Remaining 3D features (shadows, fog, edge marking, anti-aliasing)
- [ ] First playable version

## License

The code in this repository is licensed under the
[GNU General Public License v3.0](LICENSE). This covers the project's own code (recompiler,
runtime, launcher, scripts, documentation) – not the game, which is not part of this repository.

## Legal

This project does not contain any code or data from the original game. The recompiled C code is
generated locally from your own ROM and is never committed. Mario & Luigi: Bowser's Inside Story
is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo.
