/* Fiber-Scheduler für recompilierte Threads.
 *
 * Jeder Thread (identifiziert über einen 32-Bit-Schlüssel, z. B. die Adresse der
 * SDK-Thread-Struktur) bekommt einen eigenen Host-Fiber mit eigenem C-Stack.
 * Alle Fibers teilen sich EINEN CPU-Kontext (CPU *c), so wie der recompilierte
 * Code ihn erwartet. Beim Wechsel wird *c im Slot des alten Threads gesichert und
 * beim Fortsetzen wiederhergestellt.
 *
 * Der Scheduler entscheidet nichts selbst: WER läuft, bestimmt der Aufrufer
 * (im Spiel: der Original-Code von OS_SelectThread). Er führt nur aus und protokolliert.
 *
 * Determinismus: Der Trace enthält nur Schlüssel und Zähler, keine Host-Adressen
 * oder Zeiten. Gleicher Ablauf -> gleiche Trace-Prüfsumme.
 *
 * Selbstprüfung bei jedem Fortsetzen:
 *   - Kanarienvogel am Boden jedes Fiber-Stacks (erkennt Stack-Beschädigung)
 *   - Prüfsumme über den gesicherten CPU-Kontext (erkennt Beschädigung des Slots)
 *   - Fortsetzen eines beendeten Threads
 * Verstöße werden gezählt (sched_violations) und mit Text gemeldet.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "recomp.h"

typedef void (*SchedEntry)(CPU *c, void *arg);

enum {
    TR_CREATE = 1,  /* a = neuer Thread */
    TR_SWITCH,      /* a = von, b = nach */
    TR_EXIT,        /* a = beendeter Thread, b = Rückkehr zu */
    TR_IRQ,         /* a = Interrupt-Bits, b = Thread */
    TR_TICK,        /* a = Frame-Nummer */
    TR_WAKE,        /* a = geweckter Thread */
    TR_USER,        /* frei für Tests/Spiel-HLE */
    TR_BIND,        /* a = alter Schlüssel, b = neuer Schlüssel */
};

/* Führt entry(c, arg) als ersten Thread (Schlüssel main_key) aus und kehrt
 * zurück, wenn dieser Thread endet oder sched_stop() aufgerufen wird.
 * Danach ist der Scheduler wieder leer (alle Fibers gelöscht). */
void sched_run(CPU *c, uint32_t main_key, SchedEntry entry, void *arg);

/* Wechselt zum Thread `key`. Existiert er noch nicht, wird er angelegt und
 * startet mit entry(c, arg). Der aufrufende Thread schläft, bis jemand zu ihm
 * zurückwechselt; dann ist *c genau wie vor dem Aufruf. */
void sched_switch(CPU *c, uint32_t key, SchedEntry entry, void *arg);

/* Beendet sched_run von jedem Thread aus (z. B. Wachhund). Kehrt nicht zurück. */
void sched_stop(CPU *c);

/* Läuft handler(c, arg) als "Interrupt" im aktuellen Thread: *c wird vorher
 * gesichert und danach wiederhergestellt (wie bei einem echten IRQ). */
void sched_irq(CPU *c, uint32_t bits, SchedEntry handler, void *arg);

/* Gibt dem laufenden Thread einen neuen Schlüssel (z. B. wenn der Start-Thread
 * erfährt, welche SDK-Thread-Struktur er ist). */
void sched_rebind_current(uint32_t key);

uint32_t sched_current(void);
int sched_exists(uint32_t key);
int sched_finished(uint32_t key);
unsigned sched_thread_count(void);

/* Trace */
void sched_trace(int type, uint32_t a, uint32_t b);
uint64_t sched_trace_hash(void);
uint64_t sched_trace_count(void);
void sched_trace_dump(FILE *f, unsigned last_n);
/* wie oben, aber nur Thread-Ereignisse (ohne TICK/IRQ/WAKE), die letzten n davon */
void sched_trace_dump_filtered(FILE *f, unsigned last_n);

/* Selbstprüfung */
extern unsigned sched_violations;
void sched_check_all(void);          /* prüft alle schlafenden Threads */

/* Nur für Tests: absichtlich beschädigen, um die Prüfungen zu prüfen */
void sched_debug_corrupt_slot(uint32_t key);
void sched_debug_corrupt_canary(uint32_t key);
