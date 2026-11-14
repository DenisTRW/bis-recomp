/* Schnittstelle zwischen Plattform-Schicht (boot_test.c) und spielspezifischem HLE. */
#pragma once
#include "recomp.h"

/* Kategorien für jeden Hardware-/SDK-Ersatz, siehe docs/boot.md */
typedef enum { ORIGINAL, SIMULIERT, FEHLT } HleCat;
void hle_hit(HleCat cat, const char *what);
#define HLE(cat, what) hle_hit(cat, what)

/* Beendet den Lauf aus jedem Thread heraus (Bericht folgt). Kehrt nicht zurück. */
void platform_stop(const char *why);

/* Kein Thread ist bereit: Zeit vergeht bis zum nächsten Interrupt (ein Frame). */
void platform_idle(CPU *c);

/* Gibt es an dieser DS-Adresse eine recompilierte Funktion? */
int platform_is_function(uint32_t addr);

/* Vom IRQ-Thread-Wechsel gesetzt: an einem sicheren Punkt neu planen */
extern int platform_resched_pending;

/* Schlüssel des Start-Threads, bis er sich per OS_SaveContext an seine
 * SDK-Thread-Struktur bindet */
#define BOOT_THREAD_KEY 0x00000001u
