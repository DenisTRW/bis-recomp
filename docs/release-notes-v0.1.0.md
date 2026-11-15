**Early test release – not a finished game.** This package contains only the tools. There is no
game data and no game code in it: the game is built on your PC from **your own ROM** during setup
(about 2–3 minutes).

### What works

- Boot, logos and intro, title screen, file select (2D and 3D graphics)
- Starting a new game: the opening scene in Toad Town
- Keyboard controls, window size / fullscreen / pixel filter, save files

### What does not work yet

- No sound
- The game runs too slowly in the field (about a third of the intended speed)
- The bottom screen of the title menu is missing its background
- No touch screen or controller support yet

### Which ROM you need

Only this exact version works (the setup checks it):

| | |
|---|---|
| Game | Mario & Luigi: Bowser's Inside Story (Nintendo DS) |
| Region | Europe (`CLJP`), Revision 0 |
| File size | 134,217,728 bytes (untrimmed) |
| SHA-1 | `5F35BFBF79A0947740795DD770D99E8EEA3AC85E` |

US / Japanese versions and trimmed ROMs are not supported.

### How to use

**With RecompLauncher:** add the game; it asks for your ROM, builds the game and starts it.

**By hand (Windows x64):**

1. Unzip `bis-recomp-v0.1.0-Windows-x64.zip`.
2. Copy your ROM to `orig\baserom.nds`.
3. Run `tools\python\python.exe -I recomp\setup_game.py --rom orig\baserom.nds` (2–3 minutes).
4. Start `build\pc\bis_pc.exe extract\eur --save saves\bis.sav`.

Controls: arrow keys = D-Pad, X / C = A / B, S / D = X / Y, Q / W = L / R, Enter = Start,
Backspace = Select, F11 = fullscreen, Esc = quit.

Third-party tools included (dsd, Ninja, w64devkit/GCC, SDL 3, Python) and their licenses:
see `THIRD_PARTY_NOTICES.md` and `licenses/`.

Mario & Luigi: Bowser's Inside Story is a trademark of Nintendo. This project is not affiliated
with or endorsed by Nintendo.
