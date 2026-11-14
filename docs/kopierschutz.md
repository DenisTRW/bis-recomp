# Kopierschutz (DSProtect) und selbstmodifizierender Code

Das Spiel enthält die Kopierschutz-Middleware **DSProtect** (dsd kennt sie: Symbole mit
`dsprot_range=(…)`). Sie prüft beim Start, ob das Spiel auf einer echten Karte läuft.
Schlägt eine Prüfung an, läuft das Spiel scheinbar normal weiter, **reagiert aber in der
Dateiauswahl nicht auf Tasten**. Für den Port muss die Prüfung aus denselben Gründen
bestehen wie auf echter Hardware. Patchen wir nichts weg, verhält sich das Spiel wie im
Original.

## Ablauf

1. Beim Start lädt das Spiel ov138 (Schutz-Bibliothek) und ov139. Die statische
   Initialisierung von ov139 ruft die Prüfungen in ov138 auf:
   - `0x02064144`: Prüfungen `0x0206403c` (Karten-Lesezugriffe) und `0x0206449c`
     (eigener Code unverändert). Bestehen beide, ruft sie den Rückruf `0x020647c0` auf.
   - `0x0206422c`: Prüfungen `0x02063cac` (MAC-Adresse) und `0x020643dc` (eigener Code).
     Schlägt eine an, ruft sie den Rückruf mit **anderen** Daten auf.
   - `0x02064314`: leere Liste.
2. Der Rückruf `0x020647c0` entschlüsselt 5 Worte und schreibt sie über den Anfang der
   ITCM-Funktion `func_01ff8d18`. Mit den „guten“ Daten entsteht ein Trampolin zu
   `Pad_ReadAndUpdate`, mit den „schlechten“ ein `bx lr`, also keine Tasten.
3. Die Hauptschleife (`func_01ff8bb8`) schreibt jedes Frame vor ihrem Aufruf das
   Original zurück, ruft `func_01ff8d18` als Frame-Funktion auf (Sound, Szenen) und setzt
   danach den Patch wieder ein.
4. Die Szenen-Overlays (z. B. ov129 `0x0206db20`) springen mit einem verschleierten
   `eor pc, r1, r2, ror r1` über eine verschlüsselte Zeigertabelle nach `0x01ff8d18`, also
   in den Patch. Er entschlüsselt den zweiten Zeiger der Tabelle (Tasten-Thunk
   `0x0206e398`) und springt dorthin.

Die geschützten Funktionen entschlüsseln Teile ihres eigenen Codes zur Laufzeit
(`0x02063b00`) und verschlüsseln sie danach wieder (`0x02063bc8`). dsd zeigt den Code bereits
entschlüsselt, der Recompiler übersetzt also den echten Inhalt. Die Ver-/Entschlüsselung im
Speicher läuft trotzdem mit, sie verändert aber keinen übersetzten Code.

## Prüfungen und was der Ersatz dafür tut

| Prüfung | Echte Hardware | Ersatz | Kategorie |
|---|---|---|---|
| Karte: Blöcke bei `0x1000`, `0x1200` und `0x8000` lesen, Prüfsummen gleich | Lesen unter `0x8000` wird auf `0x8000–0x81FF` umgeleitet | `card.c` macht dieselbe Umleitung | ORIGINAL |
| MAC-Adresse `0x027FFCF4` (von der Firmware) ≠ 0 und ≠ `00:09:BF:00:00:31` | Jedes DS hat eine eigene | feste, frei gewählte Adresse `00:09:BF:5A:3C:71` | SIMULIERT |
| Code der Prüffunktionen unverändert | – | Speicher enthält Originalbytes | ORIGINAL |

## Selbstmodifizierender Code – `recomp/runtime/smc.c`

Statisch übersetzter Code kennt nur die ROM-Befehle. Für `0x01ff8d18` prüft
`hle_itcm_01ff8d18` bei jedem Aufruf die 5 Worte im Speicher:

- Original → übersetzte Originalfunktion
- Patch → genau die 4 Patch-Befehle in C
- etwas anderes (z. B. der `bx lr`-Patch nach fehlgeschlagener Prüfung) → **Abbruch** mit
  Meldung. Wir tun also nie still das Falsche.

Weitere Stellen mit selbstmodifizierendem Code sind bisher nicht bekannt. Ein Watchpoint auf
`0x01ff8000:8000` zeigt Schreibzugriffe aufs ITCM.

## Recompiler-Änderungen dafür

- Symbole mit verschachtelten Klammern (`dsprot_range=(…)`) werden gelesen. Vorher landeten
  die DSProtect-Funktionen in einem Modul „unknown“, und Aufrufe dorthin blieben `RC_TODO`.
- `add lr, pc, #n … bx rN` wird zu Aufruf und Weiterlaufen an der Rücksprungadresse.
- Rechenbefehle mit `pc` als Ziel: berechneter Sprung, untere 2 Bits ignoriert wie auf der
  Hardware.
