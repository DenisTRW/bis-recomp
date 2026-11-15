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
| W A S D (oder Pfeiltasten) | Steuerkreuz |
| K / J | A (Mario) / B (Luigi) |
| I / U | X / Y |
| Enter / Rücktaste | Start / Select |
| Q / E | L / R |
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
  - **Spur zum fehlenden Einblenden:** `0208c700` läuft für den oberen Bildschirm in Frame
    2253, für den unteren erst in Frame 2267. Das Statuswort `[0x0212778c+0x68]` ist dabei 0,
    also Fall 0: Ein Bildschirm startet den Fade des **anderen** nur, wenn dieser schon im
    Zustand ≥ 3 ist (`vtable+0x54`), sonst wird nur ein „ausstehend“-Bit gesetzt
    (`.L_0208cf9c`). Oben findet unten noch nicht bereit vor, unten findet oben im Zustand 2
    – beide setzen nur das Bit, danach stellt `0208d0cc` beide auf Zustand 3
    (`func_ov004_02091bbc`, wartet auf Fade) → Stillstand. Vermutung: Reihenfolge/Abstand
    der beiden Einrichtungen hängt an unserem Lade-Timing (Karte). **Widerlegt:** Mit
    `BIS_CARD_DELAY=512` bzw. `8192` laufen beide im selben Frame (2238 bzw. 2235), das Bild
    bleibt trotzdem dunkel – die Reihenfolge oben → unten führt immer zur Blockade.
    **Nächster Schritt:** (a) wer wertet das „ausstehend“-Bit (`.L_0208d0c0`-Bitfeld) später
    aus und warum passiert das bei uns nicht; (b) ob das Statuswort (Bits 8-10) beim echten
    Spiel vor `0208c700` ungleich 0 ist, also wer es normalerweise setzt (Kandidat:
    Ereignis-/Skriptsystem, z. B. `func_ov006_0211d3e8`, `func_ov007_0210fdc0`).
  - `data_02056040` ist das Ereignis-Flag-Feld. `0208c700` setzt dort in Frame 2253 **kein**
    Flag (Vermutung „Ereignis wird übersprungen“ damit widerlegt). Ab Frame 2267 setzt
    `func_02005a78` (aufgerufen aus `0x02016254`/`0x02015f0c`) laufend Flags
    (`0x020561d0`, `0x02056228`) – das Skriptsystem läuft also. Wahrscheinlich ist die
    Eröffnung ein Skript, das vor dem Einblenden auf etwas wartet. **Nächster Schritt:** das
    Skript-/Ereignissystem ab `0x02016254` verfolgen und den Befehl finden, bei dem es steht.
  - Skriptsystem läuft weiter, nur selten: Flag-Zugriffe (`0x02016254`/`0x02015f0c`, aus
    `func_ov006_02113b70` → `0x020161d4`) in Frame 2252, 2267, 2269, 2277, 2433, 2437, 2517,
    2596, 2636, 2662. `func_ov006_02111874` ist nur die HUD-Zahlenanzeige.
  - Zustandstabelle `data_ov004_0210cc48` (je 8 Byte): +0x20 = `func_ov004_020925a0` wartet
    auf leeren Datenstrom (`[[0x0210fda8]+0x30]+0xac == 0`) und **startet dann das
    Einblenden** (−16 → Zielwert, Dauer 16) – dieser Zustand wird bei uns nie erreicht.
    Weitere Kandidaten mit derselben Strom-Bedingung: `func_ov004_0208e630`,
    `func_ov004_020922dc`. **Nächster Schritt:** dynamisch prüfen, welcher Code die
    Zustandsfunktion (`[obj+4]`) der Bildschirm-Objekte auf +0x20 setzen würde und warum
    der Weg über `0208c700` → `0208d0cc` (direkt Zustand 3) gewählt wird.
  - Zustandsablauf des oberen Bildschirms (Byte `+0xd0`): 1 (Frame 2235) → 2 (2253, über
    `0208d0cc`) → 3 (2267). Das ist der reguläre Weg; Zustand 3 wartet auf ein Einblenden,
    das `0208c700` je nach **Eintrittsart** (Bits 8-10 von `[0x0212778c+0x68]`) startet.
    Die Eintrittsart ist 0 (Struktur frisch genullt in Frame 2231, danach nie gesetzt), und
    `data_020564e0` (Fall 1) wird nach „Start“ gar nicht beschrieben. Vermutung: Beim echten
    Spiel kommt die Eintrittsart aus den Kartenwechsel-Parametern des Neues-Spiel-Ablaufs;
    bei uns fehlen sie. Offene Frage, die sich am schnellsten mit einem Vergleich auf echter
    Hardware/Emulator klären lässt: Welchen Wert haben `[0x0212778c+0x68]` und
    `data_020564e0` beim Betreten von Toad Town?
  - **GELÖST – Ursache des Schwarzbilds:** Vergleich mit DeSmuME als Messgerät
    (`recomp/tools/desmume_probe.lua`): Im echten Spiel startet Skriptbefehl `0x14b`
    (Tabellen-Eintrag 277 → `func_ov006_0211c43c`) den Fade (−16 → 0 über 60 Frames). Der
    Skript-Dispatcher `func_ov006_021142bc` springt über eine Sprungtabelle in den Bereich
    `0x02114964`–`0x02116334`; dsd hatte die Funktion aber nur mit `0x6a8` Byte angesetzt.
    Der Bereich wurde nicht als Code disassembliert, alle Sprünge wurden `RC_TODO` – fast
    alle Skriptbefehle liefen ins Leere. Fix: Größe in `config/eur/arm9/overlays/ov006/
    symbols.txt` auf `0x2078` gesetzt, `dsd dis` neu. Nicht übersetzte Befehle: 959 → 545.
  - Zusätzlich ist das genaue Karten-Timing jetzt **Standard** (`BIS_CARD_DELAY=2048`), weil
    der Sofort-Modus beim Karten-DMA den falschen Block liest. Ergebnis: „Neues Spiel“ zeigt
    die Eröffnung in Toad Town (Kamerafahrt, Toads, Brunnen). Neue Referenz-Prüfsummen
    (3000 Frames): ohne Eingabe `5e5287bf5783d551`, Dateiauswahl (`1100:start,1400:a`)
    `be2fdeed7eab0a31`, Neues Spiel (`…,1900:a,2200:a`, 3600 Frames) `d204bb10158efbe8`.
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
| `BIS_CARD_DELAY=n` | Karten-DMA nach n Speicherzugriffen (Standard 2048, 0 = sofort/alt) |
| `BIS_NOSPIN=1` | Warteschleifen nicht abkürzen (Vergleich, siehe „60 Bilder/s“) |
| `BIS_HOSTPROF=datei` | Rechenzeit-Profil auf dem PC (ab `BIS_PROFILE_FROM`), Auswertung `recomp/tools/hostprof.py` |
| `BIS_LAYERPNG=1` | zu jedem Bild jede Ebene einzeln (`frame_<n>_L0..L4.png`: BG0-3, Sprites) plus DISPCNT/BGCNT/BLDCNT |
| `BIS_MISSLOG=1` | pro ausgegebenem Bild melden, welche Renderer-Funktionen fehlten |
| `BIS_DUMP_FROM=n` | Bilder erst ab Frame n ausgeben (mit `BIS_DUMP_EVERY=1` für Einzelbild-Analysen) |
| `BIS_3DRECT=x0,y0,x1,y1` | mit `BIS_3D=1`: Polygone in diesem Bildbereich mit Texturkoordinaten |
| `BIS_RENDERLOG=x0,y0,x1,y1` | beim Zeichnen: Polygone im Bereich mit Prüfsumme ihrer Texturbytes |
| `BIS_TEXRAW=datei` | mit `BIS_3D=1`: Textur-Slots und Paletten am Ende roh speichern |
| `BIS_SWAPLOG=1` | jedes SWAP_BUFFERS mit VBlank und Bildzeile |
| `BIS_NOGQ=1` | GX-FIFO nach SWAP_BUFFERS nicht anhalten (alter Weg, nur Vergleich) |
| `BIS_RWATCH=adresse:länge` | Lesezugriffe auf einen Bereich melden |

