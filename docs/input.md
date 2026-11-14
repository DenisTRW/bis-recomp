# Input: Tasten, Touchscreen, Mikrofon

Diese Funktionen sind die Andockpunkte für die spätere PC-Steuerung
(Tastatur, Maus als Touchscreen, Taste statt Mikrofon, Controller).

Alle Adressen gelten für die EU-Version `CLJP` Rev 0. Gefunden mit
`scripts/scan_consts.py` (Hardware-Adressen im Code) und Ghidra
(`scripts/ghidra/DecompileAt.java`, `ListCallers.java`).

## Hardware-Hintergrund

| Adresse | Name (NitroSDK) | Inhalt |
|---|---|---|
| `0x04000130` | `REG_KEYINPUT` | A, B, Select, Start, Steuerkreuz, R, L (aktiv-low) |
| `0x027FFFA8` | `HW_BUTTON_XY_BUF` | X, Y, Debug, Pen-Down, Bit 15 = DS zugeklappt (vom ARM7 geschrieben) |
| `0x027FFFAA` | `HW_TOUCHPANEL_BUF` | Rohdaten des Touchscreens (vom ARM7 geschrieben) |

Touchscreen und Mikrofon hängen am ARM7. Der ARM9 bekommt die Daten über die
PXI-FIFO, und jede SDK-Bibliothek registriert dafür einen Callback auf ihrem Kanal.

## Tasten (ARM9 main)

| Adresse | Name (vergeben) | Was sie tut | Sicherheit |
|---|---|---|---|
| `0x02019a2c` | `Pad_ReadAndUpdate` | Liest `REG_KEYINPUT` + X/Y-Puffer, invertiert, maskiert und ruft `Pad_Update` | hoch |
| `0x02019a74` | `Pad_Update` | Berechnet aus dem neuen Tastenzustand: gehalten, neu gedrückt, losgelassen, Auto-Repeat (10 Tasten, eigene Zähler) | hoch |
| `0x02005f24` | `Pad_CheckSoftReset` | Liest die Tasten und prüft die Kombination `0x30C` = L+R+Start+Select | hoch |
| `0x02042b5c` | – | Prüft Bit 15 im X/Y-Puffer (zugeklappt) und behandelt den Schlafmodus | mittel |

Struktur, die `Pad_Update` beschreibt (Zeiger als 1. Argument):

| Offset | Typ | Bedeutung |
|---|---|---|
| `0x0` | u16 | gehalten (aktueller Zustand) |
| `0x2` | u16 | neu gedrückt (Trigger) |
| `0x4` | u16 | losgelassen (Release) |
| `0x6` | u16 | Auto-Repeat |
| `0x8` | u8 | Repeat-Startverzögerung |
| `0x9` | u8 | Repeat-Intervall |
| `0xA` | u8[10] | Repeat-Zähler pro Taste |

**Für den PC-Port:** `Pad_ReadAndUpdate` ist der zentrale Punkt. Ersetzt man dort
das Lesen der Register durch SDL-Tastatur/Controller, funktioniert die gesamte
Auswertung (`Pad_Update`) unverändert weiter.

`Pad_ReadAndUpdate` wird direkt aus diesen Overlays aufgerufen (vermutlich die
Hauptschleifen der einzelnen Spielmodi): 1, 4, 5, 7, 8, 122, 125, 128, 129, 130, 131.
`Pad_Update` direkt aus: 1, 123, 124.

## Touchscreen (ARM9 main, NitroSDK `TP`)

| Adresse | Name (SDK) | Was sie tut | Sicherheit |
|---|---|---|---|
| `0x0203f25c` | `TP_Init` | Setzt den TP-Zustand zurück, registriert `TPi_TpCallback` auf PXI-Kanal 6 | hoch |
| `0x0203efdc` | `TPi_TpCallback` | Empfängt Touch-Daten: X (12 Bit), Y (12 Bit), berührt (1 Bit), Gültigkeit (2 Bit); Kommando `0x10` = Auto-Sampling in Ringpuffer | hoch |
| `0x0201e380` | – | Ruft `TP_Init` auf (Teil der Spiel-Initialisierung) | – |

**Für den PC-Port:** Die Mausposition im Fenster des unteren Bildschirms wird
in DS-Touchkoordinaten (256×192) umgerechnet und in genau das Format geschrieben,
das `TPi_TpCallback` erzeugt. Damit funktionieren alle Touch-Abfragen des Spiels.

## Mikrofon (ARM9 main, NitroSDK `MIC`)

| Adresse | Name (SDK) | Was sie tut | Sicherheit |
|---|---|---|---|
| `0x0203fac4` | `MIC_Init` | Registriert den MIC-Callback auf PXI-Kanal 9 | hoch |
| `0x0203fd3c` | `MICi_Callback` | Empfängt Antworten/Daten vom ARM7 | hoch |
| `0x0203fc60`, `0x0203fcf4`, `0x0203fd2c` | – | Weitere MIC-Funktionen (vermutlich Sampling starten/stoppen, Puffer abfragen) | offen |

Benutzt wird das Mikrofon nur in den Overlays **81, 101, 120, 125, 131**.
Welche Spielszenen das sind, ist noch offen. Overlays 81, 101, 125 und 131
prüfen außerdem den Zugeklappt-Bit.

**Für den PC-Port:** Statt echter Samples liefern wir beim Drücken einer Taste
(oder Mausklick) einen künstlich „lauten“ Puffer. Optional echtes Mikrofon über SDL.

## Weitere PXI-Kanäle (zur Orientierung)

| Kanal | Bibliothek | Registriert in |
|---|---|---|
| 5 | RTC (Uhr) | `0x02040e8c` |
| 6 | TP (Touch) | `0x0203f25c` |
| 7 | SND (Sound) | `0x0203b76c` |
| 8 | PM (Power) | `0x02040100` |
| 9 | MIC | `0x0203fac4` |
| 0xB | FS | `0x020417dc` |
| 0xC | OS | `0x02039d88` |
| 0xD | CTRDG | `0x02042fb0` |
| 0xE | CARD | `0x02042acc` |

Hilfsfunktionen: `0x0203c180` = `PXI_SetFifoRecvCallback(tag, callback)`,
`0x0203c1cc` = `PXI_IsCallbackReady(tag, proc)`.

## Offen

- MIC-Funktionen `0x0203fc60`, `0x0203fcf4`, `0x0203fd2c` genau bestimmen
- Herausfinden, welche Spielszenen die Overlays 81, 101, 120, 125, 131 sind
- Wo das Spiel die Touch-Koordinaten abfragt (`TP_GetCalibratedPoint` o. ä.)
- Overlays in Ghidra laden
