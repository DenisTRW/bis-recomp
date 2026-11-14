# Recompiler: Befehls-Statistik und Folgerungen

Erzeugt mit:

```
tools\dsd.exe dis --config-path config\eur\arm9\config.yaml --asm-path build\asm
python -I scripts\insn_stats.py build\asm
```

Umfang: Hauptprogramm, ITCM, DTCM und alle 141 Overlays.

## Überblick

| | Wert |
|---|---|
| Funktionen | 12 196 ARM, 24 Thumb |
| ARM-Befehle | ~803 000 (54 verschiedene Mnemonics) |
| Thumb-Befehle | ~110 |

**Fast der gesamte Code ist ARM.** Die 24 Thumb-Funktionen sind winzige BIOS-Stubs
(`swi` + `bx lr`, z. B. `LZ77UnCompReadByCallbackWrite16bit`, `VBlankIntrWait`).
Der Recompiler braucht also im Kern nur einen ARM-Decoder. Thumb-Funktionen werden
direkt durch C-Implementierungen der BIOS-Funktionen ersetzt.

## Häufigste Befehle (ARM)

`ldr` 22 %, `mov` 21 %, `add` 10 %, `str` 7 %, `bl` 7 %, `b` 6 %, `cmp` 5 %, dann
`strh`/`ldrh`, `sub`, `ldm`/`stm`, `blx`, `strb`/`ldrb`, `orr`, `bic`, `ldrsh` …

Alles Standard-ARMv5TE, keine Exoten. Multiplikationen (`mul`, `mla`, `smull`,
`umull`, `smlal`, `umlal`) und die DSP-Befehle `smulxy`/`smlaxy` (~300×) sowie
`clz` (69×) lassen sich direkt in C ausdrücken.

## Schwierige Fälle

| Anzahl | Fall | Lösung im Recompiler |
|---:|---|---|
| ~35 500 | Bedingt ausgeführte Befehle (`addeq`, `movne`, …) | `if (cond) { … }` – einfach |
| ~20 000 | `ldm … {pc}` (Rücksprung) | `return` |
| ~14 000 | `blx <reg>` (indirekter Aufruf: Funktionszeiger, C++-vtables) | Nachschlagen in einer Tabelle „Adresse → C-Funktion“ (alle Funktionsgrenzen kennt dsd) |
| 725 | `bx <reg>` außer `bx lr` | Meist Tail-Calls über Zeiger → wie `blx <reg>` |
| 641 | `add pc, pc, rN, lsl #2` | `switch`-Sprungtabellen von mwcc → als `switch` übersetzen |
| 46 + 6 | `mcr`/`mrc` (CP15: Cache, MPU, TCM) | Weglassen bzw. Dummy – auf dem PC gibt es keinen Cache/MPU |
| 26 + 20 | `msr`/`mrs` (Interrupts an/aus) | Plattform-Schicht: Interrupt-Flag emulieren |
| 21 | `swi` (BIOS-Aufrufe) | C-Implementierungen (Dekompression, Division, Sqrt, CpuSet …) |
| 6 | `ldr pc, […]` | Einzeln ansehen |
| 6 | `<illegal>` | Vermutlich Daten, die dsd als Code markiert hat – prüfen |
| 1 | `swp` | Atomarer Tausch → normaler Tausch (Single-Thread) |

## Prototyp (`recomp/recomp.py`)

```
tools\dsd.exe dis --config-path config\eur\arm9\config.yaml --asm-path build\asm
python -I recomp\recomp.py .               # alles
python -I recomp\recomp.py . --only Pad_Update
```

Ausgabe in `build/recomp/`: eine `.c`-Datei pro Modul (`main.c`, `itcm.c`, `ov000.c` …),
`funcs.h` mit allen Prototypen und `functable.c` (Adresse → Funktion für indirekte Aufrufe).
Der Laufzeit-Header liegt in `recomp/include/recomp.h`.

**Erster Lauf (alle Module):**

| | |
|---|---|
| Funktionen vollständig übersetzt | 12 116 von 12 220 (99,1 %) |
| Befehle übersetzt | 801 791 von 802 835 (99,87 %) |
| Laufzeit | ~16 s |
| Erzeugter C-Code | ~83 MB |

Noch nicht übersetzt (werden als `RC_TODO("…")` markiert):

| Anzahl | Fall |
|---:|---|
| 34 | `b #offset` (roher Offset statt Label, z. B. in der ITCM-Dekompression) |
| 29 | Schreiben auf `pc` außer Sprungtabellen/Rücksprung |
| 24 + 4 | Sprung mitten in eine andere Funktion (`.L_02045174`, `.L_02076e24`) |
| 20 | Sprungtabelle ohne erkennbares `cmp` davor |
| ~40 | Sprungziel als rohe Zahl (`#0x3d64` …) |
| 8 | Thumb-Funktionen, die keine reinen BIOS-Stubs sind |
| 6 | `<illegal>` (Daten, die als Code markiert sind) |
| 3 | Literal-Pool *vor* der Funktion (`0x0203ece0`, siehe dsd-Warnung) |

