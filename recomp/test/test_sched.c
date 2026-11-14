/* Einzeltests für den Fiber-Scheduler (ohne Spielcode).
 *
 *  1. Ping-Pong:      2 Threads, 2000 Wechsel; Register + lokale Stack-Daten bleiben erhalten
 *  2. Start/Ende:     neuer Thread bekommt seinen Startzustand; nach Rückkehr geht es
 *                     beim Aufrufer weiter
 *  3. Interrupt:      Handler zerstört alle Register -> danach wieder exakt wie vorher;
 *                     Wechsel im Interrupt wird als Verstoß erkannt
 *  4. Determinismus:  gleicher Seed -> gleiche Trace-Prüfsumme, anderer Seed -> andere
 *  5. Tiefe Rekursion: 50 000 Ebenen tief, mittendrin Wechsel, Stack bleibt intakt
 *  6. Prüfungen prüfen: absichtlich beschädigter Kontext/Kanarienvogel wird erkannt
 */
#include <stdio.h>
#include <string.h>
#include "sched.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("    FEHLER: " __VA_ARGS__); printf("\n"); } } while (0)

static CPU cpu;

/* Register mit einem thread-eigenen Muster füllen / prüfen */
static void fill_regs(CPU *c, uint32_t seed) {
    for (int i = 0; i < 16; i++) c->r[i] = seed * 0x01000193u + (uint32_t)i;
    c->n = seed & 1; c->z = (seed >> 1) & 1; c->cf = (seed >> 2) & 1; c->v = (seed >> 3) & 1;
    c->cpsr_ctrl = 0x1f ^ seed;
}
static int regs_ok(const CPU *c, uint32_t seed) {
    CPU want; memset(&want, 0, sizeof want); fill_regs(&want, seed);
    return memcmp(c, &want, sizeof want) == 0;
}

/* ---------------------------------------------------------------- 1. Ping-Pong */

#define PP_ROUNDS 1000
static void pp_thread(CPU *c, void *arg) {
    uint32_t me = (uint32_t)(uintptr_t)arg, other = me == 0xA ? 0xB : 0xA;
    volatile uint32_t local[64];
    for (int i = 0; i < 64; i++) local[i] = me * 1000u + (uint32_t)i;
    for (uint32_t round = 0; round < PP_ROUNDS; round++) {
        fill_regs(c, me * 100000u + round);
        sched_switch(c, other, pp_thread, (void *)(uintptr_t)other);
        if (sched_finished(other)) break;
        CHECK(regs_ok(c, me * 100000u + round), "Ping-Pong: Register von %X nach Runde %u falsch", me, round);
        int ok = 1;
        for (int i = 0; i < 64; i++) ok &= local[i] == me * 1000u + (uint32_t)i;
        CHECK(ok, "Ping-Pong: lokaler Stack von %X beschaedigt (Runde %u)", me, round);
        if (!ok || failures > 5) break;
    }
}

static void test_pingpong(void) {
    printf("1. Ping-Pong\n");
    sched_run(&cpu, 0xA, pp_thread, (void *)(uintptr_t)0xA);
    CHECK(sched_trace_count() >= 2 * PP_ROUNDS, "zu wenige Wechsel: %llu", (unsigned long long)sched_trace_count());
}

/* ---------------------------------------------------------------- 2. Start/Ende */

static int child_ran;
static void child(CPU *c, void *arg) {
    /* frischer Thread startet mit leerem Kontext; setzt dann seinen Startzustand */
    CHECK(c->r[0] == 0 && c->r[13] == 0, "neuer Thread startet nicht mit leerem Kontext");
    c->r[0] = (uint32_t)(uintptr_t)arg;
    c->r[13] = 0x02380000u;
    child_ran = c->r[0] == 0x1234;
}
static void parent(CPU *c, void *arg) {
    (void)arg;
    fill_regs(c, 77);
    sched_switch(c, 0xC1, child, (void *)(uintptr_t)0x1234);
    CHECK(child_ran, "Kind-Thread lief nicht");
    CHECK(sched_finished(0xC1), "Kind-Thread nicht als beendet markiert");
    CHECK(regs_ok(c, 77), "Register des Eltern-Threads nach Ende des Kindes falsch");
    /* Wiederverwendung desselben Schlüssels */
    child_ran = 0;
    sched_switch(c, 0xC1, child, (void *)(uintptr_t)0x1234);
    CHECK(child_ran, "wiederverwendeter Schluessel startet nicht neu");
    CHECK(regs_ok(c, 77), "Register nach zweitem Kind falsch");
}
static void test_start_exit(void) {
    printf("2. Start/Ende\n");
    sched_run(&cpu, 0xF0, parent, NULL);
}

/* ---------------------------------------------------------------- 3. Interrupt */

