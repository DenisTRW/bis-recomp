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

## Was noch fehlt

- **Python (embeddable) in `tools/python`**: muss einmal heruntergeladen werden
  (python.org, „Windows embeddable package (64-bit)“). Erst danach kann das Paket gebaut werden.
- Lizenztexte liegen bei: w64devkit (`COPYING.MinGW-w64-runtime.txt`, README), SDL3
  (`LICENSE.txt`); für dsd (MIT), Ninja (Apache 2.0) und Python fehlen die Texte noch im Paket.
- Auf GitHub zuerst als **Pre-Release** veröffentlichen.
- Eintrag beim RecompLauncher: Entwickler-Konto unter https://recomplauncher.com/dev, dann
  einreichen (Schritt des Projektinhabers).