Funktionsverfolgung mit mehreren Adressen: `boot_test.exe … 0 "0201c9ac,0203dd70"`.

## Gelöst: Hintergrund des Titelmenüs (unterer Bildschirm)

Im Original zeigt der untere Bildschirm im Titelmenü einen lila Himmel mit Wolken und das
gespiegelte Schloss im Wasser, bei uns war er weiß. Ursache war **nicht** die Matrix, sondern
die Rückseiten-Erkennung im 3D-Renderer: Sie hielt Dreiecke, die auf dem Bildschirm im
Uhrzeigersinn laufen, für die Vorderseite. Auf dem DS ist es wie bei OpenGL umgekehrt
(gegen den Uhrzeigersinn = vorne, melonDS rechnet genauso). Die Wolken- und
Spiegel-Streifen sind als „nur Vorderseite zeichnen“ markiert (POLYGON_ATTR Bit 7) und
wurden deshalb komplett verworfen; übrig blieben nur entartete Dreiecke ohne Fläche.

So gefunden: `BIS_3D=1` zählte 234 020 von 256 977 Polygonen als „Rückseite“. Seit der
Korrektur sind es bis zum Titelmenü 0 und bis Toad Town 332. Die Projektion (Ortho aus
VBlank 591, x 0…16, y 0…12) und die 64-Bit-Division darin sind korrekt; die früheren
Werte oben waren falsch abgelesen. Prüfsumme des Spielablaufs unverändert
(`759a8d3974b70234`), es ändert sich nur das Bild.