static void trash_handler(CPU *c, void *arg) {
    (void)arg;
    for (int i = 0; i < 16; i++) c->r[i] = 0xdead0000u + (uint32_t)i;
    c->n = c->z = c->cf = c->v = 1;
}
static void bad_handler(CPU *c, void *arg) {
    (void)arg;
    sched_switch(c, 0xE2, trash_handler, NULL);   /* verboten: Wechsel im Interrupt */
}
static void irq_thread(CPU *c, void *arg) {
    (void)arg;
    fill_regs(c, 555);
    sched_irq(c, 1, trash_handler, NULL);
    CHECK(regs_ok(c, 555), "Register nach Interrupt nicht wiederhergestellt");
    unsigned before = sched_violations;
    sched_irq(c, 1, bad_handler, NULL);
    CHECK(sched_violations == before + 1, "Wechsel im Interrupt wurde nicht erkannt");
    sched_violations = before;   /* erwarteter Verstoß */
}
static void test_irq(void) {
    printf("3. Interrupt\n");
    sched_run(&cpu, 0xE1, irq_thread, NULL);
}

/* ---------------------------------------------------------------- 4. Determinismus */

static uint32_t lcg;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

static void det_worker(CPU *c, void *arg) {
    uint32_t me = (uint32_t)(uintptr_t)arg;
    for (int step = 0; step < 300; step++) {
        c->r[4] += me;
        uint32_t next = 0xD0 + rnd() % 4;          /* pseudozufälliges, aber reproduzierbares Ziel */
        if (next == me) { sched_trace(TR_USER, me, c->r[4]); continue; }
        sched_switch(c, next, det_worker, (void *)(uintptr_t)next);
    }
    sched_stop(c);
}
static uint64_t det_run(uint32_t seed) {
    lcg = seed;
    sched_run(&cpu, 0xD0, det_worker, (void *)(uintptr_t)0xD0);
    return sched_trace_hash();
}
static void test_determinism(void) {
    printf("4. Determinismus\n");
    uint64_t a = det_run(42), b = det_run(42), d = det_run(43);
    printf("    Seed 42: %016llx, nochmal: %016llx, Seed 43: %016llx\n",
           (unsigned long long)a, (unsigned long long)b, (unsigned long long)d);
    CHECK(a == b, "gleicher Ablauf liefert verschiedene Prüfsummen");
    CHECK(a != d, "verschiedene Abläufe liefern dieselbe Prüfsumme");
}

/* ---------------------------------------------------------------- 5. Tiefe Rekursion */

static int deep_ok = 1;
static void deep(CPU *c, int depth, int switch_at) {
    volatile uint32_t frame[8];
    for (int i = 0; i < 8; i++) frame[i] = (uint32_t)depth * 8u + (uint32_t)i;
    if (depth == switch_at) {
        fill_regs(c, 999);
        sched_switch(c, 0x52, trash_handler, NULL);   /* anderer Thread läuft und endet */
        if (!regs_ok(c, 999)) deep_ok = 0;
    }
    if (depth < 50000) deep(c, depth + 1, switch_at);
    for (int i = 0; i < 8; i++) if (frame[i] != (uint32_t)depth * 8u + (uint32_t)i) deep_ok = 0;
}
static void deep_thread(CPU *c, void *arg) { (void)arg; deep(c, 0, 40000); }
static void test_deep(void) {
    printf("5. Tiefe Rekursion\n");
    sched_run(&cpu, 0x51, deep_thread, NULL);
    CHECK(deep_ok, "Stack nach tiefer Rekursion mit Wechsel beschaedigt");
}

/* ---------------------------------------------------------------- 6. Prüfungen prüfen */

static void victim(CPU *c, void *arg) {
    (void)arg;
    fill_regs(c, 31);
    sched_switch(c, 0x61, NULL, NULL);     /* zurück zum Angreifer */
}
static void attacker(CPU *c, void *arg) {
    (void)arg;
    sched_switch(c, 0x62, victim, NULL);   /* Opfer läuft an und schläft */
    unsigned before = sched_violations;
    sched_debug_corrupt_slot(0x62);
    sched_check_all();
    CHECK(sched_violations == before + 1, "beschaedigter CPU-Kontext wurde nicht erkannt");
    sched_debug_corrupt_slot(0x62);        /* zurückdrehen (XOR) */
    sched_debug_corrupt_canary(0x62);
    sched_check_all();
    CHECK(sched_violations == before + 2, "beschaedigter Stack-Kanarienvogel wurde nicht erkannt");
    sched_debug_corrupt_canary(0x62);
    sched_check_all();
    CHECK(sched_violations == before + 2, "Fehlalarm nach Reparatur");
    sched_violations = before;   /* erwartete Verstöße */
}
static void test_self_check(void) {
    printf("6. Pruefungen pruefen\n");
    sched_run(&cpu, 0x61, attacker, NULL);
}

/* ---------------------------------------------------------------- main */

int main(void) {
    test_pingpong();
    test_start_exit();
    test_irq();
    test_determinism();
    test_deep();
    test_self_check();
    if (sched_violations) { printf("Unerwartete Scheduler-Verstoesse: %u\n", sched_violations); failures++; }
    printf(failures ? "FEHLGESCHLAGEN (%d)\n" : "OK – alle Scheduler-Tests bestanden\n", failures);
    return failures != 0;
}
