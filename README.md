# bis-decomp

Eigenes Decompilations-Projekt für **Mario & Luigi: Bowser's Inside Story** (Nintendo DS), mit dem langfristigen Ziel eines PC-Ports.

Dieses Repository enthält **keine** Original-Daten des Spiels. Du brauchst deinen eigenen Dump.

## Zielversion

| Feld | Wert |
|---|---|
| Region | Europa (En, Fr, De, Es, It) |
| Gamecode | `CLJP` |
| Revision | 0 |
| Größe | 134 217 728 Bytes (128 MiB) |
| SHA-1 | `5F35BFBF79A0947740795DD770D99E8EEA3AC85E` |

Lege den ROM als `orig/baserom.nds` ab (wird von Git ignoriert).

## Eckdaten aus dem Header

- ARM9: ROM-Offset `0x4000`, Ladeadresse `0x02004000`, Entry `0x02004800`, Größe `0x3593C` (~214 KiB)
- ARM9-Overlays: 141 (Overlay-Tabelle 4512 Bytes / 32)

## Meilensteine

1. [ ] Toolchain einrichten (dsd, objdiff, ninja, Ghidra 12.2 + JDK 25 ✅ – mwccarm fehlt noch)
2. [ ] ROM mit dsd extrahieren und byte-identisch neu bauen
   - Extraktion und Rebuild funktionieren, alle Code- und Datenbereiche sind identisch.
   - Offen: Secure-Area-CRC (`0x6C`) und Header-CRC (`0x15E`). Dafür braucht dsd das ARM7-BIOS
     (`dsd rom build --arm7-bios <datei>`), selbst gedumpt von der eigenen Konsole.
   - `dsd init` hat `config/eur/` erzeugt: 141 Overlays, ~12 200 erkannte Funktionen.

### Befehle

```
tools\dsd.exe rom extract --rom orig\baserom.nds --output-path extract\eur
tools\dsd.exe rom build --config extract\eur\config.yaml --rom build\rebuilt.nds
tools\dsd.exe init --rom-config extract\eur\config.yaml --output-path config\eur --build-path build\eur
```

### Ghidra

Ghidra und das JDK liegen in `tools\`. In `tools\ghidra_*\support\launch.properties` muss stehen:
`VMARGS=-Dapplication.settingsdir=<Projektpfad>\ghidra\settings`. Dann speichert Ghidra seine Einstellungen im Projekt.

```
scripts\ghidra.bat
scripts\ghidra-headless.bat <Projektpfad>\ghidra bis_eur -import extract\eur\arm9\arm9.bin -loader BinaryLoader -loader-baseAddr 0x02004000 -processor ARM:LE:32:v5t
```

Das Ghidra-Projekt `ghidra\bis_eur` ist lokal und wird nicht committet.
3. [ ] Erste kleine Funktionen matchen (Hilfsfunktionen: Mathe, Speicher, Strings)
4. [ ] Dateisystem und Dekompression
5. [ ] Text-/Script-System
6. [ ] … Kampf, Overworld
7. [ ] NDS-Abstraktionsschicht (SDL/OpenGL) für den PC-Port
8. [ ] PC-Steuerung: frei belegbare Tastatur, Maus als Touchscreen (inkl. Riesenkämpfe),
       Taste statt Mikrofon-Pusten, Controller über SDL

Beim Decompilieren Input-Funktionen (Pad, Touch, Mikrofon) gezielt benennen und in
`docs/input.md` festhalten – sie sind später die Andockpunkte für die PC-Steuerung.

## Ordner

- `orig/` – dein ROM (ignoriert)
- `tools/` – lokale Tools (ignoriert)
- `src/`, `include/` – decompilierter Code
- `docs/` – eigene Notizen zu Systemen und Formaten
