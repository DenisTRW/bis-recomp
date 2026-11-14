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