Weitere abgeschnittene Funktion behoben: `func_ov012_02074a54` (Sprungtabelle bis
`0x02076f74`, Größe 0x7a0 → 0x2520). Nicht übersetzte Befehle insgesamt: 959 → 69.
Prüfsummen der drei Referenzläufe unverändert (der Code läuft dort noch nicht).
Neue Hilfen: `BIS_GXLOG=n` (alle GX-Befehle eines VBlanks), `BIS_GXMTX=a-b`,
`BIS_3DTEX=n` (Textur-Slots und Polygone eines VBlanks).
## Geschwindigkeit („Ruckeln“) – Stand

Gemessen mit `boot_test` (ohne Fenster), „Neues Spiel“, Toad Town (Frame 2300–3600):

| Änderung | Hardware-Frames/s |
|---|---|
| Ausgangslage | 21 |
| Aufrufstatistik in der spielbaren Version aus (`BIS_PROFILE` schaltet sie ein), `getenv` aus heißen Pfaden entfernt | 27 |
| Spielcode mit `-O2` statt `-O1` | 28 (kaum Gewinn, nicht übernommen) |
| 3D-Zeichnen übersprungen (`BIS_NO3DRENDER=1`, nur Messung) | 27 (3D ist nicht die Bremse) |
| Thread-Wechsel auch in Warteschleifen nach Interrupts | 27 (kein Gewinn, zurückgenommen) |

Zwei getrennte Ursachen bleiben:
1. **Durchsatz:** Ein Hardware-Frame entspricht bei uns 2^22 Speicherzugriffen (Takt-Modell);
   jeder Zugriff läuft über `rd*/wr*` mit Zähler, Interrupt-Prüfung und Seitensuche. Ergebnis
   ~27 statt 60 Frames/s. Ansatz: schneller Pfad für RAM-Zugriffe (inline, ohne Prüfungen),
   Interrupt-Prüfung seltener.
2. **Spiellogik nur jeden 3. Frame:** Die Hauptschleife der Szene (`func_ov004_020cead8`)
   läuft ~0,33-mal pro Frame. Sie wartet bei `.L_020cec58` auf ein Flag (`0x0212a0d6`), das
   erst beim übernächsten VBlank gelöscht wird. Vermutlich hängt das an der um einen Frame
   verzögerten 3D-Übernahme oder am Takt-Modell. Ansatz: Abgleich mit DeSmuME, wie oft die
   Schleife dort pro Frame läuft.
### Bildzeilen-Zeitmodell (behebt den 3-Frame-Takt)

Stichproben-Profil (`BIS_PROFILE_FROM=3000`, Bericht „Zugriffs-Profil“): In Toad Town gingen
84,6 % aller Speicherzugriffe in eine Warteschleife auf `0x0212a0d6` (Bit 0 von
`[obj+0x16]`, gesetzt in `func_ov004_020cef7c`/`020ceec4`, gelöscht vom **VCount-Interrupt**)
und 14,7 % auf `0x0212a09e`. Das Spiel synchronisiert sich mehrmals pro Bild auf bestimmte
Bildzeilen; unser Modell löste den VCount-Interrupt aber nur einmal pro Frame aus.

Neu: Ein Frame (2^22 Zugriffe) hat 263 Zeilen (`LINE_LEN`), Zeile 192 = VBlank. `VCOUNT` und
die DISPSTAT-Flags folgen der Zeit, der VCount-Interrupt kommt in der eingestellten Zeile
(`line_event`), im Halt springt die Zeit bis zur VCount-Zeile (`halt_to_vcount`).

Ergebnis Toad Town: Hauptschleife 0,33 → **2 Durchläufe pro Frame**, emulierte Frames/s
27 → 35; die Eröffnung läuft sichtbar weiter (Frame 3600 zeigt schon die nächste Szene).
Offen: ob 2 pro Frame dem Original entspricht – Referenz mit DeSmuME (`RATE`-Zeilen in
`recomp/tools/desmume_probe.lua`).

