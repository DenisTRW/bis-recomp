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
- Titelmenü („START GAME / MIC TEST“) verschwand nach dem Einblenden: Liegt die 3D-Ebene
  oben und darunter ein zweites Blend-Ziel, mischt die Hardware **immer mit dem 3D-Alpha**,
  auch bei deckenden Pixeln. Wir nahmen dann BLDALPHA (16/16), dadurch wurde alles weiß
  gesättigt. Behoben.
- Nach „Start“ wechselt die Szene (Sound-Verwaltung läuft jetzt dank VBlank-Zähler), danach
  bleibt das Bild **schwarz**. **Nächste Hürde**, Diagnose:
  - Der Haupt-Thread wartet in `FS_WaitAsync` (`0x0203dc48`) auf eine Datei, deren
    `FSFile` (`0x021e3658`) überschrieben wurde.
  - Überschrieben hat sie der Streaming-Lader `0x0201c9ac`: Ringpuffer 4 × 0x200 Bytes im
    Objekt `0x021e29cc + 0xf4`, Datensätze mit 2-Byte-Länge. Läuft ein Datensatz über das
    Pufferende, wird der Rest dahinter kopiert (max. ~0x30c Bytes Platz). Hier kamen
    Längen von 0x69a/0x6aa, also falsch ausgerichtete Daten.
  - Ursache vermutlich Zeitverhalten: Beim Wechsel zur nächsten Anforderung parst der Lader
    noch Reste der alten, während der neue Block geladen wird. Auf der Hardware ist der
    Karten-DMA dafür langsam genug, bei uns kommt der Block sofort und überschreibt die Reste
    (sichtbar: Lesezeiger springt um 3 Bytes in den neuen Block).
  - **Neuer Befund (Karten-DMA):** Das Standardverhalten (DMA sofort fertig) lädt bei
    DMA-Anforderungen **den falschen Block**. Ablauf im SDK: `0x02042780` Anforderung,
    `0x02042410` setzt den Karten-Interrupt (`OS_ResetRequestIrqMask` + `OS_EnableIrqMask`),
    `0x020422ec` schaltet den DMA scharf (wartet auf Bit 31), schreibt B7 und startet ROMCTRL,
    der Fertig-Handler `0x02042340` stoppt den DMA, zählt `+0x200` weiter und startet ggf. den
    nächsten Block. Bei uns lag noch eine alte „Karte fertig“-Meldung in der internen
    Warteliste. `OS_ResetRequestIrqMask` löscht sie nicht, beim Freigeben feuert sie sofort,
    der Handler zählt weiter, und es wird `adresse + 0x200` gelesen (z. B. `0393a800` →
    `0393aa00`).
  - Mit `BIS_CARD_DELAY=n` (Übertragung nach n Speicherzugriffen) löscht die IF-Quittung
    auch die Warteliste. Dann stimmen alle Blöcke. Allerdings überschreibt der Streaming-Lader
    schon in der Startphase ein Objekt von ov122 (`0x0208bfe8`, Datensatz-Handler
    `0x01ff80a4`), und beim Wechsel zum Logo stürzt das Spiel ab (Frame 317 bei n = 2048,
    später bei größerem n). Ursache noch offen: Beim Parsen steht im Ringpuffer nicht der
    erwartete Block (Datensatzlänge 1 statt `0x0a74` laut ROM bei `0x0393a854`).
  - **Behoben (DMA-Adresszähler):** Das SDK stoppt den Karten-DMA (`0x0203a3bc`), indem es
    den noch aktiven Kanal kurz auf „sofort“ stellt (CNT `85…`, dann `05…`). Dabei wird ein
    Dummy-Wort übertragen. Auf der Hardware geht es an die **interne, schon weitergezählte**
    Zieladresse; Quelle/Ziel werden nur beim Einschalten (Bit 31: 0 → 1) übernommen. Wir
    nahmen die Register-Adresse und überschrieben so das erste Wort jedes frisch geladenen
    Blocks mit `ffffffff` → falsche Datensatzlängen → zerstörtes Objekt. Jetzt gibt es
    interne Zähler je Kanal (`dma_isrc/dma_idst`). Standardmodus unverändert (gleiche
    Prüfsummen wie vorher).
  - Mit `BIS_CARD_DELAY=2048` läuft der Start sauber, die Dateiauswahl funktioniert, und nach
    „Start“ (`BIS_INPUT="1100:start,1400:a,1900:a,2200:a"`) wird das `FSFile` nicht mehr
    zerstört. Das Bild bleibt trotzdem schwarz: In Frame 2231 setzt die neue Szene
    (`0x021117e8`) alle VRAM-Bänke auf 0 und ordnet sie bis Frame 2700 nicht neu zu; die
    meiste Zeit läuft `func_ov125_02077a70`. Der Standard ist noch „sofort“
    (`BIS_CARD_DELAY=0`).
  - Das Schwarz ist gewollt: Master-Helligkeit `0x8010` (voll abgedunkelt) bis zum Ende des
    Ladens. Drei Ursachen für das Hängen davor sind behoben:
    1. **Karte im `OS_Halt`:** Schläft der Prozessor, zählte der Countdown nicht weiter, jeder
       Block kostete bis zu 19 Frames. Jetzt wird eine laufende Übertragung im Halt fertig.
    2. **DMA-Ende-Interrupt (CNT Bit 30 → IF Bit 8+Kanal) fehlte.** Die Szene sendet 3D-Daten
       per DMA in die GX-FIFO (`0x0203a79c`, Rückruf über `0x0203a854`) und wartet in
       `func_ov006_02123680` auf ein Flag, das nur der Rückruf löscht.
    3. **GX-FIFO-Interrupt (IF Bit 21)** nach GXSTAT Bits 30-31 ergänzt (pegelgesteuert,
       FIFO immer leer, SIMULIERT), genutzt von `0x0203a598`.
  - Danach zeichnet die Szene ~900 Polygone und arbeitet zeitverteilt über `VCOUNT`
    (`func_ov003_0206b788`), bleibt aber bis Frame 6000 abgedunkelt. **Nächster Schritt:**
    herausfinden, was das Einblenden auslöst (Kandidaten: Sound-Antworten des ARM7 über PXI,
    `VCOUNT`-Nachbildung „pro Lesezugriff“).
  - Weitere Eingrenzung (Stand danach):
    - Die alte Szene blendet bis Frame 2231 auf `0x8010` ab (VBlank-Handler, `lr=0x0206abc4`);
      danach schreibt **niemand** mehr MASTER_BRIGHT – die neue Szene startet ihr Einblenden nie.
    - Der Streaming-Lader ist fertig: Warteschlange leer (`0x021e29cc+0xac = 0`), keine
      `FSFile` beschäftigt, ab Frame 2267 keine Kartenzugriffe mehr. Kein Hänger dort.
    - Hauptschleife der Szene: `func_ov004_020cead8`. Sie wartet bei `.L_020cec58` auf Bit 0
      von `[0x0212a0c0+0x16]`; das Flag wechselt alle 4 Frames 5 ↔ 6, die Szene läuft also
      weiter (kein Deadlock), nur im 4-Frame-Takt.
    - Sound: `SNDSharedWork` liegt bei `0x02060a00`. `playerStatus` (+4) ist immer 0, damit
      gilt jede gestartete Musik sofort als beendet (`func_0202f44c`). Globale
      Sequenz-Variablen (`func_0202f370`) werden nach „Start“ nicht gelesen – Warten auf Musik
      ist damit unwahrscheinlich, aber nicht ausgeschlossen (Ende-Status statt Variable).
    - Offen: was die Szene zum Einblenden braucht. Nächste Kandidaten: Zustandsmaschine der
      Szene (`[data_?+0x68]` Bits 0/1 in `func_ov004_020cead8`), der 4-Frame-Takt, und ob
      ein Skript auf ein Sound-Ende wartet, das nie „läuft“.
  - **Die Szene ist da:** Mit `BIS_NOBRIGHT=1` (Master-Helligkeit beim Zeichnen ignorieren)
    zeigt Frame 2700 die erste Spielszene (Toad Town, Pilzhaus, zwei Toads, HUD) auf beiden
    Bildschirmen. Laden, 2D und 3D funktionieren also; es fehlt nur das Einblenden, und die
    Szene steht still (Frame 2700 = 3300).
  - Sound als Ursache geprüft: Nach „Start“ schickt das Spiel PREPARE_SEQ/START_PREPARED_SEQ
    (Befehle 2/3, sichtbar mit `BIS_SNDLOG=1`). Mit `BIS_SND_STATUS=1` meldet der ARM7-Ersatz
    gestartete Spieler in `playerStatus` als laufend. Das Bild bleibt trotzdem dunkel, der
    Spielerstatus allein ist es also nicht. **Nächster Schritt:** herausfinden, worauf das
    Ereignis-Skript der Szene wartet (Kandidaten: Sequenz-Ende/Tick-Zähler des Sounds,
    Zeitgeber).
  - **Fade-Kette gefunden (Stand, noch nicht behoben):** Bildschirm-Objekte der Szene
    (`0x0233ab24` oben, `0x02339514` unten) haben eine Zustandsfunktion in `+4/+8` (Tabelle
    `data_ov004_0210cc48`) und Fade-Felder: `+0xc4` Wert (20.12, steht auf −16), `+0xc8`
    Schritt, `+0xcc` Restdauer, `+0xce` Bit 0 „aktiv“. Fade-Schritt: `func_ov003_0206ae54`,
    Fade-Start: `func_ov003_0206ad4c(obj, von, bis, dauer)` – wird nach „Start“ **nie**
    aufgerufen. Das Objekt hängt im Zustand `func_ov004_02091bbc` (wartet auf Wert > −16).
    Dorthin kommt es über `func_ov004_0208c700` → `0208d0cc`. In `0208c700` entscheidet ein
    Sprungverteiler über Bits 8-10 von `[[data_ov004_0210fda8]+0x68]` (Fälle 0-4), ob der
    Fade gestartet wird; Fall 2/4 nur, wenn Bit 19 gesetzt ist, Fall 1 nur, wenn ein Bit in
    `data_020564e0+8` nicht gesetzt ist, Fall 3 nie. **Nächster Schritt:** diesen Wert und die
    Bits beim Aufruf von `0208c700` ausgeben und klären, wer sie setzt.
  - Neue Hilfen: `BIS_PROFILE_FROM=frame` (Zähler „Häufigste“ ab diesem Frame), der Bericht
    zeigt die zuletzt außerhalb von Interrupts betretene Funktion und deren letzte Lesezugriffe
    (findet Warteschleifen ohne Funktionsaufrufe).

