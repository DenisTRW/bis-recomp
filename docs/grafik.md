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
- Mit Start + A geht es weiter (1194 Funktionen), dann **Absturz bei Frame ~1496**:
  Eine Dateistruktur bei `0x020799cc` wird beim Laden eines Overlays (Rückwärts-
  Dekompression `0x02004970`) überschrieben, danach Lesen mit Archiv-Zeiger 0. Liegt
  direkt vor dem verschobenen Heap-Anfang `0x020799e0`. **Nächste Hürde.**
- Unterer Bildschirm bleibt weiß (3D-Ebene fehlt).

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
| 3D-Ebene (BG0 von Engine A) | FEHLT – braucht 3D-Renderer |
| Affine Sprites, Bitmap-Sprites, Fenster, Alpha-Blending, Mosaik | FEHLT |
| Anzeigemodi 2/3 (VRAM-/Hauptspeicher-Anzeige) | FEHLT |

Was beim Zeichnen übersprungen wird, steht im HLE-Bericht als `FEHLT Renderer: …`.

`recomp/runtime/png.c`: PNG ohne externe Bibliothek (unkomprimierte Deflate-Blöcke).
