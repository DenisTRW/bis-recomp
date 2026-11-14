# RecompLauncher-Unterstützung

Grundlage: Anleitung `https://recomplauncher.com/sdk-files/recomplauncher-agent.md` und Schema
`https://recomplauncher.com/schema/recomplauncher.json` (Format 1).

## Grundsatz

Jeder braucht sein eigenes ROM. Das PC-Programm (`bis_pc.exe`) enthält den aus dem ROM
übersetzten Spielcode und wird deshalb **nicht** verteilt. Das Repo enthält nur den
„Übersetzer“ (Recompiler, Laufzeit, Launcher). Falls es später Releases gibt, enthalten sie
nur die Werkzeuge, und das Spiel wird beim Einrichten auf dem PC des Spielers aus dessen ROM
gebaut. Bis dahin passt `recomplauncher.json` zu einem **lokalen Build** (im RecompLauncher
über den Developer-Tab als lokalen Build hinzufügen).

## Was ergänzt wurde

- `recomplauncher.json` (Stufe 1-3):
  - Eingabe `rom`: eigenes `.nds`-ROM, wird nach `orig/baserom.nds` kopiert.
  - `prepare`: entpackt die Spieldaten mit `tools/dsd.exe` nach `extract/eur`.
  - `start`: `build/pc/bis_pc.exe <extract/eur> --save <install>/saves/bis.sav`.
  - Einstellungen (als Argumente): Fenstergröße, Vollbild, Pixelfilter, Karten-Timing (erweitert).
  - `folders.saves`, `saves`, `exit_codes` (2 = Spieldaten fehlen).
- Laufzeit (eigener Commit): optionale Argumente `--scale`, `--fullscreen`, `--filter`,
  `--card-delay`, `--save`. Ohne sie verhält sich das Programm wie vorher. `--save` behebt
  nebenbei, dass Spielstände nie in eine Datei geschrieben wurden (jetzt alle 120 Frames und
  beim Beenden, beim nächsten Start geladen).
- `run_pc.bat` legt `recomplauncher.json` neben `bis_pc.exe`.
- `.gitignore`: `saves/`, `*.sav`.
- Prüfskript: `python -I recomp/tools/check_recomplauncher.py recomplauncher.json`.

## Was geprüft wurde

- Schema-Regeln der benutzten Felder mit dem Prüfskript: OK. (Nicht gegen die
  maschinenlesbare Schema-Datei validiert.)
- Bau von Testprogramm und PC-Version: ohne Fehler.
- Start ohne neue Argumente: gleiche Referenz-Prüfsumme `5e5287bf5783d551`; PC-Version startet
  mit und ohne neue Argumente.
- Spielstand: Datei (8192 Byte) geschrieben und beim zweiten Start geladen.
- `game.dumps` weggelassen: keine CRC32-Werte aus Redump/No-Intro vorhanden, nichts erfunden.

## Was das Recomp besser machen würde

| Punkt | Warum es Spielern wichtig ist | Größe |
|---|---|---|
| Einrichtung, die das Spiel beim Spieler aus dem eigenen ROM baut | Installation über den Launcher ohne verteilten Spielcode | groß |
| Controller über SDL3, komplett mit Controller spielbar | Big-Picture-Modus, Spielen vom Sofa | mittel |
| Touchscreen per Maus | DS-Spiel braucht den unteren Bildschirm | mittel |
| Sound | Spieler erwarten Ton | groß |
| `progress`-Zeilen beim Vorbereiten | Fortschrittsbalken statt nur Text | klein |
| Recomp Achievements | Erfolge im Launcher | mittel |

## Offen

- Badge „Get it on RecompLauncher“ im README: bewusst weggelassen, weil es (noch) nichts zum
  Herunterladen gibt; kein Katalog-Eintrag gefunden (Suche unvollständig).
- Ob `tools/dsd.exe` in einem Release mitgeliefert werden darf (Lizenz), ist ungeprüft.
- Ob `saves` Platzhalter wie `{install}` akzeptiert, war aus der Anleitung nicht eindeutig.
- Projekt beanspruchen: kostenloses Entwickler-Konto unter https://recomplauncher.com/dev.
