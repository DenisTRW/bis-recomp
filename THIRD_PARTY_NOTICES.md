# Third-party software in release packages

Release packages of bis-recomp contain only tools – no game data and no game code. The game is
built on your PC from your own ROM during setup (`recomp/setup_game.py`). These third-party
programs are included unchanged so that this build works without installing anything else:

| Program | Used for | License | Source |
|---|---|---|---|
| dsd (ds-decomp) | extracting and disassembling your ROM | MIT | https://github.com/AetiasHax/ds-decomp |
| Ninja | running the compile steps | Apache License 2.0 | https://github.com/ninja-build/ninja |
| w64devkit | C compiler (GCC) and tools for Windows | Unlicense; contains GCC (GPL v3 with the GCC Runtime Library Exception), binutils (GPL v3) and the Mingw-w64 runtime (see `COPYING.MinGW-w64-runtime.txt`) | https://github.com/skeeto/w64devkit |
| SDL 3 | window, graphics output, input | zlib License | https://github.com/libsdl-org/SDL |
| Python (embeddable) | running the recompiler | Python Software Foundation License | https://www.python.org |

The complete source code of the GCC and binutils versions contained in w64devkit is available
from the w64devkit release that the package was made from (see the link above); the version is
named in `tools/w64devkit/README.md` inside the package.

The license texts are included in the package next to each program.
