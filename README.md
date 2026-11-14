# bis-decomp

Eigenes Projekt für einen PC-Port von **Mario & Luigi: Bowser's Inside Story** (Nintendo DS).

Dieses Repository enthält **keine** Original-Daten des Spiels. Du brauchst deinen eigenen Dump.

## Ansatz

**Statische Recompilation** (wie bei den Zelda-64-Recomp-Ports): Ein eigener Recompiler
übersetzt den ARM9-Maschinencode (Hauptprogramm + Overlays) automatisch nach C. Eine
Plattform-Schicht ersetzt die DS-Hardware auf dem PC. Parallel kann Code von Hand
decompiliert werden und den generierten Code nach und nach ersetzen.

## Zielversion

| Feld | Wert |
|---|---|
| Region | Europa (En, Fr, De, Es, It) |
| Gamecode | `CLJP` |
| Revision | 0 |
| Größe | 134 217 728 Bytes (128 MiB) |
| SHA-1 | `5F35BFBF79A0947740795DD770D99E8EEA3AC85E` |

Lege den ROM als `orig/baserom.nds` ab (wird von Git ignoriert).

### Eckdaten aus dem Header

- ARM9: ROM-Offset `0x4000`, Ladeadresse `0x02004000`, Entry `0x02004800`, Größe `0x3593C` (~214 KiB)
- ARM9-Overlays: 141
- Laut dsd ~12 200 Funktionen (Hauptprogramm 1 875, Rest in Overlays)

## Roadmap

### Phase 0: Grundlagen

- [x] Tools: dsd, objdiff, Ninja, Ghidra 12.2 + JDK 25
- [x] ROM mit dsd extrahieren und neu bauen. Code und Daten sind identisch,
      offen sind nur Secure-Area-CRC (`0x6C`) und Header-CRC (`0x15E`) → braucht ARM7-BIOS
- [x] `dsd init`: Funktionsgrenzen, Symbole, Relocations in `config/eur/`
- [x] Input-Funktionen gefunden → [`docs/input.md`](docs/input.md)

### Phase 1: Code verstehen

- [x] Alle Overlays in Ghidra laden, dsd-Symbole übernehmen (`scripts/ghidra/LoadOverlays.java`)
- [ ] Overlays einordnen (Kampf, Overworld, Menüs, …) → `docs/overlays.md`
- [ ] SDK-Funktionen (NitroSDK, MSL) erkennen und benennen
- [ ] Kernsysteme kartieren: Dateisystem, Hauptschleife/Zustände, Grafik, Text

### Phase 2: Recompiler

- [x] Befehls-Statistik: Welche ARM-/Thumb-Befehle, indirekte Sprünge, Sprungtabellen? → [`docs/recomp.md`](docs/recomp.md)
- [x] Recompiler-Prototyp: ARM → C, Funktion für Funktion (Grenzen aus dsd), 99,1 % der Funktionen vollständig
- [x] Erzeugten Code mit einem C-Compiler bauen und testen: alles kompiliert und linkt,
      `Pad_Update` besteht 100 000 Zufallstests (w64devkit/GCC in `tools\`)
- [ ] Rest-TODOs: rohe Offsets, Sprünge in fremde Funktionen, Sprungtabellen ohne `cmp`
- [ ] Overlays: Funktionen pro Overlay, Laden/Entladen zur Laufzeit
- [x] Hauptprogramm kompiliert und läuft bis zur ersten Hardware-Abfrage
      → [`docs/boot.md`](docs/boot.md): ~140 Funktionen inkl. OS/PXI/IRQ-Init und Tastenabfrage
- [x] Thread-Modell: SDK-Threads als Host-Fibers, deterministisch, 7 Einzeltests
      → [`docs/threads.md`](docs/threads.md) (Zweig `thread-model`)
- [ ] ARM7-Ersatz für den Kartenzugriff (PXI-Kanal 11): aktueller Halt beim Start

### Phase 3: Plattform-Schicht (SDL + OpenGL)

- [ ] Speicher und Register (I/O-Zugriffe abfangen)
- [ ] 2D-Grafik: BG-Layer, Sprites, Paletten, VRAM
- [ ] Input: Tastatur, Maus als Touchscreen, Controller (Andockpunkte in `docs/input.md`)
- [ ] ARM7-Ersatz in PC-Code: Touch, Mikrofon, Uhr, Power, Dateisystem
- [ ] 3D-GPU
- [ ] Sound
- [ ] **Erster spielbarer Stand**

### Launcher

Das Spiel startet später über einen Launcher (Teil derselben `.exe`):
- Beim ersten Start die eigene `.nds` auswählen, SHA-1 prüfen (nur `CLJP` Rev 0),
  Grafiken/Sounds/Texte lokal extrahieren. Es werden keine Spieldaten mitgeliefert.
- Danach: Start per Klick, Einstellungen für Tastenbelegung, Fenstergröße, Bildschirm-Layout.

### Phase 4: PC-Extras

- [ ] Fenster-Layout für zwei Bildschirme, freie Tastenbelegung
- [ ] Maus als Stylus in Riesenkämpfen, Taste statt Mikrofon-Pusten
- [ ] Widescreen, höhere Framerate (wo möglich)

### Nebenher: Decomp von Hand

Braucht den Compiler mwccarm (über den DS-Decomp-Discord). Gematchte Funktionen ersetzen
den generierten Code. Beim Arbeiten Input-Funktionen gezielt benennen.

## Befehle

### dsd

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

Overlays laden und Symbole aus dsd übernehmen (mehrfach ausführbar, dauert mit Analyse ~4 Minuten):

```
scripts\ghidra-headless.bat <Projektpfad>\ghidra bis_eur -process arm9.bin -scriptPath scripts\ghidra -preScript LoadOverlays.java <Projektpfad>
```

Das Ghidra-Projekt `ghidra\bis_eur` ist lokal und wird nicht committet.

## Ordner

- `orig/` – dein ROM (ignoriert)
- `extract/` – von dsd extrahierte ROM-Inhalte (ignoriert)
- `config/` – dsd-Konfiguration: Symbole, Relocations, Funktionsgrenzen
- `tools/` – lokale Tools (ignoriert)
- `scripts/` – eigene Skripte (Python, Ghidra)
- `src/`, `include/` – decompilierter Code
- `docs/` – Notizen zu Systemen und Formaten
