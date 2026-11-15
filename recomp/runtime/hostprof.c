/* Zeit-Profiler für die Fehlersuche (nur Windows): BIS_HOSTPROF=datei
 *
 * Ein Hilfs-Thread hält den Hauptthread etwa 1000-mal pro Sekunde kurz an und merkt sich,
 * an welcher Stelle des PC-Programms er gerade war (Befehlszeiger). Am Ende stehen die
 * Adressen mit ihrer Häufigkeit in der Datei; recomp/tools/hostprof.py ordnet sie mit nm
 * den Funktionen zu. Anders als das Zugriffs-Profil misst das echte Rechenzeit auf dem PC.
 * Ändert nichts am Ablauf des Spiels. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

volatile int hostprof_active;   /* boot_test.c: ab BIS_PROFILE_FROM */

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>

#define HP_SLOTS (1 << 20)
static uint64_t *hp_addr;
static uint32_t *hp_n;
static volatile LONG hp_run;
static HANDLE hp_main, hp_thread;
static uint64_t hp_total;

static void hp_add(uint64_t a) {
    uint32_t h = (uint32_t)((a * 0x9e3779b97f4a7c15ull) >> 44) & (HP_SLOTS - 1);
    for (;;) {
        if (hp_addr[h] == a) { hp_n[h]++; return; }
        if (!hp_addr[h]) { hp_addr[h] = a; hp_n[h] = 1; return; }
        h = (h + 1) & (HP_SLOTS - 1);
    }
}

static DWORD WINAPI hp_loop(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    while (hp_run) {
        Sleep(1);
        if (SuspendThread(hp_main) == (DWORD)-1) break;
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (hostprof_active && GetThreadContext(hp_main, &ctx)) { hp_add(ctx.Rip); hp_total++; }
        ResumeThread(hp_main);
    }
    return 0;
}

void hostprof_start(void) {
    if (!getenv("BIS_HOSTPROF")) return;
    hp_addr = calloc(HP_SLOTS, sizeof *hp_addr);
    hp_n = calloc(HP_SLOTS, sizeof *hp_n);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &hp_main, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    hp_run = 1;
    hp_thread = CreateThread(NULL, 0, hp_loop, NULL, 0, NULL);
}

void hostprof_stop(void) {
    if (!hp_thread) return;
    hp_run = 0;
    WaitForSingleObject(hp_thread, INFINITE);
    hp_thread = NULL;
    FILE *f = fopen(getenv("BIS_HOSTPROF"), "w");
    if (!f) return;
    /* Basisadresse des Programms, damit hostprof.py auch mit ASLR die Adressen zuordnen kann */
    fprintf(f, "base %llx\n", (unsigned long long)(uintptr_t)GetModuleHandle(NULL));
    HMODULE self = GetModuleHandle(NULL);
    for (uint32_t i = 0; i < HP_SLOTS; i++) {
        if (!hp_addr[i]) continue;
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)hp_addr[i], &m);
        if (m && m != self) {   /* Stichprobe in einer DLL: Modulname statt Adresse */
            GetModuleFileNameA(m, name, sizeof name);
            const char *base = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
            fprintf(f, "dll:%s %u\n", base, hp_n[i]);
        } else fprintf(f, "%llx %u\n", (unsigned long long)hp_addr[i], hp_n[i]);
    }
    fclose(f);
    fprintf(stderr, "Zeit-Profil: %llu Stichproben in %s\n", (unsigned long long)hp_total, getenv("BIS_HOSTPROF"));
}
#else
void hostprof_start(void) {}
void hostprof_stop(void) {}
#endif