### Bauen und Testen

C-Compiler: w64devkit (GCC 16.2) in `tools\w64devkit\`.

```
python -I recomp\recomp.py .
python -I recomp\gen_ninja.py . -O1
tools\ninja.exe -C build\recomp          # alle 136 Dateien, ~45 s auf 12 Kernen
recomp\test\run_tests.bat                # Einzeltests
```

**Stand:**
- Der gesamte erzeugte Code kompiliert fehlerfrei (136 Dateien).
- Alles zusammen linkt ohne offene Symbole zu einem Programm (11,5 MB, 12 220 Funktionen
  in der Funktionstabelle).
- `test_pad_update`: Die übersetzte `Pad_Update` wird mit 100 000 zufälligen Tastenfolgen
  gegen eine Referenz nach dem Ghidra-Pseudocode geprüft → **0 Fehler**.
  Die Masken-Tabelle `0x0204ac4c` enthält die Reihenfolge
  hoch/runter, rechts/links, A, B, X, Y, Start, Select, L, R.

Der Test-Harness (`recomp/test/harness.c`) bildet nur ITCM, Haupt-RAM und DTCM als flachen
Speicher nach. Alle Hooks (indirekte Aufrufe, BIOS, Systemregister) zählen nur mit. Das ist
die Keimzelle der späteren Plattform-Schicht.

**Bekannte Vereinfachungen im Prototyp:**
- Carry bei Immediate-Operanden mit Rotation (`movs r0, #0x…`) bleibt unverändert.
- Ein Rücksprung (`ldm sp!, {…, pc}`, `bx lr`) wird immer als C-`return` behandelt.
  Das stimmt, solange niemand die gespeicherte Rücksprungadresse manipuliert.
- Ein `pc`-Wert als Operand ist immer `Adresse + 8` (ARM-Pipeline). Bei Verschiebung um
  ein Register wären es eigentlich `+12`, das ist hier aber egal.
- Aufrufe zwischen Modulen gehen direkt an die Zielfunktion (dsd hat das Ziel-Overlay
  aufgelöst). **Ausnahme:** Haben mehrere Overlays an der Zieladresse eine Funktion
  (z. B. `0x02069820` in ov001/002/122/127), nennt dsd nur das erste. Solche Aufrufe aus
  einem anderen Modul gehen über `rc_call_indirect`, die Laufzeit erkennt das geladene
  Overlay am RAM-Inhalt (420 Aufrufstellen). Vorher lief nach dem Titelbildschirm Code aus
  ov001, obwohl ov127 geladen war.
- Rechenbefehle mit `pc` als Ziel (`eor pc, r1, r2, ror r1`, verschleierter Sprung in ov129
  `0x0206db20`) werden zu einem indirekten Sprung zur berechneten Adresse.
- **Mehrere Einstiege:** Springt Code auf ein Label mitten in einer fremden Funktion (64-Bit-
  Division `0x02045164`/`0x02045174`, `0x02045324` → `0x020451e8`, `0x02045578` →
  `0x02045580`), bekommt diese Funktion eine Variante `rc_<name>__at(c, einstieg)` mit
  `switch` auf das Label. Vorher waren das `RC_TODO`s, und die Projektionsmatrizen der
  3D-Grafik waren kaputt. Test: `recomp/test/test_lldiv.c` (~200 000 Zufallsfälle).
- DSProtect-Funktionen, `add lr, pc, #n … bx`, unteren pc-Bits: siehe
  [`kopierschutz.md`](kopierschutz.md).
- `recomp.py` löscht vor dem Schreiben alte erzeugte `.c`-Dateien im Ausgabeordner.

## Folgerungen für das Design

1. **Ein Decoder für ARM reicht** (plus ein paar BIOS-Stubs in C).
2. **Funktionsgrenzen von dsd übernehmen.** dsd kennt für jede Funktion Start, Größe
   und Modus und hat die Sprungziele schon aufgelöst.
3. **Indirekte Aufrufe sind häufig** (~14 700). Wir brauchen eine schnelle Tabelle
   „DS-Adresse → C-Funktion“. Bei Overlays hängt die Zuordnung davon ab, welches
   Overlay gerade geladen ist.
4. **Overlays teilen sich Adressen.** Jede Overlay-Funktion bekommt einen eindeutigen
   C-Namen (`ov081_func_020c9000`). Der Overlay-Lader aktualisiert die Tabelle.
5. **Register als Variablen:** Jede Funktion arbeitet auf einem gemeinsamen
   CPU-Kontext (r0–r15, Flags N/Z/C/V). Der C-Compiler optimiert das später weg.
6. **Speicherzugriffe** gehen über eine Schicht, die Haupt-RAM direkt abbildet und
   I/O-Register (`0x04xxxxxx`) an die Plattform-Schicht weiterleitet.
