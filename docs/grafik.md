# Grafik: VRAM-Bänke und 2D-Renderer

**Erstes Bild am 15.11.2026:** Nach 600 Frames zeigt der Start-Test das Nintendo-Logo mit
Mario und Luigi. Davor wird über die Master-Helligkeit eingeblendet (Frame 100 schwarz,
Frame 300 weiß).

```
recomp\runtime\run_boot.bat 600 4000     # Bilder landen in build\frames\
```

Alle 100 Frames und am Ende entsteht `build/frames/frame_<n>.png` (256×384, beide Bildschirme).
Welche Engine oben liegt, entscheidet `POWCNT1` Bit 15. Die Bilder enthalten Spielgrafik und
werden **nicht** committet (`build/` wird ignoriert).

## Fenster (SDL3) – `recomp/runtime/frontend_sdl.c`

```
recomp\runtime\run_pc.bat          # spielen (ohne Frame-Limit)
recomp\runtime\run_pc.bat 300      # Test: 300 Frames, Fenster schliesst sich selbst
```

SDL 3.4.20 (MinGW) liegt in `tools\SDL3-3.4.20\` und wird nicht committet. Programm:
`build\pc\bis_pc.exe` mit `SDL3.dll` daneben. Etwa 60 Bilder/s (DS: 59,83), die aktuelle
Rate steht im Fenstertitel. Bei einem Absturz bleibt das letzte Bild stehen, der Grund steht im
Titel und der Bericht in der Konsole.

| Taste | DS |
|---|---|
| Pfeiltasten | Steuerkreuz |
| X / C | A (Mario) / B (Luigi) |
| S / D | X / Y |
| Enter / Rücktaste | Start / Select |
| Q / W | L / R |
| F11 / Esc | Vollbild / Beenden |

X/Y liest das Spiel nicht aus einem Register, sondern aus `HW_BUTTON_XY_BUF` (`0x027fffa8`,
Bit 10/11, aktiv-niedrig), den der ARM7 füllt. Früher stand dort 0, das Spiel sah X und Y
also dauerhaft gedrückt. Behoben.

**Eingabe-Automatik für Tests ohne Fenster:** Umgebungsvariable
`BIS_INPUT="1100:start,1400:a"` drückt die Taste ab dem Frame für 6 Frames. Das bleibt
reproduzierbar.

## Stand nach dem Fenster (15.11.2026)

- Logo → **Titelbildschirm** (Peachs Schloss, Logo „Mario & Luigi: Bowser's Inside Story“),
  3000 Frames ohne Absturz, 1108 Funktionen.
- Mit Start + A geht es zur **Dateiauswahl** (Karte des Pilzkönigreichs, L/R oben),
  5000 Frames ohne Absturz. Der Absturz bei Frame ~1496 war kein Speicherproblem, sondern
  zwei Recompiler-Fehler: Aufrufe an Adressen, die sich mehrere Overlays teilen, gingen an
  das falsche Overlay (Pfad-Tabelle aus ov001 statt ov127 → Datei nie geöffnet), und ein
  verschleierter Sprung `eor pc, …` in ov129 fiel in die nächste Funktion durch
  (→ [`recomp.md`](recomp.md)).
- Der untere Bildschirm der Dateiauswahl ist komplett **3D** (BG0 von Engine A; BG1/BG2 und
  Sprites sind dort leer). Seit dem 3D-Renderer sind die Spielstand-Felder und
  „Choose a save file.“ zu sehen.

## Stand Dateiauswahl (14.11.2026)

- Dateiauswahl sichtbar **und bedienbar**: Rechts/Links bewegt die Hand, A wählt ein Feld
  („Start / Cancel“). Dafür waren nötig: 3D-Renderer, 64-Bit-Division mit mehreren
  Einstiegen (Projektionsmatrix), und der Kopierschutz musste durchlaufen
  (→ [`kopierschutz.md`](kopierschutz.md)).
- Nach „Start“ wechselt die Szene (Sound-Verwaltung läuft jetzt dank VBlank-Zähler), danach
  bleibt das Bild **schwarz**: VRAM-Bänke aus, alle Threads warten. **Nächste Hürde.**

Fehlersuche: `boot_test.exe … [funktion_hex] [adresse:laenge]` zeigt Argumente samt Frame
und Vorgeschichte bzw. Schreibzugriffe mit Frame, `lr`, `sp` und Vorgeschichte. Umgebungsvariablen:

| Variable | Wirkung |
|---|---|
| `BIS_INPUT="1100:start,1400:a"` | Tasten ab Frame drücken (6 Frames) |
| `BIS_DUMP="adresse:laenge"` | Speicher am Ende ausgeben |
| `BIS_RWATCH=adresse` | Lesezugriffe auf ein Wort melden |
| `BIS_WATCH_FROM=frame` | Schreib-Watchpoint erst ab diesem Frame |
| `BIS_LAYERS=1f` | nur diese Ebenen zeichnen (Bit 0-3 BG0-3, Bit 4 Sprites) |
| `BIS_3D=1` | 3D: Polygon-Zähler, Matrizen, erste Polygone |
| `BIS_DMALOG=1` / `BIS_DIV=1` | DMA-Übertragungen / auffällige Divisionen protokollieren |

## VRAM-Bänke – `recomp/runtime/vram.c`

9 Bänke (A–I) mit eigenem Speicher. Die Zuordnung folgt `VRAMCNT` (`0x04000240–0x04000249`,
ohne `0x247` = WRAMCNT): LCDC, BG/OBJ von Engine A/B, erweiterte Paletten, Texturen.
CPU-Zugriffe auf `0x06xxxxxx` laufen über die aktuelle Zuordnung. Bereiche ohne Bank werden
ignoriert wie auf der Hardware. Der Renderer liest über die Engine-Sicht
(`vram_bg_read8`, `vram_obj_read8`, `vram_bg_extpal`, `vram_obj_extpal`).

Beobachtete Zuordnung am Logo-Bildschirm: A/B = Texturen, C = Engine-B-BG, D = Engine-B-OBJ,
E aus, F = Textur-Paletten, G = Engine-A-OBJ, H = Engine-B-BG-ExtPal, I = Engine-B-OBJ-ExtPal.

## 2D-Renderer – `recomp/runtime/render2d.c`

Zeichnet den Zustand am Frame-Ende (noch nicht Zeile für Zeile).

| Funktion | Stand |
|---|---|
| Anzeigemodus 1, erzwungenes Leerbild | ✅ |
| Text-Hintergründe: 16/256 Farben, erweiterte Paletten, 256/512, Scroll, Spiegeln | ✅ |
| Normale Sprites: 16/256 Farben, 1D/2D, erweiterte Paletten, Prioritäten | ✅ |
| Master-Helligkeit (Ein-/Ausblenden) | ✅ |
| Affine/erweiterte Hintergründe | FEHLT |
| 3D-Ebene (BG0 von Engine A, horizontal verschiebbar, 3D-Alpha) | ✅ |
| Farbeffekte: Alpha-Blending, Aufhellen/Abdunkeln, halbtransparente Sprites | ✅ |
| Affine Sprites, Bitmap-Sprites, Fenster, Mosaik | FEHLT |
| Anzeigemodi 2/3 (VRAM-/Hauptspeicher-Anzeige) | FEHLT |

Was beim Zeichnen übersprungen wird, steht im HLE-Bericht als `FEHLT Renderer: …`.

`recomp/runtime/png.c`: PNG ohne externe Bibliothek (unkomprimierte Deflate-Blöcke).

## 3D-Renderer – `recomp/runtime/gpu3d.c`

Eigene Software-Umsetzung nach GBATEK. Befehle kommen über die gepackte GX-FIFO
(`0x04000400`, auch per DMA mit Start „GX-FIFO“) oder die direkten Ports (`0x04000440–5ff`).
Die bis `SWAP_BUFFERS` gesammelte Liste wird beim nächsten VBlank übernommen und im Frame
danach gezeichnet (eine Frame Verzögerung wie auf der Hardware).

| Teil | Stand |
|---|---|
| Matrix-Befehle, Stapel (Projektion/Position/Richtung/Textur), Ergebnisse lesen | ✅ |
| Vertex-Formate, Dreiecke/Vierecke/Streifen, Clipping an 6 Ebenen, Culling | ✅ |
| Licht (4 Lichter, Shininess), Texturkoordinaten-Transformation | ✅ (Normale/Vertex-Quelle ungeprüft) |
| Texturen: alle 7 Formate inkl. 4x4-komprimiert, Wiederholen/Spiegeln | ✅ |
| Modulate/Decal/Toon/Highlight, Alpha-Test, Alpha-Blending, Z-/W-Buffer, Drahtgitter | ✅ |
| Schatten-Polygone, Nebel, Kantenmarkierung, Anti-Aliasing, Rear-Plane-Bitmap | FEHLT |
| Box-Test | SIMULIERT (immer „sichtbar“) |
| GXSTAT/FIFO-Zeitverhalten | SIMULIERT (FIFO immer leer, nie beschäftigt) |

Nicht aus fremden Emulatoren übernommen; Rundung und Interpolation sind eigene Näherungen.