Fehlersuche: `boot_test.exe … [funktion_hex] [adresse:laenge]` zeigt Argumente samt Frame
und Vorgeschichte bzw. Schreibzugriffe mit Frame, `lr`, `sp` und Vorgeschichte. Umgebungsvariablen:

| Variable | Wirkung |
|---|---|
| `BIS_INPUT="1100:start,1400:a"` | Tasten ab Frame drücken (6 Frames) |
| `BIS_DUMP="adresse:laenge"` | Speicher am Ende ausgeben |
| `BIS_RWATCH=adresse` | Lesezugriffe auf ein Wort melden (ab `BIS_WATCH_FROM`) |
| `BIS_WATCH_FROM=frame` | Schreib-Watchpoint erst ab diesem Frame |
| `BIS_LAYERS=1f` | nur diese Ebenen zeichnen (Bit 0-3 BG0-3, Bit 4 Sprites) |
| `BIS_3D=1` | 3D: Polygon-Zähler, Matrizen, erste Polygone |
| `BIS_DMALOG=1` / `BIS_DIV=1` | DMA-Übertragungen / auffällige Divisionen protokollieren |
| `BIS_DUMP_EVERY=n` | Bild alle n Frames (Standard 100) |
| `BIS_3DPNG=1` | zusätzlich das reine 3D-Bild als `frame_<n>_3d.png` |
| `BIS_CTXMEM="ac*:40,c00:c"` | bei jedem verfolgten Aufruf Worte ab r0+off ausgeben (`*` = Zeiger folgen) |
| `BIS_NOBRIGHT=1` | Master-Helligkeit beim Zeichnen ignorieren (zeigt abgedunkelte Szenen) |
| `BIS_SNDLOG=1` / `BIS_SND_STATUS=1` | Sound-Befehle protokollieren / Experiment: Spielerstatus pflegen |
| `BIS_CARDLOG=1` | Karten-Befehle, DMA-Steuerung, Karten-DMA |
| `BIS_CARD_DELAY=n` | experimentell: Karten-DMA erst nach n Speicherzugriffen |

Funktionsverfolgung mit mehreren Adressen: `boot_test.exe … 0 "0201c9ac,0203dd70"`.

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
