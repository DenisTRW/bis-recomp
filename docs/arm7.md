# ARM7-Ersatz und Spielkarte

Auf dem DS verwaltet der ARM7 Karte (Spielstand), Sound, Touch, Mikrofon, Uhr und Power.
Wir führen keinen ARM7-Code aus. `recomp/runtime/arm7_hle.c` beantwortet die
PXI-Nachrichten des ARM9 so, wie der ARM7 es täte.

## PXI-Format (NitroSDK)

32-Bit-Wort über die IPC-FIFO: Bits 0–4 Kanal, Bit 5 Flag, Bits 6–31 Daten.
ARM9 → ARM7: Schreiben auf `0x04000188`. ARM7 → ARM9: Wort in der Empfangs-FIFO
`0x04100000`, Status in `0x04000184` (Bit 8 = leer), Interrupt-Bit 18. Der
PXI-Interrupt-Handler des Spiels verteilt die Antworten an die registrierten Callbacks.

## Kanäle

| Kanal | Bibliothek | Stand | Kategorie |
|---|---|---|---|
| 11 | Karte (`CARDi_Request`) | INIT, IDENTIFY, Spielstand lesen/schreiben | SIMULIERT |
| 5 | Uhr (RTC) | Datum/Zeit lesen mit **fester Zeit** (Fr, 09.10.2009, 12:00) | SIMULIERT |
| 6 | Touch (TP) | Befehle bestätigen, Messung = „kein Stylus“ | SIMULIERT |
| 7 | Sound (SND) | noch nicht beantwortet (Init schickt `0x02060c80`) | FEHLT |
| 8, 9 | Power, Mikrofon | noch nicht gesehen | – |

### Kanal 11: Karte

`CARD_Init` (`0x020417dc`): gemeinsamer Zustand `0x02062740`, Befehlspuffer `0x020626e0`,
Karten-Thread `0x02062784` (Start `0x02042928`), Callback `0x020428f4`.

Ablauf: ARM9 schickt Befehlsnummer (Flag 1). Bei `INIT` folgt die Pufferadresse. Antwort:
Wort mit Flag 1, Ergebnis in `cmd+0` (0 = Erfolg).

| Nr. | Befehl | beobachtet |
|---|---|---|
| 0 | INIT | ✅ beim Start |
| 2 | IDENTIFY | ✅ |
| 6 | READ_BACKUP | ✅ 0x100-Blöcke ab 0x000…0x700 nach `0x02062860` |
| 7 | WRITE_BACKUP | angenommen (SDK-Reihenfolge), noch nicht gesehen |

Befehlspuffer: `+0x00` Ergebnis, `+0x04` Typ (`0x0d01`), `+0x0c` Quelle, `+0x10` Ziel,
`+0x14` Länge, `+0x18` Gesamtgröße (`0x2000` = **8 KB EEPROM**), `+0x1c` Seitengröße (`0x20`).

Spielstand: leerer Baustein (`0xFF`), optional aus Datei (`arm7_set_save_path`).

### Kanal 5: Uhr

ARM9 schickt `Befehl << 8` (`0x10` Datum+Zeit, `0x11` Datum, `0x12` Zeit). Der ARM7 legt
BCD-Werte ab: Datum `0x027ffde8` (Jahr | Monat<<8 | Tag<<16 | Wochentag<<24), Zeit
`0x027ffdec` (Stunde | Minute<<8 | Sekunde<<16), und antwortet `Befehl << 8` (Ergebnis 0).
Während des Wartens läuft `0x02041574` in einer engen Schleife ohne Funktionsaufruf.

### Kanal 6: Touch

Mehrteilige Befehle, Sender z. B. `0x0203f490` (`TP_RequestAutoSamplingStart`):
erstes Paket `Bit25 | Befehl<<8 | Arg`, letztes `Bit24 | 0x10000 | Arg`. Antwort
`Bit24 | Befehl<<8 | 0`. `TP_WaitBusy` (`0x0203fa98`) wartet auf das Beschäftigt-Bit
`1 << Befehl` in `0x020625f0+0x3a`. Rohdaten: `0x027fffaa`.

## Spielkarte (ROM-Bus) – `recomp/runtime/card.c`

Das SDK liest ROM-Daten **direkt** über die Karten-Register, nicht über den ARM7:
`0x040001a4` ROMCTRL (Bit 31 Busy, Bit 23 Wort bereit, Bits 24–26 Blockgröße),
`0x040001a8` 8 Befehls-Bytes, `0x04100010` Datenport. Nachgebildet: `B7` (Daten lesen),
`B8` (Chip-ID). Die Bus-Verschlüsselung macht die Hardware und ist für die Software
unsichtbar. Karten-DMA (Timing 5) ist eingebaut, wird beim Start aber nicht benutzt.

**Firmware-Ersatz vor dem Start:** ROM-Header (0x170 Bytes) nach `0x027ffe00`, Chip-ID
`0x00007fc2` nach `0x027ff800` und `0x027ffc00`, Boot-Typ 1 nach `0x027ffc40`. Ohne den
Header richtet `FS_Init` (`0x0203decc`) das ROM-Archiv nicht ein.

## Interrupts in Warteschleifen

Einige Warteschleifen rufen keine Funktion auf (Uhr, `TP_WaitBusy`, Karte busy). Steht ein
Interrupt an, wird er deshalb auch beim nächsten **Lesezugriff des Spielcodes** ausgeliefert.
Das gilt nur bei erlaubten Interrupts, nie aus eigenem Ersatz-Code
(`platform_inhibit_irq`), und ohne Thread-Wechsel (der kommt am nächsten Funktionsanfang).
Die Sperre darf nie über einen Thread-Wechsel bestehen bleiben, das wird bei jedem
`OS_LoadContext` geprüft.

## Stand Start-Test (15.11.2026)

- 451 verschiedene Funktionen, 5 Threads (neu: `0x0205f860`, Start `0x0203306c`).
- Karte: rund 250 Lesebefehle, Dateisystem und **Overlay-Laden funktionieren** (Overlay 122 läuft).
- Erste 3D-Grafikbefehle (noch FEHLT).
- 0 Scheduler-Verstöße, deterministisch (Trace `0abe843a032099cb`).
- **Aktueller Halt:** Nullzeiger in der Speicherverwaltung des Spiels (`0x02019eac`),
  Verwaltungstabelle bei `DAT_0201a04c` leer. Vermutung: Arena-Grenzen des SDK
  (`OS_InitArena`) fehlen im Systembereich.