Neue Referenz-Prüfsummen: ohne Eingabe (3000) `ac1b83dc7b22181a`, Dateiauswahl
(`1100:start,1400:a`, 3000) `698b1d268279c0e7`, Neues Spiel (3600) `759a8d3974b70234`
(der früher notierte Wert `a7e0ce41…` war veraltet; der Lauf stoppt bei Frame 3151 am
Standard-Budget von 2000 Mio. Zugriffen). Mit Budget 100000: Neues Spiel bis 5000
`f9d6029a2ec7d436`.

### 60 Bilder/s: Warteschleifen und Zeit-Profil

Neues Werkzeug: `BIS_HOSTPROF=datei` (`recomp/runtime/hostprof.c`) misst echte Rechenzeit auf
dem PC (Stichproben des Befehlszeigers, ab `BIS_PROFILE_FROM`), Auswertung mit
`python -I recomp/tools/hostprof.py programm.exe datei`. Ergebnis im Feld: 53 % der Zeit in
der Warteschleife `.L_020cec58` (siehe oben), danach Buchführung `hle_hit` (Textvergleich bei
jedem Aufruf) und der 3D-Renderer.

1. **Warteschleifen überspringen (bitgleich).** Der Recompiler erkennt reine Warteschleifen
   (`spin_loop` in `recomp.py`: nur Lesen und Rechnen, keine Schreibzugriffe/Aufrufe, jede
   Runde hängt nur von frisch gelesenen Werten ab; derzeit 51 Stück) und ruft am Rücksprung
   `rc_spin` auf. Las die letzte Runde nur RAM und kam kein Ereignis dazwischen, überspringt
   die Laufzeit ganze Runden bis kurz vor das nächste Ereignis (Bildzeile, Takt, Karte,
   Stichprobe, Budget). Das Ereignis kommt dann beim selben Zugriff wie ohne Abkürzung:
   alle Prüfsummen unverändert, `BIS_NOSPIN=1` schaltet es zum Vergleich ab. Im Feld werden
   so ~90 % der Zugriffe übersprungen.
2. **`hle_hit` mit Zeiger-Cache** statt Textvergleich über alle Einträge.

Ergebnis (5000 Frames „Neues Spiel“, ohne Fenster): 83,5 s → 44,8 s → 21,3 s; das Feld läuft
ohne Fenster mit ~175 Bildern/s, im Fenster stabil mit 60 (Taktbremse). Größter
verbliebener Posten: der 3D-Software-Renderer (`gpu3d_vblank`, `plot`).
### Offen nach Spieltest (15.11.2026)

Laufen und Sprechen in Toad Town funktionieren und fühlen sich mit dem Bildzeilen-Modell
flüssig an (damals ~30 Bilder/s im Fenster, inzwischen 60). Zwei sichtbare Fehler:

1. ~~**Keine Sprechblasen / kein Text.**~~ Gelöst, siehe nächster Abschnitt. Die Vermutung
   (affine Sprites/BGs) war falsch.
2. ~~**Figuren-Animationen verzerrt/glitchig.**~~ Gelöst, siehe „Flackernde sprechende
   Figuren“ unten. `BIS_INPUT` nimmt jetzt bis zu 1024 Tasten an (vorher 64).

### Gelöst: Flackernde sprechende Figuren im Schloss (15.11.2026)

Spieltest: Im Schloss zerfiel die gerade sprechende Figur alle paar Bilder für ein Bild
(Körper über dem Kopf, Kopf nach unten verschoben); im Haus davor nicht. Vergleich mit
DeSmuME an derselben Stelle: Standbilder gleich, das Flackern fehlt dort.

Ursache: Zeitpunkt des 3D-Zeichnens. Figuren bestehen aus 16×16-Texturteilen; beim Sprechen
wechselt die Animationsphase, und das Spiel ordnet dabei Kopf und Körper im Textur-Speicher
neu an (z. B. Kopf `0x1e0` → `0x1c0`, Körper `0x1b8` → `0x1e0`). Ablauf im Spiel
(`BIS_SWAPLOG`, Watchpoint mit Bildzeile): Liste L(N) wird im VBlank N in Zeile ~196
eingereicht, die Texturen dafür in Zeile ~197 hochgeladen; im VBlank N+1 wird L(N)
übernommen, kurz danach kommen schon die Texturen für L(N+1). Wir zeichneten früher *vor*
dem Wechsel (also eine Liste zu alt) – alte Geometrie mit neuen Texturen ergab den Zerfall.
Ein Versuch, wie die Hardware ab Zeile 215 zu zeichnen, half nicht: Leerlauf springt bei uns
direkt zum VBlank, die Zeile wird dann nie erreicht. Jetzt wird direkt nach dem Wechsel im
VBlank gezeichnet (`gpu3d_vblank`): Liste und Texturen passen zusammen, und das Bild ist
nebenbei ein Frame aktueller. Spielablauf und Prüfsummen unverändert.

