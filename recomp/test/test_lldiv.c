/* Test: 64-Bit-Division der Laufzeitbibliothek (main 0x02045164-0x02045774).
 *
 * Diese Routinen haben mehrere Einstiegspunkte, die sich einen Rumpf teilen:
 *   0x02045164  signierter Rest     (r4 Bit 0 = 1)
 *   0x02045174  signierter Quotient (zweiter Einstieg, nur per "bl .L_02045174" erreichbar)
 *   0x02045324  vorzeichenloser Quotient, springt für große Werte in den Rumpf von 0x02045164
 *   0x02045578  32-Bit vorzeichenlos (Einstieg 0x02045580 ohne Null-Prüfung)
 * Früher übersetzte der Recompiler die Sprünge in fremde Funktionen nicht (RC_TODO), dadurch
 * waren die Projektionsmatrizen der 3D-Grafik kaputt. Geprüft wird gegen C-Arithmetik
 * mit Zufallswerten aller Größenordnungen; Stackpointer und r4-r11 müssen erhalten bleiben.
 */
#include <stdio.h>
#include <string.h>
#include "harness.h"
#include "funcs.h"

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t rnd64(void) { return next() >> (next() % 64); }   /* alle Größenordnungen */

typedef void (*Entry)(CPU *c);
static int fails;

static uint64_t call64(int which, uint64_t n, uint64_t d) {
    CPU c;
    memset(&c, 0, sizeof c);
    c.r[0] = (uint32_t)n; c.r[1] = (uint32_t)(n >> 32);
    c.r[2] = (uint32_t)d; c.r[3] = (uint32_t)(d >> 32);
    for (int r = 4; r <= 11; r++) c.r[r] = 0x40000000u + (uint32_t)r;
    c.r[13] = HARNESS_STACK_TOP;
    c.r[14] = 0xdeadbeefu;
    if (which == 0) rc_func_02045164(&c);
    else if (which == 1) rc_func_02045164__at(&c, 0x02045174u);
    else rc_func_02045324(&c);
    if (c.r[13] != HARNESS_STACK_TOP) { if (fails < 8) printf("Stackpointer falsch (Variante %d)\n", which); fails++; }
    for (int r = 4; r <= 11; r++)
        if (c.r[r] != 0x40000000u + (uint32_t)r) { if (fails < 8) printf("r%d nicht erhalten (Variante %d)\n", r, which); fails++; break; }
    return c.r[0] | (uint64_t)c.r[1] << 32;
}

int main(void) {
    const char *name[3] = { "signierter Rest", "signierter Quotient", "vorzeichenloser Quotient" };
    int n_cases = 0;
    for (int i = 0; i < 200000; i++) {
        uint64_t un = rnd64(), ud = rnd64();
        if (next() & 1) un = (uint64_t)-(int64_t)un;
        if (next() & 1) ud = (uint64_t)-(int64_t)ud;
        if (ud == 0) continue;
        int64_t sn = (int64_t)un, sd = (int64_t)ud;
        if (sn == INT64_MIN && sd == -1) continue;
        uint64_t want[3] = { (uint64_t)(sn % sd), (uint64_t)(sn / sd), un / ud };
        for (int w = 0; w < 3; w++) {
            uint64_t got = call64(w, un, ud);
            if (got != want[w]) {
                if (fails < 8)
                    printf("%s: %016llx / %016llx = %016llx, erwartet %016llx\n", name[w],
                           (unsigned long long)un, (unsigned long long)ud, (unsigned long long)got, (unsigned long long)want[w]);
                fails++;
            }
        }
        n_cases++;
    }
    /* 32 Bit vorzeichenlos: Quotient in r0, Rest in r1 */
    for (int i = 0; i < 200000; i++) {
        uint32_t n = (uint32_t)rnd64(), d = (uint32_t)rnd64();
        if (!d) continue;
        CPU c;
        memset(&c, 0, sizeof c);
        c.r[0] = n; c.r[1] = d; c.r[13] = HARNESS_STACK_TOP; c.r[14] = 0xdeadbeefu;
        rc_func_02045578(&c);
        if (c.r[0] != n / d || c.r[1] != n % d) {
            if (fails < 8) printf("32 Bit: %08x / %08x = %08x Rest %08x\n", n, d, c.r[0], c.r[1]);
            fails++;
        }
    }
    printf("64-Bit-Division: %d Faelle x 3 Einstiege + 32 Bit, %d Fehler, %u TODOs\n", n_cases, fails, harness_todo_count);
    printf(fails == 0 && harness_todo_count == 0 ? "OK\n" : "FEHLGESCHLAGEN\n");
    return fails != 0;
}
