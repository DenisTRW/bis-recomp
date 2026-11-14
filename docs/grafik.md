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