Neue Hilfen: `BIS_3DRECT` / `BIS_RENDERLOG` (Polygone eines Bildbereichs, beim Zeichnen mit
Prüfsumme der Texturbytes), `BIS_TEXRAW` (Textur-Speicher roh), `BIS_SWAPLOG`,
`BIS_DUMP_FROM`, `BIS_NOGQ` (FIFO-Anhalten abschalten), `BIS_RWATCH=adresse:länge`.

### Offen: 10 s schwarzer oberer Bildschirm vor dem Haus

Frame ~3300–4000 (Lauf „Neues Spiel“) ist oben ~700 Frames schwarz, bevor der Toad das Haus
seiner Mutter betritt. Unabhängig von `BIS_CARD_DELAY`. In dieser Zeit liest das Spiel nur
`SNDSharedWork` (`0x02060a00`: `+0` finishCommandTag, `+4` playerStatus). Mit
`BIS_SND_STATUS=1` (Spieler gelten als „spielt“) bleibt es dauerhaft schwarz. Die Szene ist
offenbar auf den Sound abgestimmt und endet ohne ihn erst über ein Zeitlimit → mit der
Sound-Nachbildung angehen.

### Gelöst: Sprechblasen (15.11.2026)

Die Sprechblase („Mom, I'm home!“ bei Frame ~4100 im Lauf „Neues Spiel“) liegt auf BG1 von
Engine A (Text-BG, Kacheln ab `0x06000000`, Kachelplan ab `0x06004800`). Das Spiel schrieb
sie dorthin, aber auf `0x06000000` lag **keine VRAM-Bank**: Die Schreibzugriffe gingen ins
Leere (wie auf echter Hardware ohne Zuordnung). Drei Fehler:

1. **Recompiler, Sprungtabellen mit `addge`.** mwcc erzeugt neben
   `cmp rN, #k / addls pc, pc, rN, lsl #2` auch
   `cmp rN, #k / bgt … / cmp rN, #0 / addge pc, pc, rN, lsl #2`. Das letzte `cmp` ist dort
   nur die Untergrenze; der Recompiler nahm es als Obergrenze und erzeugte nur `case 0`.
   Betroffen waren 15 Tabellen, darunter `GX_SetBankForBG` (`0x02035688`): Der Aufruf mit
   Bank E (Frame 2232) schrieb nie `VRAMCNT_E = 0x81`. Jetzt wird bei anderen Bedingungen als
   `ls`/`lo` die Tabelle selbst ausgemessen (aufeinanderfolgende `b`-Befehle ab pc+8). Auch
   die 12 `addpl`-Tabellen haben jetzt die richtige Länge (vorher eine fremde Konstante,
   für gültige Indizes harmlos).
2. **GX-FIFO hält nach `SWAP_BUFFERS` bis zum VBlank an.** Bei uns lief die
   Geometrie-Engine weiter: Das Spiel wartet in `func_02036c6c` mit
   `while (GXSTAT & 0x08000000)` darauf und schickte so bis zu vier Szenen pro Bild in
   dieselbe Liste (Polygonspeicher voll, 6144 Vertices). Jetzt werden Befehle nach dem Swap
   gepuffert und im VBlank ausgeführt; GXSTAT meldet „beschäftigt“, Füllstand, halb voll und
   leer, der FIFO-Interrupt richtet sich danach (`gpu3d.c`: `gq_*`).
3. **Warteschleifen auf GXSTAT** dürfen jetzt ebenfalls übersprungen werden (GXSTAT ändert
   sich nur durch GX-Befehle oder im VBlank, ein Ereignis). Ohne das dauerte ein Lauf von
   4500 Frames 4½ min statt 30 s; die Prüfsumme ist mit und ohne `BIS_NOSPIN` gleich.

Nebenbei erscheint jetzt auch das Ortsschild „Toad Town“. Neue Referenz-Prüfsummen (der
Spielablauf ändert sich durch 1. und 2. absichtlich): ohne Eingabe (3000)
`666210be1076f442`, `1100:start,1400:a` (3000) `bd1d82a4df0d85ac`, Neues Spiel
`1100:start,1400:a,1900:a,2200:a` (3600, Standard-Budget) `de7e0c8edb5b2b72`.
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
