# tools

Local tools used to build the game. Nothing in here is part of the repository (only this file and
`.gitkeep`); release packages contain the tools listed in `THIRD_PARTY_NOTICES.md`.

| Tool | Path | Used for |
|---|---|---|
| dsd (ds-decomp) | `tools/dsd.exe` | extracting and disassembling your ROM |
| Ninja | `tools/ninja.exe` | running the compile steps |
| w64devkit (GCC) | `tools/w64devkit/` | C compiler for Windows |
| SDL 3 (MinGW build) | `tools/SDL3-<version>/` | window, graphics output, input |
| Python (embeddable) | `tools/python/` | running the recompiler (only needed in release packages) |

See the main README, section "Building the game".