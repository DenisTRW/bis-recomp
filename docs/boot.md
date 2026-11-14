# Start-Test: Wie weit kommt der recompilierte Code?

```
python -I recomp\recomp.py .
python -I recomp\gen_ninja.py . -O1
tools\ninja.exe -C build\recomp
gcc -O1 -w -DRC_TRACE -I recomp\include -I build\recomp -o build\test\boot_test.exe recomp\runtime\boot_test.c build\recomp\obj\*.o
build\test\boot_test.exe extract\eur [frames] [budget_mio] [funktion_hex]
```

`recomp/runtime/boot_test.c` ist die Keimzelle der Plattform-Schicht: Speicherkarte,
I/O-Register, BIOS, Interrupts, Funktionstabelle und ein Wachhund, der Endlosschleifen
abbricht und einen Bericht ausgibt.

## Startablauf

`Entry` (0x02004800) macht: IME aus, auf VCOUNT 0 warten, CP15/MPU/Cache einrichten,
DTCM/Paletten/OAM löschen, Autoload (ITCM/DTCM kopieren), BSS löschen,
`DTCM+0x3ffc = 0x01ff8d70` (Interrupt-Handler), dann `func_02045cb8`,
`func_02019a18`, `func_020471dc` und Sprung nach `main` (0x02004bd8).

Der Test übernimmt den Hardware-Teil selbst (lädt `arm9.bin`, `itcm.bin`, `dtcm.bin`
an ihre Adressen) und ruft danach dieselben Funktionen auf wie `Entry`.

## Hürden und ihre Lösung (Stand 14.11.2026)

| # | Hürde | Ursache | Lösung | Kategorie |
|---|---|---|---|---|
| 1 | Endlosschleife auf `0x04000180` (IPCSYNC) | `PXI_InitFifo` (0x0203c080) zählt mit dem ARM7 einen 4-Bit-Wert herunter | ARM7-Ersatz antwortet mit „Wert − 1“ | SIMULIERT |
| 2 | Endlosschleife in `PXI_IsCallbackReady` | wartet, bis der ARM7 seine Kanäle bei `0x027ffc00 + 0x38c` als bereit meldet | alle Bits gesetzt | SIMULIERT |
| 3 | Endlosschleife `OS_SleepThread` → `OS_RescheduleThread` → `OS_SelectThread` | Thread wartet auf Interrupt, kein anderer Thread lauffähig | beim Schlafen vergeht ein Frame, Interrupt wird ausgelöst | SIMULIERT |
| 4 | Interrupt-Handler wird nie aufgerufen | Spiel aktiviert nicht VBlank, sondern **VCount-Match** (IE = `0x00040004`: Bit 2 + Bit 18 IPC-Empfang) | pro Frame VCount-Match + VBlank auslösen | SIMULIERT |
| 5 | Interrupt-Handler kehrt falsch zurück | **Recompiler-Fehler:** `ldr lr, =func_01ff8dd8 / bx r0` wurde als normaler Tail-Call übersetzt | Recompiler erkennt das Muster: erst `r0` aufrufen, dann `func_01ff8dd8` | – |
| 6 | **Aktueller Halt:** `func_01ff8dd8` | Thread-Wechsel beim Verlassen des Interrupts (`subs pc, lr, #4`, Moduswechsel) | **offen → Thread-Modell nötig** | FEHLT |

Bis Hürde 6 laufen ~140 verschiedene Funktionen, darunter OS-, PXI-, RTC- und
Interrupt-Initialisierung, Overlay 1 und schon die Tastenabfrage (`Pad_ReadAndUpdate`).
Wichtige Funktionen:

| Adresse | Vermutung |
|---|---|
| `0x0203c080` | `PXI_InitFifo` |
| `0x02038dc4` | `OS_SleepThread` |
| `0x020389a8` | `OS_RescheduleThread` |
| `0x02038ea4` | `OS_SelectThread` |
| `0x02037fdc` | `OS_WaitIrq(clear, mask)` (wartet auf Flags bei `DTCM+0x3ff8`) |
| `0x0201e9c8` | Frame-Warten des Spiels: `OS_WaitIrq(1, VCount)` |
| `0x01ff8d70` | `OS_IrqHandler` (ITCM): sucht höchstes gesetztes IF-Bit, ruft Tabelle `0x027e007c` |
| `0x01ff8a38` | VCount-Handler: setzt `DTCM+0x3ff8 |= 4` |
| `0x01ff8dd8` | Thread-Wechsel nach Interrupt (`OS_IrqHandler_ThreadSwitch`) |

## Nächster großer Baustein: Thread-Modell

Das Spiel benutzt die Threads des NitroSDK. Ein Thread-Wechsel speichert auf dem DS alle
Register und springt in einen anderen Thread. In recompiliertem Code gehört zu jedem Thread
aber auch ein **C-Aufrufstapel**. Geplanter Ansatz:

- Jeder SDK-Thread bekommt einen eigenen Host-Fiber/Coroutine-Stack (Windows-Fibers bzw.
  `ucontext`), mit eigenem CPU-Kontext.
- Die SDK-Funktionen für Threads (`OS_CreateThread`, `OS_SleepThread`, `OS_WakeupThread`,
  `OS_RescheduleThread`, `OS_ExitThread`) und der Thread-Wechsel nach Interrupts werden
  durch Host-Code ersetzt (HLE), der zwischen den Fibers umschaltet.
- Ein Leerlauf-Fiber übernimmt das „Warten auf Interrupt“ und treibt den Frame-Takt.

## Hardware-Ersatz in Kategorien

Jeder Ersatz in `boot_test.c` ist einer von drei Kategorien zugeordnet und wird gezählt.
Der Bericht listet am Ende auf, welcher Ersatz wie oft benutzt wurde:

- **ORIGINAL**: verhält sich wie die echte Hardware (Hardware-Divider und -Wurzel,
  DMA sofort, BIOS Div/Sqrt/CpuSet/LZ77/RL).
- **SIMULIERT**: bewusst vereinfachte Antwort, damit der Code weiterläuft (ARM7-Handshake,
  ARM7-Kanäle bereit, Timer/VCOUNT pro Lesezugriff, Thread-Schlafen = Frame, CP15/CPSR).
  **Mögliche Fehlerquelle**, wenn sich das Spiel später seltsam verhält.
- **FEHLT**: nicht implementiert (IPC-FIFO senden/empfangen, also alle Nachrichten an
  den ARM7, verzögerte DMA, 3D-Grafik, Karten-Datenport, ungemappter Speicher).

Langfristig soll aus „SIMULIERT“ für den ARM7 ein richtiger ARM7-Ersatz werden, der die
PXI-Nachrichten des SDK versteht (Sound, Touch, Mikrofon, Uhr, Power, Dateisystem).
