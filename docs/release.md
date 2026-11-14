# Release (nur Werkzeuge, Spiel wird beim Spieler gebaut)

Grundsatz: Ein Release enthält **nie** Spieldaten oder übersetzten Spielcode. Jeder Spieler
braucht sein eigenes ROM; das Spiel wird bei der Einrichtung auf seinem PC daraus gebaut.

## Ablauf beim Spieler (RecompLauncher)

1. Der Launcher lädt `bis-recomp-v<version>-Windows-x64.zip` und entpackt es.
2. Er fragt nach dem ROM und kopiert es nach `orig/baserom.nds` (`recomplauncher.json`, Eingabe `rom`).
3. Vorbereitung: `tools/python/python.exe -I recomp/setup_game.py --rom … --root …`
   entpackt das ROM, disassembliert, übersetzt, kompiliert und linkt `build/pc/bis_pc.exe`.
   Fortschritt über `progress`-Zeilen.
4. Start: `build/pc/bis_pc.exe <extract/eur> --save <install>/saves/bis.sav` plus Einstellungen.

Von Hand geht dasselbe mit `python -I recomp/setup_game.py --rom orig/baserom.nds`.

## Paket bauen

```
python -I recomp/make_release.py --version 0.1.0
```

Ergebnis: `build/release/bis-recomp-v0.1.0-Windows-x64.zip`. Enthalten sind alle von git
verfolgten Dateien, `tools/dsd.exe`, `tools/ninja.exe`, `tools/w64devkit`, SDL3 (x86_64-MinGW)
und `tools/python`. Das Skript bricht ab, wenn etwas nach Spieldaten oder erzeugtem Spielcode
aussieht.

## Stand

- Python 3.12.10 (embeddable, python.org) liegt in `tools/python` (nicht im Repo, nur im Paket).
- Lizenztexte in `licenses/`; Übersicht in `THIRD_PARTY_NOTICES.md`.
- Einrichtung mit dem mitgelieferten Python getestet: rund 2 Minuten von Grund auf.
- Auf GitHub zuerst als **Pre-Release** veröffentlichen.
- Eintrag beim RecompLauncher: Entwickler-Konto unter https://recomplauncher.com/dev, dann
  einreichen (Schritt des Projektinhabers).
