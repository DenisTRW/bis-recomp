# Sound – Ersatz für den ARM7-Sound-Treiber (`recomp/runtime/sound.c`)

Auf dem DS spielt der ARM7 den Ton: Der ARM9 schickt über IPC-Kanal 7 Adressen von
Befehlslisten (`SND_FlushCommand`, `0x0203b410`), der ARM7-Treiber des NitroSDK arbeitet sie ab,
spielt Sequenzen und steuert die 16 Hardware-Kanäle. Wir ersetzen diesen Treiber in C.

## Schnittstelle zum Spiel

- **Befehlsliste:** verkettete Einträge `next, id, arg[4]` (0x18 Bytes). Nach jeder Liste
  erhöht der ARM7 `SNDSharedWork.finishCommandTag` (in `arm7_hle.c`).
- **SNDSharedWork** (Zeiger per Befehl 29, hier `0x02060a00`): `+0` finishCommandTag,
  `+4` playerStatus (Bit je aktivem Spieler), `+8` channelStatus, ab `+0x20` je Spieler
  0x24 Bytes (16 lokale Variablen s16 + Tick-Zähler), `+0x260` globale Variablen.
- **Alarme:** `SETUP_ALARM` (18) + `START_TIMER` (12). Bei Ablauf schickt der ARM7
  `Alarmnummer | ID << 8` über Kanal 7 zurück; der ARM9-Rückruf (`0x0203b748` → `0x0203b8fc`)
  ruft die Funktion aus seiner Alarm-Tabelle auf. Damit füllen Streams ihre Puffer nach.
- **Wichtig für den Spielablauf:** Nach `PREPARE_SEQ` (2) muss der Spieler in `playerStatus`
  als aktiv erscheinen, erst dann schickt das Spiel `START_PREPARED_SEQ` (3). Endet die
  Sequenz, muss das Bit wieder fallen. Ohne das starteten keine Sequenzen, mit „immer aktiv“
  blieb die Szene vor dem Haus schwarz.

Befehlsnummern (NitroSDK): 0 START_SEQ, 1 STOP_SEQ, 2 PREPARE_SEQ, 3 START_PREPARED_SEQ,
4 PAUSE_SEQ, 5 SKIP_SEQ, 6 PLAYER_PARAM, 7 TRACK_PARAM, 8 MUTE_TRACK, 9 ALLOCATABLE_CHANNEL,
10/11 lokale/globale Variable, 12/13 START/STOP_TIMER, 14/15/16 Kanal PCM/PSG/Rauschen,
18 SETUP_ALARM, 19/20/21 Kanal-Timer/Lautstärke/Pan, 26/27 LOCK/UNLOCK_CHANNEL,
28 STOP_UNLOCKED_CHANNEL, 29 SHARED_WORK, 30–32 Cache-Hinweise, 33 Treiberinfo.

`SETUP_CHANNEL_PCM` (aus `0x0203af64`): arg0 = Kanal | Periode << 16, arg1 = Daten,
arg2 = Länge (Worte) | Shift << 22 | Lautstärke << 24, arg3 = Loop-Start | Pan << 16 |
Format << 24 | Loop << 26. Periode 0x200 = 32728 Hz.

## Umfang des Ersatzes

| Teil | Stand |
|---|---|
| Befehle, Status, Variablen, Tick-Zähler | ✅ |
| Sequencer: Timing (Tempo, 240er-Zähler, Takt 64·2728 Zyklen), Noten, Warten, Sprünge, Aufrufe, Schleifen, Variablen/Vergleiche, Zufall/Variablen-Präfix | ✅ |
| Instrumente aus SBNK (PCM, PSG, Rauschen, Drum-Sets, Key-Splits), Samples aus SWAR (PCM8/16, IMA-ADPCM) | ✅ |
| Hüllkurve ADSR, Lautstärke/Pan/Tonhöhe in SDK-Einheiten (0,1 dB, 1/64 Halbton) | ✅ (Lautstärke-Tabelle berechnet, nicht aus dem SDK übernommen) |
| Kanalvergabe nach Priorität, gesperrte Kanäle | ✅ |
| Direkte Kanäle (Streams, z. B. kurze Sprach-Clips) mit Alarmen | ✅ |
| Modulation (LFO), Portamento, Sweep, Capture, Effekte | FEHLT |
| Mixer 32728 Hz Stereo, Ausgabe über SDL3 (`BIS_MUTE=1` = aus) | ✅ |

Zeitbasis: Der Treiber läuft im VBlank (`sound_vblank`) und rechnet je Bild 560190 Zyklen
weiter; Sequencer-Takte und Alarme fallen in dieser Zeit in der richtigen Reihenfolge an.
Damit ist alles deterministisch (gleiche Prüfsummen bei gleichem Lauf). Speicherzugriffe
gehen über `platform_mem` an der Zeitmessung des ARM9 vorbei – der ARM7 läuft parallel.

## Fehlersuche

| Umgebungsvariable | Wirkung |
|---|---|
| `BIS_SNDLOG=1` | Befehle protokollieren (ohne Lautstärke/Pan) |
| `BIS_SNDSTAT=n` | alle n Bilder aktive Spieler (Sequenz, Tick) und Kanäle (`d` = direkt) |
| `BIS_WAV=datei` | ohne Fenster den Ton als WAV schreiben (zum Anhören/Messen) |

Behobener Fehler beim Aufbau: Der ADPCM-Dekoder dekodierte bei tiefer gespielten Noten
(dasselbe Sample mehrfach) jedes Mal von vorn – der Mixer war dadurch 8× zu langsam.
