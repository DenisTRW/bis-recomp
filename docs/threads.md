# Thread-Modell (Zweig `thread-model`)

Referenzpunkt vor dieser Arbeit: Tag `ref/boot-140` (= `f1ecd6d`). Gebautes Testprogramm
und Bericht dieses Stands liegen lokal in `build/ref/boot-140/`.

## Stand (15.11.2026): funktioniert

```
recomp\test\run_tests.bat            # Einzeltests (Pad_Update + 7 Scheduler-Tests)
recomp\runtime\run_boot.bat 600 2000 # Start-Test mit Thread-Modell
```

**Umsetzung:**
- `recomp/runtime/sched.c/.h`: Fiber-Scheduler. Ein Fiber pro SDK-Thread, gemeinsamer
  CPU-Kontext, Selbstprüfung (Stack-Kanarienvogel, Kontext-Prüfsumme, kein Wechsel im
  Interrupt), deterministischer Trace mit Prüfsumme.
- `recomp/hle.txt`: Der Recompiler ersetzt dort gelistete Funktionen. Das Original
  bleibt als `rc_orig_…` erhalten.
- `recomp/runtime/threads_hle.c`: Ersatz für `OS_SaveContext`, `OS_LoadContext`,
  `OS_SelectThread` und `OS_IrqHandler_ThreadSwitch`.
- `boot_test.c`: Interrupts über `sched_irq`, nur bei freiem I-Bit (wie Hardware).
  `OS_Halt` (CP15 c7,c0,4 = „Warte auf Interrupt“) lässt einen Frame vergehen und plant
  danach neu. Zählerbasierter Takt alle 2^22 Speicherzugriffe an Funktionsanfängen.

**Ergebnis Start-Test:**
- 4 Threads: Start-Thread (bindet sich an `0x020606d4`), `0x02062784` (Start bei
  `0x02042928`), `0x020630a0` (`0x020433a8`) und der Leerlauf-Thread `0x02060614`
  (`0x02038fb8`: Interrupts an, Endlosschleife `OS_Halt`).
- Wechsel Start-Thread ↔ Leerlauf ↔ Interrupt funktionieren, 600 Frames stabil,
  **0 Scheduler-Verstöße**.
- **Deterministisch:** zwei Läufe → gleiche Trace-Prüfsumme (`7b1818c64a5cfe3e`, 1 211 Ereignisse).
- **Neuer Halt (nicht mehr Thread-Modell):** Der Start-Thread schickt zwei PXI-Nachrichten an
  **Kanal 11 (Dateisystem/Karte)**, die zweite mit Zeiger `0x020626e0` auf einen
  Befehlsblock, und wartet auf die Antwort des ARM7. → nächstes Thema: ARM7-Ersatz für den
  Kartenzugriff.

**Unterwegs gefunden und behoben:**
- Ein Wechsel im Interrupt wurde zunächst nicht erkannt, wenn der andere Thread sofort
  endete. Test 3 hat das gezeigt, jetzt wird schon beim Wechsel geprüft.
- dsd schreibt manche Sprungziele als rohen Offset relativ zur Befehlsadresse
  (`b #-0x8`). Der Recompiler wertet sie jetzt aus (+20 vollständig übersetzte Funktionen,
  u. a. Leerlauf-Thread und `OS_Terminate`).
- `0x0203a070` = `OS_Terminate`, `0x0203a080` = `OS_Halt`, `0x02038fb8` = Leerlauf-Thread,
  `0x02038cb4` = `OS_ExitThread`, `0x02039cdc` = `OS_EnableInterrupts`.

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
kein Zufall. **Einschränkung:** Wartet Code in einer Schleife ohne zu schlafen auf ein Flag,
das ein Interrupt setzt, käme dieser Interrupt nie. Deshalb gibt es zusätzlich einen
zählerbasierten Takt (z. B. alle N Speicherzugriffe). Der ist ebenfalls deterministisch. Jedes Ereignis (Thread anlegen, Wechsel, Interrupt, Wecken, Frame) kommt in
einen Trace mit Prüfsumme. Zwei Läufe müssen dieselbe Prüfsumme ergeben.

**Einzeltests (`recomp/test/test_sched.c`, ohne Spielcode):**
1. Ping-Pong zwischen zwei Threads, 1000 Wechsel: Register und lokale Stack-Muster bleiben erhalten.
2. Neuer Thread: Start mit r0/sp aus dem Kontext, Rückkehr → Ende-Funktion wird gerufen.
3. Interrupt am sicheren Punkt: CPU-Kontext ist danach unverändert.
4. Determinismus: gleiches Szenario zweimal → gleiche Trace-Prüfsumme, anderes → andere.
5. Tiefe Rekursion in einem Fiber, dann Wechsel → Stack intakt.

## Nächste Schritte (ursprünglicher Plan – erledigt)

1. ✅ `recomp/runtime/sched.c/.h` (Fibers, Kontext sichern/laden, Trace)
2. ✅ `recomp/test/test_sched.c` (Tests 1–5, dazu 6 „Prüfungen prüfen“ und 7 „Umbenennen“)
3. ✅ Recompiler: HLE-Override-Liste (`recomp/hle.txt`), Original als `rc_orig_…`
4. ✅ HLE für die vier Funktionen, Einbau in `boot_test.c`. Leerlauf über den echten
   Leerlauf-Thread (`OS_Halt`), der Leerlauf in `OS_SelectThread` ist nur noch Rückfallebene.
5. ✅ Start-Test: kommt weiter als `ref/boot-140` (dort Halt beim IRQ-Thread-Wechsel)
