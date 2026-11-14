# Thread-Modell (in Arbeit, Zweig `thread-model`)

Referenzpunkt vor dieser Arbeit: Tag `ref/boot-140` (= `f1ecd6d`). Gebautes Testprogramm
und Bericht dieses Stands liegen lokal in `build/ref/boot-140/`.

## Analyse der SDK-Funktionen

| Adresse | Funktion | Verhalten |
|---|---|---|
| `0x020390bc` | `OS_SaveContext(ctx)` | sichert Register in `ctx`, gibt 0 zurück; beim späteren Fortsetzen „kommt sie nochmal zurück“ mit 1 (setjmp-Prinzip). Ruft `0x0203ef60` (Divider-Zustand sichern) |
| `0x02039108` | `OS_LoadContext(ctx)` | lädt Register aus `ctx` und springt hinein (`subs pc, lr, #4`), kehrt nie zurück. Ruft `0x0203efa0` (Divider-Zustand laden) |
| `0x020389a8` | `OS_RescheduleThread` | `cur = *(info+8)`, `next = OS_SelectThread()`; bei `next != cur && next != 0`: `SaveContext(cur)` (≠0 → return), Callbacks `info+0`, `0x02060604+0xc`, `info+0x28 = next`, `LoadContext(next)` |
| `0x02038ea4` | `OS_SelectThread` | erster Thread der Liste (`0x020605e0+0x2c`, nach Priorität) mit Zustand 1 = bereit |
| `0x02038dc4` | `OS_SleepThread(queue)` | Zustand = 0, in Queue eintragen, `OS_RescheduleThread` |
| `0x02038cd4` | (Thread-Ende/-Zerstörung) | ruft ebenfalls `OS_LoadContext` |
| `0x01ff8dd8` | Thread-Wechsel nach IRQ | weckt alle Threads der IRQ-Queue `0x027e11f0` (Zustand 1), prüft Flag `0x02060604`; falls gesetzt: ersten bereiten Thread wählen, IRQ-Kontext in aktuellen Thread sichern, neuen laden |

Aufrufer von `OS_RescheduleThread`: `0x02038dc4`, `0x02038e14`, `0x02038e7c`, `0x02038ecc`, `0x02038ee8`.

**Kontext-Layout (Anfang der Thread-Struktur):**
`+0x00` CPSR, `+0x04…+0x34` r0–r12, `+0x38` sp, `+0x3c` lr, `+0x40` pc+4,
`+0x44` SVC-sp, `+0x48` Divider-Kontext. Thread: `+0x64` Zustand (0 wartet, 1 bereit, 2 beendet?),
`+0x68` nächster in Liste, `+0x78` Queue, `+0x80` Queue-Verkettung.

**Thread-Info:** `0x020605e0` (`+0x04` ≠ 0 → kein Wechsel, `+0x08` → aktueller Thread,
`+0x26` Wechsel gesperrt, `+0x28` neuer Thread, `+0x2c` Listenkopf).
`0x02060604`: `+0` Wechsel-angefordert-Flag, `+0x0c` Wechsel-Callback.

## Geplanter Entwurf

**Fibers:** Jeder SDK-Thread bekommt einen eigenen Host-Fiber (Windows `CreateFiberEx`)
mit eigenem C-Stack. Der CPU-Kontext (r0–r15, Flags) wird beim Wechsel pro Thread
gesichert und beim Fortsetzen wiederhergestellt.

**HLE-Ersatz (vom Recompiler eingebunden, Original bleibt als `rc_orig_…` erhalten):**
- `OS_SaveContext`: Register in den DS-Kontext schreiben (für Fehlersuche), laufenden Fiber
  an den Thread binden, 0 zurückgeben.
- `OS_LoadContext(next)`: CPU sichern → zum Fiber von `next` wechseln (neu anlegen, falls
  der Thread noch nie lief: Start bei `pc+4 − 4` mit r0/sp/lr aus dem Kontext). Beim
  Zurückkommen CPU wiederherstellen und normal zurückkehren. Das entspricht dem
  Original-Pfad „SaveContext gab 1 zurück → return“.
- `OS_SelectThread`: Original aufrufen; liefert es 0 (kein Thread bereit = Leerlauf),
  vergeht ein Frame (Interrupts), danach erneut wählen. Mit Grenze für Deadlock-Erkennung.
- `0x01ff8dd8`: Threads der IRQ-Queue wecken wie im Original, aber **nicht innerhalb des
  Interrupts umschalten**. Der Wechsel passiert danach am sicheren Punkt über die
  Neuwahl in `OS_SelectThread`.

**Deterministisch:** Interrupts gibt es nur an festen Punkten (Leerlauf). Keine Uhrzeit,
kein Zufall. Jedes Ereignis (Thread anlegen, Wechsel, Interrupt, Wecken, Frame) kommt in
einen Trace mit Prüfsumme. Zwei Läufe müssen dieselbe Prüfsumme ergeben.

**Einzeltests (`recomp/test/test_sched.c`, ohne Spielcode):**
1. Ping-Pong zwischen zwei Threads, 1000 Wechsel: Register und lokale Stack-Muster bleiben erhalten.
2. Neuer Thread: Start mit r0/sp aus dem Kontext, Rückkehr → Ende-Funktion wird gerufen.
3. Interrupt am sicheren Punkt: CPU-Kontext ist danach unverändert.
4. Determinismus: gleiches Szenario zweimal → gleiche Trace-Prüfsumme, anderes → andere.
5. Tiefe Rekursion in einem Fiber, dann Wechsel → Stack intakt.

## Nächste Schritte

1. `recomp/runtime/sched.c/.h` (Fibers, Kontext sichern/laden, Trace)
2. `recomp/test/test_sched.c` (Tests 1–5)
3. Recompiler: HLE-Override-Liste (`recomp/hle.txt`), Original als `rc_orig_…`
4. HLE für die vier Funktionen oben, Einbau in `boot_test.c`, Leerlauf-Takt aus
   `OS_SleepThread` nach `OS_SelectThread` verlegen
5. Start-Test laufen lassen, mit `build/ref/boot-140/bericht.txt` vergleichen
